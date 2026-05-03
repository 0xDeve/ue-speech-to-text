#include "AudioCaptureHandler.h"
#include "AudioCaptureCore.h"

FAudioCaptureHandler::FAudioCaptureHandler()
	: bIsCapturing(false)
	, bIsListening(false)
	, bVADSpeechActive(false)
	, bMaxDurationReached(false)
{
	EnumerateDevices();
}

FAudioCaptureHandler::~FAudioCaptureHandler()
{
	if (bIsListening)
	{
		StopListening();
	}
	else if (bIsCapturing)
	{
		StopCapture();
	}
}

// ── Shared mic open helper ─────────────────────────────────────────────────────

bool FAudioCaptureHandler::OpenMicStream(int32 InSampleRate, bool bForListening)
{
	CaptureSampleRate = InSampleRate;
	CaptureNumChannels = 1;
	bReceivedFirstCallback = false;

	{
		FScopeLock Lock(&BufferLock);
		CapturedSamples.Empty();
	}

	AudioCapture = MakeUnique<Audio::FAudioCapture>();

	Audio::FAudioCaptureDeviceParams Params;
	Params.NumInputChannels = CaptureNumChannels;

	auto CaptureCallback = [this, bForListening](const float* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
	{
		if (bForListening)
		{
			OnAudioCaptureListening(InAudio, NumFrames, NumChannels, SampleRate, StreamTime, bOverflow);
		}
		else
		{
			OnAudioCapture(InAudio, NumFrames, NumChannels, SampleRate, StreamTime, bOverflow);
		}
	};

	constexpr uint32 NumFramesDesired = 1024;
	if (!AudioCapture->OpenCaptureStream(Params, MoveTemp(CaptureCallback), NumFramesDesired))
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: Failed to open audio capture stream. Is a microphone connected?"));
		AudioCapture.Reset();
		return false;
	}

	if (!AudioCapture->StartStream())
	{
		UE_LOG(LogTemp, Error, TEXT("SpeechToText: Failed to start audio capture stream"));
		AudioCapture->CloseStream();
		AudioCapture.Reset();
		return false;
	}

	return true;
}

// ── Push-to-talk mode ──────────────────────────────────────────────────────────

bool FAudioCaptureHandler::StartCapture(int32 InSampleRate)
{
	if (bIsCapturing)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Already capturing audio"));
		return false;
	}

	if (bIsListening)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot start push-to-talk capture while listening"));
		return false;
	}

	if (!OpenMicStream(InSampleRate, false))
	{
		return false;
	}

	bIsCapturing = true;
	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Microphone capture started (requested %d Hz)"), InSampleRate);
	return true;
}

void FAudioCaptureHandler::StopCapture()
{
	if (!bIsCapturing)
	{
		return;
	}

	bIsCapturing = false;

	if (AudioCapture)
	{
		AudioCapture->StopStream();
		AudioCapture->CloseStream();
		AudioCapture.Reset();
	}

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Microphone capture stopped (%.1f seconds captured)"), GetCaptureDuration());
}

void FAudioCaptureHandler::OnAudioCapture(const float* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
{
	if (!bIsCapturing)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// On first callback, capture the actual device sample rate and channel count
	if (!bReceivedFirstCallback)
	{
		bReceivedFirstCallback = true;
		CaptureSampleRate = SampleRate;
		CaptureNumChannels = NumChannels;

		// Pre-allocate now that we know the real rate
		CapturedSamples.Reserve(CaptureSampleRate * static_cast<int32>(MaxDurationSeconds) * CaptureNumChannels);

		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Actual device capture format: %d Hz, %d ch"), SampleRate, NumChannels);
	}

	// Update RMS energy for debug visualization during push-to-talk
	const float RMS = ComputeRMS(InAudio, NumFrames * NumChannels);
	CurrentRMSEnergy.store(RMS);

	const int32 MaxSamples = CaptureSampleRate * static_cast<int32>(MaxDurationSeconds) * CaptureNumChannels;
	const int32 SamplesToAdd = FMath::Min(NumFrames * NumChannels, MaxSamples - CapturedSamples.Num());

	if (SamplesToAdd > 0)
	{
		CapturedSamples.Append(InAudio, SamplesToAdd);
	}

	if (bOverflow)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Audio capture buffer overflow detected"));
	}
}

// ── Listening (VAD) mode ───────────────────────────────────────────────────────

bool FAudioCaptureHandler::StartListening(int32 InSampleRate)
{
	if (bIsListening)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Already listening"));
		return false;
	}

	if (bIsCapturing)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: Cannot start listening while push-to-talk is active"));
		return false;
	}

	// Reset state
	{
		FScopeLock Lock(&BufferLock);
		PreRollBuffer.Empty();
		PreRollWritePos = 0;
		PreRollCapacity = 0;
		bPreRollFull = false;
		CapturedSamples.Empty();
		PendingVADAudio.Empty();
	}
	bVADSpeechActive = false;
	bMaxDurationReached = false;
	CurrentRMSEnergy.store(0.0f);

	if (!OpenMicStream(InSampleRate, true))
	{
		return false;
	}

	bIsListening = true;
	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Listening started (pre-roll=%.2fs)"), PreRollSeconds);
	return true;
}

void FAudioCaptureHandler::StopListening()
{
	if (!bIsListening)
	{
		return;
	}

	bIsListening = false;
	bVADSpeechActive = false;

	if (AudioCapture)
	{
		AudioCapture->StopStream();
		AudioCapture->CloseStream();
		AudioCapture.Reset();
	}

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Listening stopped"));
}

void FAudioCaptureHandler::OnAudioCaptureListening(const float* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
{
	if (!bIsListening)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// First callback: initialize device format and pre-roll buffer
	if (!bReceivedFirstCallback)
	{
		bReceivedFirstCallback = true;
		CaptureSampleRate = SampleRate;
		CaptureNumChannels = NumChannels;

		PreRollCapacity = FMath::Max(1, static_cast<int32>(PreRollSeconds * SampleRate * NumChannels));
		PreRollBuffer.SetNumZeroed(PreRollCapacity);
		PreRollWritePos = 0;
		bPreRollFull = false;

		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Device format: %d Hz, %d ch (pre-roll: %d samples / %.2fs)"),
			SampleRate, NumChannels, PreRollCapacity, PreRollSeconds);
	}

	const int32 TotalSamples = NumFrames * NumChannels;

	// Update RMS energy for debug visualization
	const float RMS = ComputeRMS(InAudio, TotalSamples);
	CurrentRMSEnergy.store(RMS);

	// Buffer audio for game thread VAD processing
	PendingVADAudio.Append(InAudio, TotalSamples);

	if (!bVADSpeechActive)
	{
		// --- Not yet speaking: write to pre-roll ring buffer ---
		for (int32 i = 0; i < TotalSamples; ++i)
		{
			PreRollBuffer[PreRollWritePos] = InAudio[i];
			PreRollWritePos++;
			if (PreRollWritePos >= PreRollCapacity)
			{
				PreRollWritePos = 0;
				bPreRollFull = true;
			}
		}
	}
	else
	{
		// --- Currently recording speech: append to captured samples ---
		const int32 MaxSamples = CaptureSampleRate * static_cast<int32>(MaxDurationSeconds) * CaptureNumChannels;
		const int32 SamplesToAdd = FMath::Min(TotalSamples, MaxSamples - CapturedSamples.Num());

		if (SamplesToAdd > 0)
		{
			CapturedSamples.Append(InAudio, SamplesToAdd);
		}

		// Check max duration
		if (CapturedSamples.Num() >= MaxSamples)
		{
			bVADSpeechActive = false;
			bMaxDurationReached = true;

			PreRollWritePos = 0;
			bPreRollFull = false;
			if (PreRollCapacity > 0)
			{
				FMemory::Memzero(PreRollBuffer.GetData(), PreRollCapacity * sizeof(float));
			}

			UE_LOG(LogTemp, Log, TEXT("SpeechToText: Max duration reached (%.0f s)"), MaxDurationSeconds);
		}
	}
}

// ── Game thread VAD interface ──────────────────────────────────────────────────

TArray<float> FAudioCaptureHandler::DrainPendingAudio()
{
	FScopeLock Lock(&BufferLock);
	TArray<float> Result = MoveTemp(PendingVADAudio);
	PendingVADAudio.Empty();
	return Result;
}

void FAudioCaptureHandler::NotifySpeechStarted()
{
	FScopeLock Lock(&BufferLock);

	if (bVADSpeechActive)
	{
		return; // Already recording
	}

	bVADSpeechActive = true;

	// Pre-allocate for max duration
	const int32 MaxSamples = CaptureSampleRate * static_cast<int32>(MaxDurationSeconds) * CaptureNumChannels;
	CapturedSamples.Empty();
	CapturedSamples.Reserve(MaxSamples);

	// Copy pre-roll (ring buffer) into CapturedSamples in correct order
	if (bPreRollFull)
	{
		CapturedSamples.Append(PreRollBuffer.GetData() + PreRollWritePos, PreRollCapacity - PreRollWritePos);
		CapturedSamples.Append(PreRollBuffer.GetData(), PreRollWritePos);
	}
	else
	{
		CapturedSamples.Append(PreRollBuffer.GetData(), PreRollWritePos);
	}

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Speech started (pre-roll: %d samples / %.2fs)"),
		CapturedSamples.Num(),
		(CaptureSampleRate > 0 && CaptureNumChannels > 0)
			? static_cast<float>(CapturedSamples.Num()) / static_cast<float>(CaptureSampleRate * CaptureNumChannels)
			: 0.0f);
}

void FAudioCaptureHandler::NotifySpeechEnded()
{
	FScopeLock Lock(&BufferLock);

	if (!bVADSpeechActive)
	{
		return;
	}

	bVADSpeechActive = false;

	// Reset pre-roll for next utterance
	PreRollWritePos = 0;
	bPreRollFull = false;
	if (PreRollCapacity > 0)
	{
		FMemory::Memzero(PreRollBuffer.GetData(), PreRollCapacity * sizeof(float));
	}

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Speech ended (%.1f seconds captured)"), GetCaptureDuration());
}

// ── Utilities ──────────────────────────────────────────────────────────────────

float FAudioCaptureHandler::ComputeRMS(const float* Samples, int32 Count)
{
	if (Count <= 0) return 0.0f;

	float SumSquares = 0.0f;
	for (int32 i = 0; i < Count; ++i)
	{
		SumSquares += Samples[i] * Samples[i];
	}
	return FMath::Sqrt(SumSquares / static_cast<float>(Count));
}

float FAudioCaptureHandler::GetCurrentRMSEnergy() const
{
	return CurrentRMSEnergy.load();
}

void FAudioCaptureHandler::EnumerateDevices()
{
	Audio::FAudioCapture TempCapture;

	TArray<Audio::FCaptureDeviceInfo> Devices;
	CachedNumDevices = TempCapture.GetCaptureDevicesAvailable(Devices);

	UE_LOG(LogTemp, Log, TEXT("SpeechToText: Found %d audio capture device(s):"), CachedNumDevices);
	for (int32 i = 0; i < Devices.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\" (ID: %s, Channels: %d, SampleRate: %d)"),
			i, *Devices[i].DeviceName, *Devices[i].DeviceId, Devices[i].InputChannels, Devices[i].PreferredSampleRate);
	}

	// Get the default device info
	Audio::FCaptureDeviceInfo DefaultDevice;
	if (TempCapture.GetCaptureDeviceInfo(DefaultDevice))
	{
		CachedDeviceName = DefaultDevice.DeviceName;
		UE_LOG(LogTemp, Log, TEXT("SpeechToText: Default capture device: \"%s\""), *CachedDeviceName);
	}
	else
	{
		CachedDeviceName = TEXT("(No device found)");
		UE_LOG(LogTemp, Warning, TEXT("SpeechToText: No default capture device found!"));
	}
}

float FAudioCaptureHandler::GetCaptureDuration() const
{
	FScopeLock Lock(&BufferLock);
	if (CaptureSampleRate <= 0 || CaptureNumChannels <= 0)
	{
		return 0.0f;
	}
	return static_cast<float>(CapturedSamples.Num()) / static_cast<float>(CaptureSampleRate * CaptureNumChannels);
}

TArray<uint8> FAudioCaptureHandler::GetWAVData() const
{
	FScopeLock Lock(&BufferLock);
	return EncodeToWAV(CapturedSamples, CaptureSampleRate, CaptureNumChannels);
}

TArray<uint8> FAudioCaptureHandler::EncodeToWAV(const TArray<float>& Samples, int32 SampleRate, int32 NumChannels)
{
	// Convert float32 [-1.0, 1.0] to int16 [-32768, 32767]
	TArray<int16> PCM16;
	PCM16.SetNum(Samples.Num());
	for (int32 i = 0; i < Samples.Num(); i++)
	{
		const float Clamped = FMath::Clamp(Samples[i], -1.0f, 1.0f);
		PCM16[i] = static_cast<int16>(Clamped * 32767.0f);
	}

	const int32 DataSize = PCM16.Num() * sizeof(int16);
	const int32 FileSize = 44 + DataSize; // 44-byte WAV header + PCM data

	TArray<uint8> WAV;
	WAV.SetNum(FileSize);
	uint8* Ptr = WAV.GetData();

	auto WriteBytes = [&Ptr](const void* Src, int32 Size)
	{
		FMemory::Memcpy(Ptr, Src, Size);
		Ptr += Size;
	};

	auto WriteInt32 = [&WriteBytes](int32 Value) { WriteBytes(&Value, 4); };
	auto WriteInt16 = [&WriteBytes](int16 Value) { WriteBytes(&Value, 2); };

	// RIFF header
	WriteBytes("RIFF", 4);
	WriteInt32(FileSize - 8);       // ChunkSize
	WriteBytes("WAVE", 4);

	// fmt subchunk
	WriteBytes("fmt ", 4);
	WriteInt32(16);                  // SubChunk1Size (PCM = 16)
	WriteInt16(1);                   // AudioFormat (PCM = 1)
	WriteInt16(static_cast<int16>(NumChannels));
	WriteInt32(SampleRate);
	WriteInt32(SampleRate * NumChannels * sizeof(int16)); // ByteRate
	WriteInt16(static_cast<int16>(NumChannels * sizeof(int16))); // BlockAlign
	WriteInt16(16);                  // BitsPerSample

	// data subchunk
	WriteBytes("data", 4);
	WriteInt32(DataSize);
	WriteBytes(PCM16.GetData(), DataSize);

	return WAV;
}
