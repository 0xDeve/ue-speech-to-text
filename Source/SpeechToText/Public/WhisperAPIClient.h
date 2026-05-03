#pragma once

#include "CoreMinimal.h"
#include "SpeechToTextTypes.h"
#include "Http.h"

/**
 * HTTP client for the OpenAI Whisper transcription API.
 * Sends WAV audio data and receives transcribed text.
 */
class SPEECHTOTEXT_API FWhisperAPIClient
{
public:
	using FTranscriptionCallback = TFunction<void(const FSpeechToTextResult& Result)>;

	~FWhisperAPIClient();

	/**
	 * Send audio data to OpenAI Whisper API for transcription.
	 * @param WAVData      - Audio data encoded as WAV bytes
	 * @param APIKey       - OpenAI API key
	 * @param Language     - Language code (default "vi" for Vietnamese)
	 * @param AudioDuration - Duration of the audio in seconds (for result metadata)
	 * @param OnComplete   - Callback fired on game thread when transcription completes
	 */
	void SendTranscriptionRequest(
		const TArray<uint8>& WAVData,
		const FString& APIKey,
		const FString& Language,
		const FString& Model,
		const FString& Prompt,
		float AudioDuration,
		FTranscriptionCallback OnComplete
	);

	/** Cancel any in-flight HTTP request. */
	void CancelRequest();

private:
	static TArray<uint8> BuildMultipartBody(
		const TArray<uint8>& WAVData,
		const FString& Language,
		const FString& Model,
		const FString& Prompt,
		const FString& Boundary
	);

	void HandleHTTPResponse(
		FHttpRequestPtr Request,
		FHttpResponsePtr Response,
		bool bConnectedSuccessfully,
		FTranscriptionCallback OnComplete,
		float AudioDuration,
		FString Language
	);

	FHttpRequestPtr PendingRequest;
};
