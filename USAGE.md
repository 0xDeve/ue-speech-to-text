# SpeechToText Plugin

Push-to-talk speech-to-text for Unreal Engine 5.6 using the OpenAI Whisper API. Captures microphone audio, encodes it to WAV, sends it to Whisper, and returns transcribed text via delegates. Optimized for VR (bypasses audio engine to prevent echo/feedback).

## Setup

### 1. Enable the Plugin

The plugin is already enabled in `VR_Demo.uproject`. If adding to a different project, add to your `.uproject`:

```json
{
    "Name": "SpeechToText",
    "Enabled": true
}
```

Also ensure the `AudioCapture` plugin is enabled (required dependency).

### 2. Configure Your API Key

Three options, checked in this priority order:

| Priority | Method | Where |
|----------|--------|-------|
| 1 | Per-instance override | Component Details panel > "API Key Override" |
| 2 | Project Settings | Edit > Project Settings > Plugins > Speech To Text > API Key |
| 3 | Environment variable | Set `OPENAI_API_KEY` in your OS environment |

### 3. Project Settings

Navigate to **Edit > Project Settings > Plugins > Speech To Text**:

| Setting | Default | Description |
|---------|---------|-------------|
| OpenAI API Key | *(empty)* | Your OpenAI API key |
| Default Language | `vi` | ISO 639-1 language code (`vi` = Vietnamese, `en` = English, etc.) |
| Max Recording Duration | `30.0 s` | Auto-stops recording after this many seconds |
| Sample Rate | `16000 Hz` | Microphone capture sample rate. 16 kHz is optimal for Whisper |

Settings are saved to `Config/DefaultSpeechToText.ini`.

## Architecture

```
Plugins/SpeechToText/
├── SpeechToText.uplugin
└── Source/SpeechToText/
    ├── SpeechToText.Build.cs
    ├── Public/
    │   ├── SpeechToTextModule.h        # IModuleInterface
    │   ├── SpeechToTextTypes.h         # ESpeechToTextState, FSpeechToTextResult, delegates
    │   ├── SpeechToTextSettings.h      # UDeveloperSettings (Project Settings UI)
    │   ├── SpeechToTextComponent.h     # Main actor component
    │   ├── AudioCaptureHandler.h       # Microphone capture + WAV encoding
    │   ├── WhisperAPIClient.h          # OpenAI Whisper HTTP client
    │   └── SpeechToTextWidget.h        # UMG widget for displaying transcription
    └── Private/
        ├── SpeechToTextModule.cpp
        ├── SpeechToTextSettings.cpp
        ├── SpeechToTextComponent.cpp
        ├── AudioCaptureHandler.cpp
        ├── WhisperAPIClient.cpp
        └── SpeechToTextWidget.cpp
```

### Module Dependencies

**Public:** Core, CoreUObject, Engine, InputCore, AudioCapture, AudioMixer
**Private:** HTTP, Json, JsonUtilities, UMG, Slate, SlateCore

## Usage

### Blueprint

1. **Add the component** to any Actor (player pawn, world object, etc.):
   - Select your Actor in the editor
   - Add Component > Speech To Text

2. **Configure overrides** (optional) in the Details panel:
   - API Key Override
   - Language Override (e.g. `en` for English)
   - Max Duration Override
   - Sample Rate Override

3. **Call functions** from your Blueprint:
   - `StartRecording` — begins microphone capture
   - `StopRecording` — stops capture, sends audio to Whisper API
   - `GetCurrentState` — returns the current `ESpeechToTextState`

4. **Bind delegates** to react to events:
   - `OnTranscriptionComplete` — fires with `FSpeechToTextResult` when done
   - `OnRecordingStateChanged` — fires on every state transition

### C++

```cpp
#include "SpeechToTextComponent.h"

// In your Actor's BeginPlay or wherever appropriate:
USpeechToTextComponent* STT = FindComponentByClass<USpeechToTextComponent>();

// Bind to transcription results
STT->OnTranscriptionComplete.AddDynamic(this, &AMyActor::OnTranscriptionReceived);

// Start/stop (push-to-talk)
STT->StartRecording();
// ... user speaks ...
STT->StopRecording();

// Handler
void AMyActor::OnTranscriptionReceived(const FSpeechToTextResult& Result)
{
    if (Result.bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("Player said: %s"), *Result.TranscribedText);
        // Use Result.TranscribedText here
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Transcription error: %s"), *Result.ErrorMessage);
    }
}
```

### VR Push-to-Talk Example

Bind `StartRecording`/`StopRecording` to a controller button:

```cpp
// In your VR Pawn's SetupPlayerInputComponent:
EnhancedInput->BindAction(PushToTalkAction, ETriggerEvent::Started, this, &AMyVRPawn::OnPTTPressed);
EnhancedInput->BindAction(PushToTalkAction, ETriggerEvent::Completed, this, &AMyVRPawn::OnPTTReleased);

void AMyVRPawn::OnPTTPressed()
{
    SpeechToTextComponent->StartRecording();
}

void AMyVRPawn::OnPTTReleased()
{
    SpeechToTextComponent->StopRecording();
}
```

## API Reference

### USpeechToTextComponent

The main actor component. Attach to any Actor.

#### Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `StartRecording()` | `void` | Begin microphone capture. Ignored if not in Idle state. |
| `StopRecording()` | `void` | Stop capture, send audio to Whisper. Ignored if not Recording. Recordings < 0.1s are rejected. |
| `GetCurrentState()` | `ESpeechToTextState` | Current state (Pure, no side effects). |

#### Delegates

| Delegate | Signature | Fires When |
|----------|-----------|------------|
| `OnTranscriptionComplete` | `(const FSpeechToTextResult& Result)` | Transcription finishes (success or failure) |
| `OnRecordingStateChanged` | `(ESpeechToTextState NewState)` | Any state transition occurs |

#### Properties (Per-Instance Overrides)

All optional. Leave empty/zero to use Project Settings defaults.

| Property | Type | Range | Description |
|----------|------|-------|-------------|
| `APIKeyOverride` | `FString` | — | OpenAI API key for this specific instance |
| `LanguageOverride` | `FString` | — | Language code override (e.g. `en`, `ja`, `vi`) |
| `MaxRecordingDurationOverride` | `float` | 0–120 s | Max recording duration |
| `SampleRateOverride` | `int32` | 0–48000 Hz | Mic sample rate |

### ESpeechToTextState

```
Idle        — Ready to record
Recording   — Microphone is capturing audio
Processing  — Audio sent to Whisper API, waiting for response
Error       — An error occurred (transitions back to Idle automatically)
```

### FSpeechToTextResult

| Field | Type | Description |
|-------|------|-------------|
| `TranscribedText` | `FString` | The transcribed text (empty on failure) |
| `Language` | `FString` | Language code used for transcription |
| `AudioDuration` | `float` | Duration of the recorded audio in seconds |
| `bSuccess` | `bool` | `true` if transcription succeeded |
| `ErrorMessage` | `FString` | Error description (empty on success) |

### USpeechToTextWidget

UMG widget for displaying transcription results in VR world space.

#### Setup

1. Create a Widget Blueprint that inherits from `SpeechToTextWidget`
2. Add two `TextBlock` widgets named:
   - **`StatusText`** (required) — shows "Ready" / "Recording..." / "Transcribing..." / "Error"
   - **`TranscriptionText`** (optional) — shows transcription history
3. Place the widget in your level via a `WidgetComponent` (set to World space for VR)

#### Functions

| Function | Description |
|----------|-------------|
| `BindToComponent(Component)` | Auto-bind to a `USpeechToTextComponent` for live updates |
| `UpdateTranscription(Result)` | Manually push a transcription result to the display |
| `UpdateState(NewState)` | Manually update the status indicator |
| `ClearHistory()` | Clear all displayed transcription text |

#### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `MaxHistoryLines` | `int32` | `5` | Number of transcription lines kept visible |

#### Status Colors

| State | Text | Color |
|-------|------|-------|
| Idle | "Ready" | White |
| Recording | "Recording..." | Red |
| Processing | "Transcribing..." | Yellow |
| Error | "Error" | Red |

## Internal Components

These are non-UObject utility classes used internally by `USpeechToTextComponent`. You don't interact with them directly, but they're documented here for maintainability.

### FAudioCaptureHandler

Handles microphone input and WAV encoding. Uses `Audio::FAudioCapture` directly (bypasses the audio engine to avoid VR echo/feedback).

- Captures raw float32 PCM samples from the microphone
- Thread-safe buffer with `FCriticalSection`
- Converts float32 [-1.0, 1.0] to int16 PCM [-32768, 32767]
- Generates a complete WAV file (44-byte RIFF header + PCM data)
- Auto-stops at `MaxDurationSeconds`
- Detects and logs buffer overflows

### FWhisperAPIClient

HTTP client for `POST https://api.openai.com/v1/audio/transcriptions`.

- Builds multipart/form-data request with WAV audio
- Sends to Whisper with model `whisper-1`
- Parses JSON response `{ "text": "..." }`
- Full error handling: network errors, HTTP errors, JSON parse errors
- Callbacks fire on the game thread (standard UE5 HTTP behavior)

## Data Flow

```
[Microphone] ──PCM float32──> [FAudioCaptureHandler]
                                       │
                               EncodeToWAV()
                                       │
                               WAV byte array
                                       │
                                       v
                              [FWhisperAPIClient]
                                       │
                           POST /v1/audio/transcriptions
                           (multipart/form-data)
                                       │
                                       v
                              [OpenAI Whisper API]
                                       │
                              JSON { "text": "..." }
                                       │
                                       v
                           [USpeechToTextComponent]
                                       │
                         OnTranscriptionComplete.Broadcast()
                                       │
                              ┌────────┴────────┐
                              v                 v
                        [Your Code]    [USpeechToTextWidget]
```

## State Machine

```
             StartRecording()
    [Idle] ──────────────────> [Recording]
      ^                            │
      │                    StopRecording() or
      │                    max duration reached
      │                            │
      │                            v
      │                      [Processing]
      │                            │
      │                  Whisper API responds
      │                            │
      └────────────────────────────┘

    [Error] ──(auto)──> [Idle]
```

## Troubleshooting

| Issue | Cause | Fix |
|-------|-------|-----|
| "No API key configured" warning | No key found at any level | Set key in Project Settings or `OPENAI_API_KEY` env var |
| "Failed to start microphone capture" | No mic connected or permissions denied | Connect a microphone; check OS audio permissions |
| "Recording too short" | Button released too quickly (< 0.1s) | Hold the push-to-talk button longer |
| "Network error" | No internet or OpenAI unreachable | Check internet connection and firewall |
| "API error (HTTP 401)" | Invalid API key | Verify your OpenAI API key is correct and active |
| "API error (HTTP 429)" | Rate limited | Wait and retry; check your OpenAI usage limits |
| Audio echo in VR | Using AudioEngine capture instead of FAudioCapture | The plugin already bypasses this; ensure you're using this plugin's component |

## Supported Languages

The plugin defaults to Vietnamese (`vi`), but any Whisper-supported language can be used via the Language Override. Common codes:

| Code | Language |
|------|----------|
| `vi` | Vietnamese |
| `en` | English |
| `ja` | Japanese |
| `ko` | Korean |
| `zh` | Chinese |
| `fr` | French |
| `de` | German |
| `es` | Spanish |

See the [OpenAI Whisper documentation](https://platform.openai.com/docs/guides/speech-to-text) for the full list.
