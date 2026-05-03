# SpeechToText — Unreal Engine 5 Plugin

Push-to-talk and hands-free (VAD) speech-to-text for Unreal Engine 5.6, powered by the OpenAI Whisper API. Captures microphone audio, encodes it to WAV, and returns transcribed text via Blueprint-compatible delegates. Designed for VR — bypasses the audio engine to prevent echo/feedback.

## Features

- **Push-to-talk** — `StartRecording` / `StopRecording` from Blueprint or C++
- **Hands-free (VAD)** — WebRTC-based voice activity detection via `libfvad` (RuntimeAudioImporter); auto-detects speech onset and silence
- **Direct audio streaming** — stream PCM audio directly to an `NPCConversationComponent` (GeminiLive backend) for ultra-low-latency voice chat without a separate STT step
- **Multi-language** — defaults to Vietnamese (`vi`); any Whisper-supported language via override
- **Project Settings UI** — configure API key, language, model, VAD thresholds, pre-roll buffer from `Edit → Project Settings → Plugins → Speech To Text`
- **Per-instance overrides** — API key, language, model, duration, sample rate settable on each component
- **Android support** — runtime microphone permission handling, Oboe stream lifecycle recovery
- **UMG debug widget** — `USpeechToTextWidget` for in-world status/history display

## Requirements

| Requirement | Version |
|-------------|---------|
| Unreal Engine | 5.6+ |
| OpenAI API key | Whisper access required |
| Engine plugin: AudioCapture | Bundled with UE5 |
| Marketplace plugin: RuntimeAudioImporter | For WebRTC VAD |

## Quick Start

### 1. Add to your project

Copy the `SpeechToText` folder into your project's `Plugins/` directory, then add to your `.uproject`:

```json
{
    "Name": "SpeechToText",
    "Enabled": true
}
```

Ensure `AudioCapture` and `RuntimeAudioImporter` are also enabled.

### 2. Set your API key

Navigate to **Edit → Project Settings → Plugins → Speech To Text** and enter your OpenAI API key. Alternatively, set the `OPENAI_API_KEY` environment variable, or use a per-component override in the Details panel.

### 3. Add the component

Add a `SpeechToTextComponent` to any Actor. Wire up delegates and call `StartRecording` / `StopRecording`:

```cpp
USpeechToTextComponent* STT = FindComponentByClass<USpeechToTextComponent>();
STT->OnTranscriptionComplete.AddDynamic(this, &AMyActor::OnTranscription);

STT->StartRecording();   // user presses button
STT->StopRecording();    // user releases button

void AMyActor::OnTranscription(const FSpeechToTextResult& Result)
{
    if (Result.bSuccess)
        UE_LOG(LogTemp, Log, TEXT("%s"), *Result.TranscribedText);
}
```

For hands-free mode:

```cpp
STT->EnableHandsFreeMode();   // mic stays open, VAD handles start/stop
// ...
STT->DisableHandsFreeMode();
```

## Documentation

Full setup, configuration, API reference, state machine, and troubleshooting guide: **[USAGE.md](USAGE.md)**

Architecture, class diagram, threading model, and data flow: **[ARCHITECTURE.md](ARCHITECTURE.md)**

## License

Internal plugin — see your project's license terms.
