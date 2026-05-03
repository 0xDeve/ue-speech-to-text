#pragma once

#include "CoreMinimal.h"
#include "SpeechToTextTypes.generated.h"

UENUM(BlueprintType)
enum class ESpeechToTextState : uint8
{
	Idle        UMETA(DisplayName = "Idle"),
	Listening   UMETA(DisplayName = "Listening"),
	Recording   UMETA(DisplayName = "Recording"),
	Processing  UMETA(DisplayName = "Processing"),
	Error       UMETA(DisplayName = "Error")
};

USTRUCT(BlueprintType)
struct SPEECHTOTEXT_API FSpeechToTextResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Speech To Text")
	FString TranscribedText;

	UPROPERTY(BlueprintReadOnly, Category = "Speech To Text")
	FString Language;

	UPROPERTY(BlueprintReadOnly, Category = "Speech To Text")
	float AudioDuration = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Speech To Text")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "Speech To Text")
	FString ErrorMessage;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTranscriptionComplete, const FSpeechToTextResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRecordingStateChanged, ESpeechToTextState, NewState);
