#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include <atomic>

namespace Audio { class FAudioCapture; }

/**
 * Handles microphone capture and WAV encoding.
 * Uses the low-level Audio::FAudioCapture API to record raw PCM samples
 * without routing through the audio engine (no echo/feedback in VR).
 *
 * Supports two modes:
 * - Push-to-talk: StartCapture/StopCapture (manual control)
 * - Listening (VAD): StartListening continuously monitors the mic and
 *   buffers audio for external VAD processing (WebRTC via URuntimeVoiceActivityDetector).
 *   The component drives speech start/end via NotifySpeechStarted/Ended.
 */
class SPEECHTOTEXT_API FAudioCaptureHandler
{
public:
	FAudioCaptureHandler();
	~FAudioCaptureHandler();

	// --- Push-to-talk mode ---

	/** Start capturing microphone audio. Returns false if mic unavailable. */
	bool StartCapture(int32 InSampleRate = 16000);

	/** Stop capturing and finalize the buffer. */
	void StopCapture();

	// --- Listening (VAD) mode ---

	/** Start listening. Mic stays open, audio is buffered for external VAD processing. */
	bool StartListening(int32 InSampleRate = 16000);

	/** Stop listening. Also stops any in-progress recording. */
	void StopListening();

	/** Is the handler currently in listening mode? */
	bool IsListening() const { return bIsListening; }

	/**
	 * Drain buffered audio frames for external VAD processing.
	 * Call from game thread. Returns accumulated audio since last drain.
	 */
	TArray<float> DrainPendingAudio();

	/**
	 * Notify that external VAD detected speech onset.
	 * Copies pre-roll buffer into CapturedSamples and starts appending new audio.
	 * Call from game thread.
	 */
	void NotifySpeechStarted();

	/**
	 * Notify that external VAD detected end of speech.
	 * Stops appending audio and resets pre-roll for next utterance.
	 * Call from game thread.
	 */
	void NotifySpeechEnded();

	/** Has the max recording duration been reached? Check from game thread. */
	bool IsMaxDurationReached() const { return bMaxDurationReached; }

	/** Clear the max duration flag after handling it. */
	void ClearMaxDurationFlag() { bMaxDurationReached = false; }

	/** Get the current RMS energy level (useful for debug visualization). Thread-safe. */
	float GetCurrentRMSEnergy() const;

	/** Is the handler currently recording speech (between NotifySpeechStarted/Ended)? */
	bool IsVADSpeechActive() const { return bVADSpeechActive; }

	// --- Common ---

	/** Is the microphone currently capturing (push-to-talk mode)? */
	bool IsCapturing() const { return bIsCapturing; }

	/** Get the captured audio encoded as a WAV byte array. Call after StopCapture(). */
	TArray<uint8> GetWAVData() const;

	/** Get the duration of captured audio in seconds. */
	float GetCaptureDuration() const;

	/** Get the name of the default capture device (cached on construction). */
	FString GetDeviceName() const { return CachedDeviceName; }

	/** Get the number of available capture devices. */
	int32 GetNumAvailableDevices() const { return CachedNumDevices; }

	/** Get the actual device sample rate (may differ from requested rate). Available after first audio callback. */
	int32 GetCaptureSampleRate() const { return CaptureSampleRate; }

	/** Get the actual device channel count. Available after first audio callback. */
	int32 GetCaptureNumChannels() const { return CaptureNumChannels; }

	/** Maximum recording duration in seconds. Recording auto-stops after this. */
	float MaxDurationSeconds = 30.0f;

	/** Seconds of audio to retain before speech onset (pre-roll). */
	float PreRollSeconds = 0.8f;

private:
	void OnAudioCapture(const float* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow);
	void OnAudioCaptureListening(const float* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow);
	void EnumerateDevices();

	static float ComputeRMS(const float* Samples, int32 Count);
	static TArray<uint8> EncodeToWAV(const TArray<float>& Samples, int32 SampleRate, int32 NumChannels);

	bool OpenMicStream(int32 InSampleRate, bool bForListening);

	TUniquePtr<Audio::FAudioCapture> AudioCapture;
	TArray<float> CapturedSamples;
	mutable FCriticalSection BufferLock;

	int32 CaptureSampleRate = 16000;
	int32 CaptureNumChannels = 1;
	FThreadSafeBool bIsCapturing;
	FThreadSafeBool bIsListening;
	bool bReceivedFirstCallback = false;

	// --- VAD state (accessed from audio thread under BufferLock) ---
	TArray<float> PreRollBuffer;        // Circular buffer for pre-roll audio
	int32 PreRollWritePos = 0;          // Write position in circular buffer
	int32 PreRollCapacity = 0;          // Max samples in pre-roll buffer
	bool bPreRollFull = false;          // Has the ring buffer wrapped at least once

	TArray<float> PendingVADAudio;      // Audio frames buffered for game thread VAD

	FThreadSafeBool bVADSpeechActive;   // Is speech currently being captured
	FThreadSafeBool bMaxDurationReached; // Max duration hit on audio thread
	std::atomic<float> CurrentRMSEnergy{0.0f}; // Latest RMS for debug display

	FString CachedDeviceName = TEXT("(Not enumerated)");
	int32 CachedNumDevices = 0;
};
