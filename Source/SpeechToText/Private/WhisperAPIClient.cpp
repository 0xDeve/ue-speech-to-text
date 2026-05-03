#include "WhisperAPIClient.h"
#include "Http.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

static const FString WhisperAPIEndpoint = TEXT("https://api.openai.com/v1/audio/transcriptions");

FWhisperAPIClient::~FWhisperAPIClient()
{
	CancelRequest();
}

void FWhisperAPIClient::CancelRequest()
{
	if (PendingRequest.IsValid())
	{
		PendingRequest->OnProcessRequestComplete().Unbind();
		PendingRequest->CancelRequest();
		PendingRequest.Reset();
	}
}

void FWhisperAPIClient::SendTranscriptionRequest(
	const TArray<uint8>& WAVData,
	const FString& APIKey,
	const FString& Language,
	const FString& Model,
	const FString& Prompt,
	float AudioDuration,
	FTranscriptionCallback OnComplete)
{
	if (APIKey.IsEmpty())
	{
		FSpeechToTextResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("API key is empty. Set your OpenAI API key.");
		if (OnComplete) { OnComplete(Result); }
		return;
	}

	if (WAVData.Num() == 0)
	{
		FSpeechToTextResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("No audio data to transcribe.");
		if (OnComplete) { OnComplete(Result); }
		return;
	}

	const FString Boundary = FString::Printf(TEXT("UnrealBoundary%08X"), FMath::Rand());

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(WhisperAPIEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *APIKey));
	Request->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));

	TArray<uint8> Body = BuildMultipartBody(WAVData, Language, Model, Prompt, Boundary);
	Request->SetContent(Body);

	CancelRequest();

	PendingRequest = Request;

	Request->OnProcessRequestComplete().BindRaw(
		this,
		&FWhisperAPIClient::HandleHTTPResponse,
		OnComplete,
		AudioDuration,
		Language
	);

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Sending %.1f sec audio to %s (%.1f KB, lang=%s)"),
		AudioDuration, *Model, Body.Num() / 1024.0f, *Language);

	Request->ProcessRequest();
}

TArray<uint8> FWhisperAPIClient::BuildMultipartBody(
	const TArray<uint8>& WAVData,
	const FString& Language,
	const FString& Model,
	const FString& Prompt,
	const FString& Boundary)
{
	TArray<uint8> Body;

	auto AppendString = [&Body](const FString& Str)
	{
		const FTCHARToUTF8 Converter(*Str);
		Body.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	};

	// File part
	AppendString(FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendString(TEXT("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"));
	AppendString(TEXT("Content-Type: audio/wav\r\n\r\n"));
	Body.Append(WAVData);
	AppendString(TEXT("\r\n"));

	// Model part
	AppendString(FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendString(TEXT("Content-Disposition: form-data; name=\"model\"\r\n\r\n"));
	AppendString(FString::Printf(TEXT("%s\r\n"), *Model));

	// Language part
	AppendString(FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendString(TEXT("Content-Disposition: form-data; name=\"language\"\r\n\r\n"));
	AppendString(FString::Printf(TEXT("%s\r\n"), *Language));

	// Prompt part (guides language detection and transcription style)
	if (!Prompt.IsEmpty())
	{
		AppendString(FString::Printf(TEXT("--%s\r\n"), *Boundary));
		AppendString(TEXT("Content-Disposition: form-data; name=\"prompt\"\r\n\r\n"));
		AppendString(FString::Printf(TEXT("%s\r\n"), *Prompt));
	}

	// Response format part
	AppendString(FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendString(TEXT("Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"));
	AppendString(TEXT("json\r\n"));

	// End boundary
	AppendString(FString::Printf(TEXT("--%s--\r\n"), *Boundary));

	return Body;
}

void FWhisperAPIClient::HandleHTTPResponse(
	FHttpRequestPtr Request,
	FHttpResponsePtr Response,
	bool bConnectedSuccessfully,
	FTranscriptionCallback OnComplete,
	float AudioDuration,
	FString Language)
{
	PendingRequest.Reset();
	FSpeechToTextResult Result;
	Result.Language = Language;
	Result.AudioDuration = AudioDuration;

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Network error: Could not connect to OpenAI API. Check internet connection.");
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: %s"), *Result.ErrorMessage);
		if (OnComplete) { OnComplete(Result); }
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();

	if (ResponseCode != 200)
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("API error (HTTP %d): %s"), ResponseCode, *ResponseBody);
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: %s"), *Result.ErrorMessage);
		if (OnComplete) { OnComplete(Result); }
		return;
	}

	// Parse JSON response: { "text": "transcribed text here" }
	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to parse API response: %s"), *ResponseBody);
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: %s"), *Result.ErrorMessage);
		if (OnComplete) { OnComplete(Result); }
		return;
	}

	Result.TranscribedText = JsonObject->GetStringField(TEXT("text"));
	Result.bSuccess = true;

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Transcription successful: \"%s\""), *Result.TranscribedText);

	if (OnComplete)
	{
		OnComplete(Result);
	}
}
