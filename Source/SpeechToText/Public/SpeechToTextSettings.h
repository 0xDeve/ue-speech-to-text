#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SpeechToTextSettings.generated.h"

/**
 * Project-wide settings for the Speech To Text plugin.
 * Configure in: Edit → Project Settings → Plugins → Speech To Text
 * Values are saved to Config/DefaultSpeechToText.ini
 */
UCLASS(Config = SpeechToText, DefaultConfig, meta = (DisplayName = "Speech To Text"))
class SPEECHTOTEXT_API USpeechToTextSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USpeechToTextSettings();

	/** Get the singleton settings instance. */
	static const USpeechToTextSettings* Get();

	virtual FName GetCategoryName() const override;

	// --- API Configuration ---

	/** OpenAI API key for Whisper transcription. Leave empty to use OPENAI_API_KEY environment variable. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API", meta = (DisplayName = "OpenAI API Key"))
	FString APIKey;

	/** OpenAI transcription model. Examples: "whisper-1", "gpt-4o-transcribe", "gpt-4o-mini-transcribe". */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "API", meta = (DisplayName = "Transcription Model"))
	FString Model = TEXT("gpt-4o-mini-transcribe");

	// --- Default Recording Settings ---

	/** Default language code for transcription. "vi" = Vietnamese. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Defaults", meta = (DisplayName = "Default Language"))
	FString DefaultLanguage = TEXT("vi");

	/** Optional prompt to guide transcription. A prompt in the target language helps prevent misdetection. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Defaults", meta = (DisplayName = "Default Prompt"))
	FString DefaultPrompt = TEXT("Xin chào. Đây là cuộc trò chuyện bằng tiếng Việt.");

	/** Default max recording duration in seconds. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Defaults", meta = (DisplayName = "Max Recording Duration", ClampMin = "1.0", ClampMax = "120.0", Units = "s"))
	float DefaultMaxRecordingDuration = 30.0f;

	/** Default audio sample rate. 16000 Hz is optimal for Whisper. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Defaults", meta = (DisplayName = "Sample Rate", ClampMin = "8000", ClampMax = "48000", Units = "Hz"))
	int32 DefaultSampleRate = 16000;

	// --- Voice Activity Detection (Hands-Free Mode) ---

	/** Enable hands-free mode by default (auto-detect speech without button press). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Voice Activity Detection", meta = (DisplayName = "Enable Hands-Free Mode"))
	bool bDefaultHandsFreeMode = true;

	/** RMS energy threshold for speech detection. Audio above this level is considered speech.
	 *  Typical values: 0.01-0.03. Check logs for your mic's RMS levels. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Voice Activity Detection", meta = (DisplayName = "Energy Threshold", ClampMin = "0.001", ClampMax = "0.5"))
	float EnergyThreshold = 0.015f;

	/** Minimum consecutive milliseconds of detected speech before confirming speech onset. Prevents brief noises from triggering. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Voice Activity Detection", meta = (DisplayName = "Min Speech Duration", ClampMin = "100", ClampMax = "2000", Units = "ms"))
	int32 VADMinSpeechDurationMs = 250;

	/** Milliseconds of silence after speech before finalizing recording. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Voice Activity Detection", meta = (DisplayName = "Silence Duration", ClampMin = "200", ClampMax = "5000", Units = "ms"))
	int32 VADSilenceDurationMs = 1500;

	/** Seconds of audio to keep before speech onset so the beginning of words is not clipped. Should be >= MinSpeechDuration. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Voice Activity Detection", meta = (DisplayName = "Pre-Roll Duration", ClampMin = "0.0", ClampMax = "2.0", Units = "s"))
	float VADPreRollSeconds = 0.8f;
};
