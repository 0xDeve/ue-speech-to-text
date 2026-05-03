#include "SpeechToTextComponent.h"
#include "SpeechToTextModule.h"
#include "SpeechToTextSettings.h"
#include "AudioCaptureHandler.h"
#include "WhisperAPIClient.h"
#include "NPCConversationComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

#if PLATFORM_ANDROID
#include "AndroidPermissionFunctionLibrary.h"
#include "AndroidPermissionCallbackProxy.h"
#endif

USpeechToTextComponent::USpeechToTextComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

USpeechToTextComponent::~USpeechToTextComponent() = default;

void USpeechToTextComponent::BeginPlay()
{
	Super::BeginPlay();

	CaptureHandler = MakeUnique<FAudioCaptureHandler>();
	WhisperClient = MakeUnique<FWhisperAPIClient>();

	// Register for Android lifecycle: ON_STOP destroys the Oboe audio stream,
	// so we must re-initialize capture when the app resumes.
	AppReactivatedHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(
		this, &USpeechToTextComponent::OnAppReactivated);

	// Auto-enable hands-free mode if configured in project settings
	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings && Settings->bDefaultHandsFreeMode)
	{
		EnableHandsFreeMode();
	}
}

void USpeechToTextComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(AppReactivatedHandle);

	// End direct audio stream if active
	if (bDirectAudioStreamActive)
	{
		if (UNPCConversationComponent* NPC = ResolveTargetNPC())
		{
			NPC->EndAudioInput();
		}
		bDirectAudioStreamActive = false;
	}

	if (bHandsFreeMode)
	{
		DisableHandsFreeMode();
	}
	else if (CurrentState == ESpeechToTextState::Recording)
	{
		CaptureHandler->StopCapture();
	}

	CaptureHandler.Reset();
	WhisperClient.Reset();

	Super::EndPlay(EndPlayReason);
}

void USpeechToTextComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Delayed audio capture re-initialization after Android lifecycle event.
	if (bPendingReinit)
	{
		ReinitDelayTimer -= DeltaTime;
		if (ReinitDelayTimer <= 0.0f)
		{
			bPendingReinit = false;
			UE_LOG(LogTemp, Log, TEXT("SpeechToText: Delayed re-init — re-initializing audio capture now"));
			ReinitializeAudioCapture();
		}
		return;
	}

	if (CurrentState == ESpeechToTextState::Recording)
	{
		RecordingTimer += DeltaTime;

		// In push-to-talk mode, auto-stop at max duration
		if (!bHandsFreeMode && RecordingTimer >= ResolveMaxRecordingDuration())
		{
			UE_LOG(LogTemp, Log, TEXT("SpeechToText: Max recording duration (%.0f s) reached, auto-stopping"), ResolveMaxRecordingDuration());
			StopRecording();
		}
	}

	// Energy-based VAD for hands-free mode
	if (bHandsFreeMode && CaptureHandler)
	{
		// Server-VAD-only mode: lazily open the direct-audio stream when the NPC becomes
		// resolvable. Doing this in tick covers late-binding cases (NPC spawned after STT,
		// TargetNPCConversation set by gaze/proximity, WebSocket pool not ready at BeginPlay).
		if (!bUseLocalVAD && bStreamDirectAudio && !bDirectAudioStreamActive)
		{
			if (UNPCConversationComponent* NPC = ResolveTargetNPC())
			{
				if (NPC->StartAudioInput(/*bForceServerVAD=*/true))
				{
					bDirectAudioStreamActive = true;
					ResetResamplerState();
					CaptureHandler->NotifySpeechStarted();
					RecordingTimer = 0.0f;
					SetState(ESpeechToTextState::Recording);
					UE_LOG(LogTemp, Log, TEXT("SpeechToText: Server-VAD stream opened to NPC '%s' — streaming continuously"),
						*NPC->GetOwner()->GetName());
				}
				else
				{
					// Log once per 3s to avoid spam while StartAudioInput keeps rejecting (e.g. NPC busy).
					LazyStreamOpenRetryTimer += DeltaTime;
					if (LazyStreamOpenRetryTimer >= 3.0f)
					{
						LazyStreamOpenRetryTimer = 0.0f;
						UE_LOG(LogTemp, Warning, TEXT("SpeechToText: StartAudioInput rejected by NPC (check bUseDirectAudioInput=true, ChatBackend=GeminiLive, and NPC state=Idle)"));
					}
				}
			}
			else
			{
				LazyStreamOpenRetryTimer += DeltaTime;
				if (LazyStreamOpenRetryTimer >= 3.0f)
				{
					LazyStreamOpenRetryTimer = 0.0f;
					UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Still waiting for target NPC — set TargetNPCConversation or place NPC on same actor/player pawn"));
				}
			}
		}

		// Drain buffered audio to prevent unbounded PendingVADAudio growth.
		// The audio callback appends to this buffer every ~21ms; without draining,
		// the TArray grows and reallocates inside the audio thread's lock, choking
		// the callback on memory-constrained devices like Quest.
		TArray<float> DrainedAudio = CaptureHandler->DrainPendingAudio();

		// If direct audio streaming is enabled and we're recording, stream audio to NPC
		if (bStreamDirectAudio && bDirectAudioStreamActive && CurrentState == ESpeechToTextState::Recording && DrainedAudio.Num() > 0)
		{
			StreamAudioToNPC(DrainedAudio);
		}

		// Server-VAD-only mode streams continuously; skip local VAD and max-duration auto-stop.
		if (bUseLocalVAD)
		{
			ProcessEnergyVAD(DeltaTime);

			// Handle max duration reached on audio thread
			if (CaptureHandler->IsMaxDurationReached())
			{
				CaptureHandler->ClearMaxDurationFlag();
				if (CurrentState == ESpeechToTextState::Recording)
				{
					// For direct audio streaming, transition back to Listening so we can detect
					// the next speech onset, but keep the audio stream active.
					if (bStreamDirectAudio && bDirectAudioStreamActive)
					{
						UE_LOG(LogTemp, Log, TEXT("SpeechToText: Max duration reached, returning to Listening (server VAD mode, stream stays active)"));
						bEnergyVADSpeechActive = false;
						ConsecutiveSpeechMs = 0.0f;
						ConsecutiveSilenceMs = 0.0f;
						SetState(ESpeechToTextState::Listening);
					}
					else
					{
						// Whisper mode: end capture and send audio
						UE_LOG(LogTemp, Log, TEXT("SpeechToText: VAD max duration auto-stop"));
						CaptureHandler->NotifySpeechEnded();
						bEnergyVADSpeechActive = false;
						ConsecutiveSpeechMs = 0.0f;
						ConsecutiveSilenceMs = 0.0f;
						ProcessAndSendAudio();
					}
				}
			}
		}
	}
}

// ── Energy-based VAD ───────────────────────────────────────────────────────────

void USpeechToTextComponent::ProcessEnergyVAD(float DeltaTime)
{
	const float Energy = CaptureHandler->GetCurrentRMSEnergy();
	const float DeltaMs = DeltaTime * 1000.0f;
	const bool bAboveThreshold = (Energy >= EnergyThreshold);

	if (bAboveThreshold)
	{
		ConsecutiveSpeechMs += DeltaMs;
		ConsecutiveSilenceMs = 0.0f;
	}
	else
	{
		ConsecutiveSilenceMs += DeltaMs;
		// Only reset speech counter if we haven't confirmed onset yet
		if (!bEnergyVADSpeechActive)
		{
			ConsecutiveSpeechMs = 0.0f;
		}
	}

	// Speech onset: energy above threshold for MinSpeechDurationMs
	if (!bEnergyVADSpeechActive && ConsecutiveSpeechMs >= static_cast<float>(MinSpeechDurationMs))
	{
		bEnergyVADSpeechActive = true;

		if (CurrentState == ESpeechToTextState::Listening)
		{
			CaptureHandler->NotifySpeechStarted();
			RecordingTimer = 0.0f;
			SetState(ESpeechToTextState::Recording);
			UE_LOG(LogTemp, Log, TEXT("SpeechToText: Energy VAD speech onset (RMS=%.5f, threshold=%.5f)"), Energy, EnergyThreshold);

			// For direct audio streaming, start the audio input session with the NPC
			// (skip if already streaming - server VAD mode keeps stream open between utterances)
			if (bStreamDirectAudio)
			{
				if (bDirectAudioStreamActive)
				{
					UE_LOG(LogTemp, Log, TEXT("SpeechToText: Speech onset detected, audio stream already active (continuing)"));
				}
				else
				{
					UE_LOG(LogTemp, Log, TEXT("SpeechToText: Direct audio mode enabled, looking for target NPC..."));
					if (UNPCConversationComponent* NPC = ResolveTargetNPC())
					{
						UE_LOG(LogTemp, Log, TEXT("SpeechToText: Found NPC, bUseDirectAudioInput=%d, ChatBackend=%d"),
							NPC->bUseDirectAudioInput, static_cast<int32>(NPC->ChatBackend));
						if (NPC->StartAudioInput())
						{
							bDirectAudioStreamActive = true;
							ResetResamplerState();
							UE_LOG(LogTemp, Log, TEXT("SpeechToText: Started direct audio stream to NPC"));
						}
						else
						{
							UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Failed to start direct audio stream to NPC (check bUseDirectAudioInput and ChatBackend=GeminiLive)"));
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("SpeechToText: No target NPC found for direct audio streaming. Set TargetNPCConversation or place NPC on same actor/player pawn."));
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("SpeechToText: Using Whisper transcription (bStreamDirectAudio=false)"));
			}
		}
	}

	// Speech end: silence for SilenceDurationMs after confirmed speech
	// For direct audio mode, skip client VAD cutoff - let server VAD detect end of speech
	if (bEnergyVADSpeechActive && ConsecutiveSilenceMs >= static_cast<float>(SilenceDurationMs))
	{
		bEnergyVADSpeechActive = false;
		ConsecutiveSpeechMs = 0.0f;
		ConsecutiveSilenceMs = 0.0f;

		if (CurrentState == ESpeechToTextState::Recording)
		{
			// For direct audio streaming, transition back to Listening so we can detect
			// the next speech onset, but keep the audio stream to NPC active.
			// Server VAD handles the actual turn management.
			if (bStreamDirectAudio && bDirectAudioStreamActive)
			{
				UE_LOG(LogTemp, Log, TEXT("SpeechToText: Silence detected, returning to Listening (server VAD mode, stream stays active)"));
				SetState(ESpeechToTextState::Listening);
			}
			else
			{
				// Whisper mode: end capture and send audio
				CaptureHandler->NotifySpeechEnded();
				UE_LOG(LogTemp, Log, TEXT("SpeechToText: Energy VAD speech end (silence >= %dms)"), SilenceDurationMs);
				ProcessAndSendAudio();
			}
		}
	}

	// Periodic logging
	VADLogTimer += DeltaTime;
	if (VADLogTimer >= 1.0f)
	{
		VADLogTimer = 0.0f;
		UE_LOG(LogSpeechToText, Verbose, TEXT("VAD: state=%s  RMS=%.5f  thresh=%.4f  speech=%.0fms  silence=%.0fms"),
			(CurrentState == ESpeechToTextState::Recording) ? TEXT("RECORDING") : TEXT("LISTENING"),
			Energy, EnergyThreshold, ConsecutiveSpeechMs, ConsecutiveSilenceMs);
	}
}

// ── Push-to-talk mode ──────────────────────────────────────────────────────────

void USpeechToTextComponent::StartRecording()
{
	if (CurrentState != ESpeechToTextState::Idle && CurrentState != ESpeechToTextState::Listening)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot start recording in state %d"), static_cast<int32>(CurrentState));
		return;
	}

	if (!CaptureHandler)
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: CaptureHandler not initialized. Is the component active?"));
		return;
	}

	// If in hands-free/listening mode, don't allow manual push-to-talk
	if (bHandsFreeMode)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot use push-to-talk while in hands-free mode"));
		return;
	}

	// On Android, ensure microphone permission is granted before proceeding
	if (!EnsureMicPermission(EPendingMicAction::StartRecording))
	{
		return;
	}

	CaptureHandler->MaxDurationSeconds = ResolveMaxRecordingDuration();

	if (!CaptureHandler->StartCapture(ResolveSampleRate()))
	{
		SetState(ESpeechToTextState::Error);

		FSpeechToTextResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to start microphone capture. Check that a microphone is connected.");
		OnTranscriptionComplete.Broadcast(Result);

		// Return to idle so user can retry
		SetState(ESpeechToTextState::Idle);
		return;
	}

	RecordingTimer = 0.0f;
	SetComponentTickEnabled(true);
	SetState(ESpeechToTextState::Recording);
}

void USpeechToTextComponent::StopRecording()
{
	if (CurrentState != ESpeechToTextState::Recording)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot stop recording - not currently recording"));
		return;
	}

	// In push-to-talk mode, stop the capture stream
	if (!bHandsFreeMode)
	{
		SetComponentTickEnabled(false);
		CaptureHandler->StopCapture();
	}
	// In hands-free mode, the capture stream stays open (listening continues)

	ProcessAndSendAudio();
}

// ── Hands-free (Energy VAD) mode ───────────────────────────────────────────────

void USpeechToTextComponent::EnableHandsFreeMode()
{
	if (bHandsFreeMode)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Hands-free mode already enabled"));
		return;
	}

	if (CurrentState == ESpeechToTextState::Recording || CurrentState == ESpeechToTextState::Processing)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot enable hands-free mode while recording/processing"));
		return;
	}

	if (!CaptureHandler)
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: CaptureHandler not initialized"));
		return;
	}

	// On Android, ensure microphone permission is granted before proceeding
	if (!EnsureMicPermission(EPendingMicAction::EnableHandsFree))
	{
		return;
	}

	// Configure from project settings
	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings)
	{
		CaptureHandler->PreRollSeconds = Settings->VADPreRollSeconds;
		EnergyThreshold = Settings->EnergyThreshold;
		MinSpeechDurationMs = Settings->VADMinSpeechDurationMs;
		SilenceDurationMs = Settings->VADSilenceDurationMs;
	}
	CaptureHandler->MaxDurationSeconds = ResolveMaxRecordingDuration();

	// Reset energy VAD state
	ConsecutiveSpeechMs = 0.0f;
	ConsecutiveSilenceMs = 0.0f;
	bEnergyVADSpeechActive = false;

	if (!CaptureHandler->StartListening(ResolveSampleRate()))
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: Failed to start listening"));
		return;
	}

	bHandsFreeMode = true;
	SetState(ESpeechToTextState::Listening);
	SetComponentTickEnabled(true);

	if (bUseLocalVAD)
	{
		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Hands-free mode enabled (Local Energy VAD, threshold=%.4f, minSpeech=%dms, silence=%dms)"),
			EnergyThreshold, MinSpeechDurationMs, SilenceDurationMs);
	}
	else if (bStreamDirectAudio)
	{
		// Server-VAD-only mode: stream opens lazily on tick when the target NPC is resolvable.
		// Don't require NPC to be ready at EnableHandsFreeMode time — it may be spawned or
		// assigned later (common in VR when gaze/proximity sets TargetNPCConversation).
		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Hands-free mode enabled (Server VAD only — waiting for target NPC to open direct-audio stream)"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: bUseLocalVAD=false requires bStreamDirectAudio=true; Whisper mode needs local VAD. Hands-free will not trigger."));
	}
}

void USpeechToTextComponent::DisableHandsFreeMode()
{
	if (!bHandsFreeMode)
	{
		return;
	}

	bHandsFreeMode = false;
	bEnergyVADSpeechActive = false;
	ConsecutiveSpeechMs = 0.0f;
	ConsecutiveSilenceMs = 0.0f;

	// End direct audio stream if active
	if (bDirectAudioStreamActive)
	{
		if (UNPCConversationComponent* NPC = ResolveTargetNPC())
		{
			NPC->EndAudioInput();
		}
		bDirectAudioStreamActive = false;
	}

	if (CaptureHandler)
	{
		CaptureHandler->StopListening();
	}

	SetComponentTickEnabled(false);
	SetState(ESpeechToTextState::Idle);

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Hands-free mode disabled"));
}

// ── Shared processing ──────────────────────────────────────────────────────────

void USpeechToTextComponent::ProcessAndSendAudio()
{
	const float Duration = CaptureHandler->GetCaptureDuration();

	if (Duration < 0.1f)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Recording too short (%.2f s), skipping transcription"), Duration);

		FSpeechToTextResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Recording too short.");
		Result.AudioDuration = Duration;
		OnTranscriptionComplete.Broadcast(Result);

		// Return to appropriate state
		SetState(bHandsFreeMode ? ESpeechToTextState::Listening : ESpeechToTextState::Idle);
		return;
	}

	SetState(ESpeechToTextState::Processing);

	TArray<uint8> WAVData = CaptureHandler->GetWAVData();

	if (bSaveDebugWAV && WAVData.Num() > 0)
	{
		const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("SpeechToText");
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString FilePath = SaveDir / FString::Printf(TEXT("debug_%s.wav"), *Timestamp);
		if (FFileHelper::SaveArrayToFile(WAVData, *FilePath))
		{
			UE_LOG(LogTemp, Log, TEXT("SpeechToText: Debug WAV saved to %s (%d bytes)"), *FilePath, WAVData.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Failed to save debug WAV to %s"), *FilePath);
		}
	}

	const FString ResolvedKey = ResolveAPIKey();

	WhisperClient->SendTranscriptionRequest(
		WAVData,
		ResolvedKey,
		ResolveLanguage(),
		ResolveModel(),
		ResolvePrompt(),
		Duration,
		[this](const FSpeechToTextResult& Result)
		{
			// HTTP callback is already on game thread in UE5
			OnTranscriptionResult(Result);
		}
	);
}

void USpeechToTextComponent::OnTranscriptionResult(const FSpeechToTextResult& Result)
{
	if (Result.bSuccess)
	{
		// Detect Whisper prompt hallucination: when given silence/noise, Whisper
		// often returns the prompt text verbatim. Only discard if mic energy was
		// very low (actual silence/noise), not when VAD confirmed real speech.
		const FString Prompt = ResolvePrompt();
		const float MicEnergy = CaptureHandler ? CaptureHandler->GetCurrentRMSEnergy() : 0.0f;
		if (!Prompt.IsEmpty() && Result.TranscribedText.TrimStartAndEnd().Equals(Prompt.TrimStartAndEnd()) && MicEnergy < 0.005f)
		{
			UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Discarding prompt hallucination (text matches prompt, RMS=%.4f)"), MicEnergy);

			FSpeechToTextResult Discarded;
			Discarded.bSuccess = false;
			Discarded.ErrorMessage = TEXT("No speech detected (ambient noise).");
			Discarded.AudioDuration = Result.AudioDuration;
			OnTranscriptionComplete.Broadcast(Discarded);

			SetState(bHandsFreeMode ? ESpeechToTextState::Listening : ESpeechToTextState::Idle);
			return;
		}

		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Transcription result: \"%s\" (RMS=%.4f)"), *Result.TranscribedText, MicEnergy);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: Transcription failed: %s"), *Result.ErrorMessage);
	}

	OnTranscriptionComplete.Broadcast(Result);

	// Return to listening if hands-free, otherwise idle
	SetState(bHandsFreeMode ? ESpeechToTextState::Listening : ESpeechToTextState::Idle);
}

// ── Query helpers ──────────────────────────────────────────────────────────────

float USpeechToTextComponent::GetMicrophoneEnergy() const
{
	if (CaptureHandler)
	{
		return CaptureHandler->GetCurrentRMSEnergy();
	}
	return 0.0f;
}

FString USpeechToTextComponent::GetCaptureDeviceName() const
{
	if (CaptureHandler)
	{
		return CaptureHandler->GetDeviceName();
	}
	return TEXT("(Not initialized)");
}

float USpeechToTextComponent::GetRecordingDuration() const
{
	if (CaptureHandler && CurrentState == ESpeechToTextState::Recording)
	{
		return CaptureHandler->GetCaptureDuration();
	}
	return 0.0f;
}

void USpeechToTextComponent::SetState(ESpeechToTextState NewState)
{
	if (CurrentState != NewState)
	{
		CurrentState = NewState;
		OnRecordingStateChanged.Broadcast(NewState);
	}
}

// ── Android lifecycle recovery ─────────────────────────────────────────────────

void USpeechToTextComponent::OnAppReactivated()
{
	if (!CaptureHandler)
	{
		return;
	}

	if (bHandsFreeMode && (CurrentState == ESpeechToTextState::Listening || CurrentState == ESpeechToTextState::Recording))
	{
		UE_LOG(LogTemp, Log, TEXT("SpeechToText: App reactivated — scheduling audio capture re-init (2.5s delay)"));
		bPendingReinit = true;
		ReinitDelayTimer = 2.5f;
	}
}

void USpeechToTextComponent::ReinitializeAudioCapture()
{
	// Close the dead stream
	CaptureHandler->StopListening();

	// Re-create the capture handler entirely (fresh FAudioCapture + Oboe stream)
	const float SavedPreRoll = CaptureHandler->PreRollSeconds;
	const float SavedMaxDuration = CaptureHandler->MaxDurationSeconds;
	CaptureHandler = MakeUnique<FAudioCaptureHandler>();
	CaptureHandler->PreRollSeconds = SavedPreRoll;
	CaptureHandler->MaxDurationSeconds = SavedMaxDuration;

	if (!CaptureHandler->StartListening(ResolveSampleRate()))
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: Failed to re-initialize audio capture after app reactivation"));
		return;
	}

	// Reset energy VAD state
	ConsecutiveSpeechMs = 0.0f;
	ConsecutiveSilenceMs = 0.0f;
	bEnergyVADSpeechActive = false;

	SetState(ESpeechToTextState::Listening);
	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Audio capture re-initialized successfully"));
}

// ── Android microphone permission ──────────────────────────────────────────────

bool USpeechToTextComponent::EnsureMicPermission(EPendingMicAction Action)
{
#if PLATFORM_ANDROID
	if (!UAndroidPermissionFunctionLibrary::CheckPermission(TEXT("android.permission.RECORD_AUDIO")))
	{
		PendingMicAction = Action;

		TArray<FString> Permissions;
		Permissions.Add(TEXT("android.permission.RECORD_AUDIO"));

		UAndroidPermissionCallbackProxy* Callback =
			UAndroidPermissionFunctionLibrary::AcquirePermissions(Permissions);
		if (Callback)
		{
			Callback->OnPermissionsGrantedDynamicDelegate.AddDynamic(
				this, &USpeechToTextComponent::OnMicPermissionResult);
		}

		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Requesting RECORD_AUDIO permission..."));
		return false;
	}
#endif
	return true;
}

void USpeechToTextComponent::OnMicPermissionResult(
	const TArray<FString>& Permissions, const TArray<bool>& GrantResults)
{
	const EPendingMicAction Action = PendingMicAction;
	PendingMicAction = EPendingMicAction::None;

	bool bGranted = false;
	for (int32 i = 0; i < Permissions.Num(); ++i)
	{
		if (Permissions[i] == TEXT("android.permission.RECORD_AUDIO"))
		{
			bGranted = GrantResults.IsValidIndex(i) && GrantResults[i];
			break;
		}
	}

	if (!bGranted)
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: RECORD_AUDIO permission denied by user"));

		FSpeechToTextResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Microphone permission denied. Please grant microphone access in device settings.");
		OnTranscriptionComplete.Broadcast(Result);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: RECORD_AUDIO permission granted"));

	switch (Action)
	{
	case EPendingMicAction::StartRecording:
		StartRecording();
		break;
	case EPendingMicAction::EnableHandsFree:
		EnableHandsFreeMode();
		break;
	default:
		break;
	}
}

// ── Resolve helpers ────────────────────────────────────────────────────────────

FString USpeechToTextComponent::ResolveAPIKey() const
{
	if (!APIKeyOverride.IsEmpty())
	{
		return APIKeyOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings && !Settings->APIKey.IsEmpty())
	{
		return Settings->APIKey;
	}

	const FString EnvKey = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_API_KEY"));
	if (!EnvKey.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Using API key from OPENAI_API_KEY environment variable"));
		return EnvKey;
	}

	UE_LOG(LogTemp, Warning, TEXT("SpeechToText: No API key configured. Set it in Project Settings → Plugins → Speech To Text, or set OPENAI_API_KEY env var."));
	return FString();
}

FString USpeechToTextComponent::ResolveLanguage() const
{
	if (!LanguageOverride.IsEmpty())
	{
		return LanguageOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings)
	{
		return Settings->DefaultLanguage;
	}

	return TEXT("vi");
}

float USpeechToTextComponent::ResolveMaxRecordingDuration() const
{
	if (MaxRecordingDurationOverride > 0.0f)
	{
		return MaxRecordingDurationOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings)
	{
		return Settings->DefaultMaxRecordingDuration;
	}

	return 30.0f;
}

FString USpeechToTextComponent::ResolvePrompt() const
{
	if (!PromptOverride.IsEmpty())
	{
		return PromptOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings)
	{
		return Settings->DefaultPrompt;
	}

	return FString();
}

FString USpeechToTextComponent::ResolveModel() const
{
	if (!ModelOverride.IsEmpty())
	{
		return ModelOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings && !Settings->Model.IsEmpty())
	{
		return Settings->Model;
	}

	return TEXT("gpt-4o-mini-transcribe");
}

int32 USpeechToTextComponent::ResolveSampleRate() const
{
	if (SampleRateOverride > 0)
	{
		return SampleRateOverride;
	}

	const USpeechToTextSettings* Settings = USpeechToTextSettings::Get();
	if (Settings)
	{
		return Settings->DefaultSampleRate;
	}

	return 16000;
}

// ── Direct Audio Streaming helpers ─────────────────────────────────────────────

TArray<uint8> USpeechToTextComponent::ConvertFloatToPCM16(const TArray<float>& FloatSamples)
{
	TArray<uint8> PCMData;
	PCMData.SetNumUninitialized(FloatSamples.Num() * sizeof(int16));

	int16* PCMSamples = reinterpret_cast<int16*>(PCMData.GetData());
	for (int32 i = 0; i < FloatSamples.Num(); ++i)
	{
		// Clamp to [-1.0, 1.0] and convert to int16
		const float Clamped = FMath::Clamp(FloatSamples[i], -1.0f, 1.0f);
		PCMSamples[i] = static_cast<int16>(Clamped * 32767.0f);
	}

	return PCMData;
}

UNPCConversationComponent* USpeechToTextComponent::ResolveTargetNPC() const
{
	// First check the explicit target
	if (TargetNPCConversation && IsValid(TargetNPCConversation))
	{
		return TargetNPCConversation;
	}

	// Try to find on the same actor
	if (AActor* Owner = GetOwner())
	{
		if (UNPCConversationComponent* NPC = Owner->FindComponentByClass<UNPCConversationComponent>())
		{
			return NPC;
		}
	}

	// Try to find on the player pawn (common VR setup)
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				if (UNPCConversationComponent* NPC = Pawn->FindComponentByClass<UNPCConversationComponent>())
				{
					return NPC;
				}
			}
		}
	}

	return nullptr;
}

void USpeechToTextComponent::StreamAudioToNPC(const TArray<float>& FloatSamples)
{
	if (FloatSamples.Num() == 0)
	{
		return;
	}

	UNPCConversationComponent* NPC = ResolveTargetNPC();
	if (!NPC)
	{
		return;
	}

	// Get actual capture format
	const int32 NumChannels = CaptureHandler ? CaptureHandler->GetCaptureNumChannels() : 1;
	const int32 SampleRate = CaptureHandler ? CaptureHandler->GetCaptureSampleRate() : 16000;

	// Log format on first chunk for debugging
	static bool bLoggedFormat = false;
	if (!bLoggedFormat)
	{
		UE_LOG(LogTemp, Log,
			TEXT("SpeechToText: Streaming audio format: %d Hz, %d ch -> 16000 Hz, mono, 16-bit PCM (resampling %s)"),
			SampleRate, NumChannels,
			SampleRate == 16000 ? TEXT("disabled") : TEXT("enabled (linear interp)"));
		bLoggedFormat = true;
	}

	// --- 1. Downmix to mono ---
	TArray<float> MonoSamples;
	const TArray<float>* Mono = &FloatSamples;

	if (NumChannels == 2)
	{
		const int32 NumFrames = FloatSamples.Num() / 2;
		MonoSamples.SetNumUninitialized(NumFrames);
		for (int32 i = 0; i < NumFrames; ++i)
		{
			MonoSamples[i] = (FloatSamples[i * 2] + FloatSamples[i * 2 + 1]) * 0.5f;
		}
		Mono = &MonoSamples;
	}
	else if (NumChannels > 2)
	{
		const int32 NumFrames = FloatSamples.Num() / NumChannels;
		MonoSamples.SetNumUninitialized(NumFrames);
		for (int32 i = 0; i < NumFrames; ++i)
		{
			MonoSamples[i] = FloatSamples[i * NumChannels];
		}
		Mono = &MonoSamples;
	}

	// --- 2. Resample to 16kHz (Gemini expects 16kHz PCM; capture is usually 48kHz on VR/Windows) ---
	TArray<float> Resampled;
	const TArray<float>* FinalSamples = Mono;

	if (SampleRate != 16000 && SampleRate > 0)
	{
		ResampleTo16k(*Mono, SampleRate, Resampled);
		FinalSamples = &Resampled;
	}

	// --- 3. Convert to 16-bit PCM and send ---
	TArray<uint8> PCMData = ConvertFloatToPCM16(*FinalSamples);
	const bool bSent = NPC->SendAudioInputChunk(PCMData);

	// Periodic health telemetry (every ~2s) so the user can confirm audio is actually flowing.
	static double LastStreamLogTime = 0.0;
	static int32 StreamedChunkCount = 0;
	static int32 StreamedSampleCount = 0;
	++StreamedChunkCount;
	StreamedSampleCount += FinalSamples->Num();
	const double Now = FPlatformTime::Seconds();
	if (Now - LastStreamLogTime > 2.0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("SpeechToText: Streaming to NPC — %d chunks, %d samples (~%.2fs) in last window (last chunk sent=%d)"),
			StreamedChunkCount, StreamedSampleCount,
			static_cast<float>(StreamedSampleCount) / 16000.0f, bSent ? 1 : 0);
		LastStreamLogTime = Now;
		StreamedChunkCount = 0;
		StreamedSampleCount = 0;
	}
}

void USpeechToTextComponent::ResampleTo16k(const TArray<float>& InMono, int32 InputRate, TArray<float>& OutMono)
{
	OutMono.Reset();
	if (InMono.Num() == 0 || InputRate <= 0) return;

	// Fast path: rates already match, no resample needed.
	if (InputRate == 16000)
	{
		OutMono = InMono;
		return;
	}

	// Stitch previous chunk's last sample so interpolation straddles the boundary cleanly.
	TArray<float> Source;
	Source.Reserve(InMono.Num() + 1);
	if (bResampleHasPrev)
	{
		Source.Add(ResamplePrevSample);
	}
	Source.Append(InMono);

	const int32 SrcN = Source.Num();
	if (SrcN < 2)
	{
		// Not enough to interpolate — save last sample for next chunk and bail.
		ResamplePrevSample = Source.Last();
		bResampleHasPrev = true;
		return;
	}

	// Ratio: input samples consumed per 1 output sample (e.g. 48000/16000 = 3.0).
	const double Ratio = static_cast<double>(InputRate) / 16000.0;

	// Reserve with a small safety margin for ratios slightly below 1.
	OutMono.Reserve(static_cast<int32>(static_cast<double>(SrcN) / Ratio) + 1);

	double Pos = ResamplePos;
	while (true)
	{
		const int32 Idx = static_cast<int32>(Pos);
		if (Idx + 1 >= SrcN) break;
		const double Frac = Pos - static_cast<double>(Idx);
		const float A = Source[Idx];
		const float B = Source[Idx + 1];
		OutMono.Add(static_cast<float>(A * (1.0 - Frac) + B * Frac));
		Pos += Ratio;
	}

	// Persist state: shift coordinates so the last source sample sits at index 0 next call.
	ResamplePrevSample = Source.Last();
	bResampleHasPrev = true;
	ResamplePos = Pos - static_cast<double>(SrcN - 1);
	if (ResamplePos < 0.0) ResamplePos = 0.0; // safety clamp
}

void USpeechToTextComponent::ResetResamplerState()
{
	ResamplePos = 0.0;
	ResamplePrevSample = 0.0f;
	bResampleHasPrev = false;
}
