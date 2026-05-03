#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Misc/CoreDelegates.h"
#include "SpeechToTextTypes.h"
#include "AudioCaptureHandler.h"
#include "WhisperAPIClient.h"
#include "SpeechToTextComponent.generated.h"

class UNPCConversationComponent;

/**
 * Actor component for Vietnamese speech-to-text transcription.
 * Attach to any Actor, call StartRecording/StopRecording, and listen
 * to OnTranscriptionComplete for the transcribed text.
 *
 * Supports two modes:
 * - Push-to-talk: user triggers Start, speaks, then triggers Stop.
 * - Hands-free (Energy VAD): microphone continuously listens, automatically
 *   detects speech using RMS energy thresholding.
 *
 * Audio is sent to OpenAI Whisper API and the result fires via delegate.
 */
UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent), BlueprintType)
class SPEECHTOTEXT_API USpeechToTextComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USpeechToTextComponent();
	~USpeechToTextComponent();

	// --- Blueprint-callable functions ---

	/** Begin recording from the microphone (push-to-talk mode). */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text")
	void StartRecording();

	/** Stop recording and send audio to Whisper API for transcription (push-to-talk mode). */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text")
	void StopRecording();

	/** Enable hands-free mode: mic listens continuously, auto-detects speech. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Hands-Free")
	void EnableHandsFreeMode();

	/** Disable hands-free mode and return to idle. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Hands-Free")
	void DisableHandsFreeMode();

	/** Is hands-free (VAD) mode currently active? */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Speech To Text|Hands-Free")
	bool IsHandsFreeMode() const { return bHandsFreeMode; }

	/** When false, skip local energy-based VAD and rely entirely on Google's server-side VAD
	 *  for turn detection (matches test-google-ai-studio behavior). Audio streams continuously
	 *  from the moment hands-free mode is enabled.
	 *  Only effective when Stream Direct Audio is on — Whisper mode always uses local VAD
	 *  because it needs to know when to stop recording before uploading the WAV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Hands-Free",
	          meta = (DisplayName = "Use Local Energy VAD"))
	bool bUseLocalVAD = true;

	/** Get the current microphone RMS energy level (0.0-1.0). Useful for UI volume meters. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Speech To Text|Hands-Free")
	float GetMicrophoneEnergy() const;

	/** Get the current state of the speech-to-text system. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Speech To Text")
	ESpeechToTextState GetCurrentState() const { return CurrentState; }

	/** Get the name of the active audio capture device. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Speech To Text|Debug")
	FString GetCaptureDeviceName() const;

	/** Get the current recording duration in seconds (live during recording). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Speech To Text|Debug")
	float GetRecordingDuration() const;

	// --- Debug ---

	/** When true, saves each recorded WAV to Saved/SpeechToText/ for debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Debug", meta = (DisplayName = "Save Debug WAV Files"))
	bool bSaveDebugWAV = false;

	// --- Direct Audio Streaming (GeminiLive) ---

	/** When true, stream audio directly to an NPCConversationComponent instead of
	 *  sending to Whisper for transcription. This enables ultra-low-latency
	 *  voice interaction by skipping the STT step entirely.
	 *  Requires the target NPC to have bUseDirectAudioInput enabled and ChatBackend = GeminiLive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Direct Audio", meta = (DisplayName = "Stream Direct Audio"))
	bool bStreamDirectAudio = false;

	/** The NPCConversationComponent to stream audio to when bStreamDirectAudio is enabled.
	 *  If null, will attempt to find one on the same actor or the player pawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Direct Audio", meta = (DisplayName = "Target NPC Conversation", EditCondition = "bStreamDirectAudio", EditConditionHides))
	UNPCConversationComponent* TargetNPCConversation = nullptr;

	/** Set the target NPCConversationComponent for direct audio streaming at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Direct Audio")
	void SetTargetNPCConversation(UNPCConversationComponent* InTarget) { TargetNPCConversation = InTarget; }

	// --- Delegates ---

	/** Fired when transcription completes (success or failure). */
	UPROPERTY(BlueprintAssignable, Category = "Speech To Text")
	FOnTranscriptionComplete OnTranscriptionComplete;

	/** Fired when the recording state changes. */
	UPROPERTY(BlueprintAssignable, Category = "Speech To Text")
	FOnRecordingStateChanged OnRecordingStateChanged;

	// --- Per-Instance Overrides ---
	// These override the values from Project Settings (Edit → Project Settings → Plugins → Speech To Text).
	// Leave empty/zero to use the project-wide defaults.

	/** Per-instance API key override. Leave empty to use the key from Project Settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "API Key Override"))
	FString APIKeyOverride;

	/** Per-instance language override. Leave empty to use default from Project Settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "Language Override"))
	FString LanguageOverride;

	/** Per-instance max duration override. 0 = use Project Settings default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "Max Duration Override", ClampMin = "0.0", ClampMax = "120.0", Units = "s"))
	float MaxRecordingDurationOverride = 0.0f;

	/** Per-instance sample rate override. 0 = use Project Settings default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "Sample Rate Override", ClampMin = "0", ClampMax = "48000", Units = "Hz"))
	int32 SampleRateOverride = 0;

	/** Per-instance model override. Leave empty to use default from Project Settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "Model Override"))
	FString ModelOverride;

	/** Per-instance prompt override. Leave empty to use default from Project Settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Overrides", meta = (DisplayName = "Prompt Override"))
	FString PromptOverride;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void SetState(ESpeechToTextState NewState);
	void OnTranscriptionResult(const FSpeechToTextResult& Result);
	void ProcessAndSendAudio();

	// Energy-based VAD: speech detection via RMS threshold
	void ProcessEnergyVAD(float DeltaTime);

	FString ResolveAPIKey() const;
	FString ResolveLanguage() const;
	FString ResolveModel() const;
	FString ResolvePrompt() const;
	float ResolveMaxRecordingDuration() const;
	int32 ResolveSampleRate() const;

	// Android microphone permission handling
	enum class EPendingMicAction : uint8 { None, StartRecording, EnableHandsFree };
	EPendingMicAction PendingMicAction = EPendingMicAction::None;

	/** Returns true if permission is already granted; false if a request was fired (caller should return early). */
	bool EnsureMicPermission(EPendingMicAction Action);

	UFUNCTION()
	void OnMicPermissionResult(const TArray<FString>& Permissions, const TArray<bool>& GrantResults);

	/** Handle Android lifecycle: re-initialize mic after ON_STOP destroys the Oboe stream. */
	void OnAppReactivated();

	/** Re-open the audio capture stream (used after Android lifecycle destroys it). */
	void ReinitializeAudioCapture();

	TUniquePtr<FAudioCaptureHandler> CaptureHandler;
	TUniquePtr<FWhisperAPIClient> WhisperClient;

	ESpeechToTextState CurrentState = ESpeechToTextState::Idle;
	float RecordingTimer = 0.0f;
	bool bHandsFreeMode = false;
	bool bPendingReinit = false;
	float ReinitDelayTimer = 0.0f;
	FDelegateHandle AppReactivatedHandle;

	// Energy VAD state
	float EnergyThreshold = 0.015f;
	int32 MinSpeechDurationMs = 250;
	int32 SilenceDurationMs = 1500;
	float ConsecutiveSpeechMs = 0.0f;
	float ConsecutiveSilenceMs = 0.0f;
	bool bEnergyVADSpeechActive = false;
	float VADLogTimer = 0.0f;

	// Direct audio streaming state
	bool bDirectAudioStreamActive = false;
	float LazyStreamOpenRetryTimer = 0.0f;

	/** Convert float samples [-1.0, 1.0] to 16-bit PCM bytes. */
	static TArray<uint8> ConvertFloatToPCM16(const TArray<float>& FloatSamples);

	/** Find the target NPC conversation component (uses TargetNPCConversation or searches). */
	UNPCConversationComponent* ResolveTargetNPC() const;

	/** Stream audio chunk to the target NPC. */
	void StreamAudioToNPC(const TArray<float>& FloatSamples);

	/** Resample mono float samples from InputRate -> 16000 Hz using linear interpolation.
	 *  Stateful across calls: preserves the fractional read position and last source
	 *  sample so that chunks drained each tick stitch together without gaps or pitch drift.
	 *  Matches the algorithm in test-google-ai-studio/audio-processor.js. */
	void ResampleTo16k(const TArray<float>& InMono, int32 InputRate, TArray<float>& OutMono);

	/** Reset resampler state (call on stream start). */
	void ResetResamplerState();

	// Resampler state for StreamAudioToNPC
	double ResamplePos = 0.0;
	float  ResamplePrevSample = 0.0f;
	bool   bResampleHasPrev = false;
};
