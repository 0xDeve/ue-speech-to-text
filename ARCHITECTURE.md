# SpeechToText Plugin - Architecture

> Auto-maintained via `/update-architecture`

## Purpose

Capture microphone input and transcribe Vietnamese speech using OpenAI Whisper API.
Supports both push-to-talk and hands-free (VAD) modes.

## Module: SpeechToText (Runtime)

### Dependencies

**Public**: Core, CoreUObject, Engine, InputCore, AudioCaptureCore, AudioMixer, DeveloperSettings, HTTP
**Private**: Json, JsonUtilities, UMG, Slate, SlateCore, RuntimeAudioImporter
**Plugin**: AudioCapture (engine), RuntimeAudioImporter (WebRTC VAD via libfvad)

### File Structure

```
Plugins/SpeechToText/
├── SpeechToText.uplugin
├── Source/SpeechToText/
│   ├── SpeechToText.Build.cs
│   ├── Public/
│   │   ├── SpeechToTextModule.h        # Module interface
│   │   ├── SpeechToTextComponent.h     # Main component (BlueprintSpawnable)
│   │   ├── SpeechToTextTypes.h         # Enums, structs, delegates
│   │   ├── SpeechToTextSettings.h      # Project settings (UDeveloperSettings)
│   │   ├── AudioCaptureHandler.h       # Mic capture → WAV, pre-roll buffer
│   │   ├── WhisperAPIClient.h          # OpenAI Whisper HTTP client
│   │   └── SpeechToTextWidget.h        # UMG debug widget
│   └── Private/
│       ├── SpeechToTextModule.cpp
│       ├── SpeechToTextComponent.cpp
│       ├── SpeechToTextSettings.cpp
│       ├── AudioCaptureHandler.cpp
│       ├── WhisperAPIClient.cpp
│       └── SpeechToTextWidget.cpp
├── ARCHITECTURE.md
└── USAGE.md
```

### Class Diagram

```
USpeechToTextComponent (UActorComponent, BlueprintSpawnable)
│   States: Idle → Recording → Processing → Idle/Error
│   Hands-free: Idle → Listening ⟷ Recording → Processing → Listening
│   Delegates: OnTranscriptionComplete, OnRecordingStateChanged
│   Methods: StartRecording/StopRecording (PTT), EnableHandsFreeMode/DisableHandsFreeMode (VAD)
│
├── owns → FAudioCaptureHandler
│   ├── Push-to-talk: StartCapture/StopCapture
│   ├── Listening mode: StartListening/StopListening
│   │   ├── Pre-roll ring buffer (captures audio before speech onset, default 0.8s)
│   │   ├── PendingVADAudio buffer (for game thread VAD processing)
│   │   ├── DrainPendingAudio() — game thread drains buffered audio
│   │   ├── NotifySpeechStarted() — copies pre-roll, starts capture
│   │   └── NotifySpeechEnded() — stops capture, resets pre-roll
│   ├── RMS energy computation per frame (debug visualization)
│   └── uses → Audio::FAudioCapture (engine API)
│       └── outputs → WAV byte array
│
├── owns → URuntimeVoiceActivityDetector (from RuntimeAudioImporter)
│   ├── WebRTC GMM-based spectral classifier (libfvad)
│   ├── Modes: Quality, LowBitrate, Aggressive, VeryAggressive
│   ├── Temporal filtering: MinimumSpeechDuration, SilenceDuration
│   └── Delegates: OnSpeechStartedNative, OnSpeechEndedNative
│
└── owns → FWhisperAPIClient
    └── sends → HTTP POST to OpenAI Whisper
        └── returns → transcribed text string

USpeechToTextSettings (UDeveloperSettings)
    Project Settings → Plugins → Speech To Text
    Fields: APIKey, Model("gpt-4o-mini-transcribe"), DefaultLanguage("vi"),
            DefaultPrompt, DefaultMaxRecordingDuration, DefaultSampleRate(16000),
            bDefaultHandsFreeMode, VADMode(Aggressive),
            VADMinSpeechDurationMs(300), VADSilenceDurationMs(1500),
            VADPreRollSeconds(0.8)
```

### Data Flow

#### Push-to-Talk Mode
```
IA_Talk input → USpeechToTextComponent::StartRecording()
  → FAudioCaptureHandler::StartCapture() → PCM float samples accumulated
  → StopRecording() → StopCapture() → encode to WAV (16kHz mono)
  → FWhisperAPIClient::SendTranscriptionRequest(WAV, "vi")
  → HTTP multipart POST → OpenAI Whisper API
  → JSON response → extract text
  → USpeechToTextComponent::OnTranscriptionComplete broadcast
```

#### Hands-Free (VAD) Mode
```
EnableHandsFreeMode()
  → Create URuntimeVoiceActivityDetector (WebRTC libfvad)
  → Configure VAD mode (Aggressive) + timing (300ms onset, 1500ms silence)
  → FAudioCaptureHandler::StartListening()
  → Mic continuously open, audio buffered for game thread

TickComponent (game thread):
  → DrainPendingAudio() from capture handler
  → Feed to URuntimeVoiceActivityDetector::ProcessVAD()
  → WebRTC GMM classifier: spectral analysis on 10-30ms frames
  → Speech detected (consecutive voice frames >= MinimumSpeechDuration):
    → OnSpeechStartedNative → NotifySpeechStarted()
    → Pre-roll buffer (0.8s) copied to captured samples (word onset preserved)
    → State: Listening → Recording
    → Continue accumulating speech audio on audio thread
  → Silence detected (consecutive silence >= SilenceDuration):
    → OnSpeechEndedNative → NotifySpeechEnded()
    → State: Recording → Processing
    → Encode to WAV → Whisper API → transcription
    → State: Processing → Listening (ready for next utterance)
```

### Threading Model

```
Audio Thread (FAudioCaptureHandler callback):
  ├── Compute RMS energy (debug)
  ├── Append audio to PendingVADAudio buffer (under BufferLock)
  ├── If !speech: write to pre-roll ring buffer
  └── If speech: append to CapturedSamples

Game Thread (USpeechToTextComponent::TickComponent):
  ├── Drain PendingVADAudio (under BufferLock)
  ├── Feed to URuntimeVoiceActivityDetector::ProcessVAD()
  ├── VAD callbacks → NotifySpeechStarted/Ended (under BufferLock)
  └── Check max duration flag
```
