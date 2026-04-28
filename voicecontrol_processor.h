#pragma once

#include <cstdint>
#include <vector>
#include <opus/opus.h>
#include "CRC.h"
#include "defines.h"

struct VoiceControlSettings
{
	float manualGainDb = 0.0f;
	float duckGainDb = 0.0f;
	bool agcEnabled = true;
	float agcTargetRms = 0.12f;
	float agcNoiseFloorRms = 0.0015f;
	float agcMaxBoostDb = 18.0f;
	float agcMaxCutDb = -12.0f;
	float limiterCeiling = 0.95f;
	bool dspEnabled = true;
	bool highpassEnabled = true;
	float highpassCutoffHz = 100.0f;
	bool noiseGateEnabled = true;
	float noiseGateThresholdRms = 0.002f;
	float noiseGateHysteresisRms = 0.001f;
	float noiseGateAttenDb = -8.0f;
	float noiseGateAttackMs = 5.0f;
	float noiseGateReleaseMs = 120.0f;
	bool softClipEnabled = true;
	float softClipThreshold = 0.85f;
	int preset = 0;
};

struct VoiceControlDebugInfo
{
	int inputBytes = 0;
	int outputBytes = 0;
	int chunkCount = 0;
	int sampleCount = 0;
	opus_int32 sampleRate = 0;
	float inputRms = 0.0f;
	float outputRms = 0.0f;
	float manualGainDb = 0.0f;
	float agcGainDb = 0.0f;
	float duckGainDb = 0.0f;
	float finalGainDb = 0.0f;
	bool dspEnabled = false;
	bool highpassEnabled = false;
	bool gateOpen = true;
	float gateGainDb = 0.0f;
	float rmsAfterHighpass = 0.0f;
	float rmsAfterGate = 0.0f;
	float inputPeak = 0.0f;
	float outputPeak = 0.0f;
	int clipCount = 0;
	bool softClipEnabled = false;
	int preset = 0;
};

enum class VoiceEffectType
{
	HighPass,
	NoiseGate,
	Gain,
	AutoLevel,
	Limiter
};

struct VoiceEffectChain
{
	bool gain = true;
	bool autoLevel = true;
	bool limiter = true;
};

class VoiceControlProcessor
{
public:
	VoiceControlProcessor();
	~VoiceControlProcessor();

	bool Process(const uint8_t* data, int nBytes, const VoiceControlSettings& settings, std::vector<uint8_t>& output, VoiceControlDebugInfo* debugInfo = nullptr);
	void Reset();

private:
	struct EncodedChunk
	{
		int16_t index = 0;
		std::vector<uint8_t> data;
	};

	struct DecodedChunk
	{
		int16_t index = 0;
		std::vector<int16_t> data;
	};

	bool ParseSteamVoicePacket(const uint8_t* data, int nBytes, opus_int32& sampleRate, std::vector<EncodedChunk>& chunks);
	bool EnsureCodec(opus_int32 sampleRate);
	bool DecodeChunks(const std::vector<EncodedChunk>& encodedChunks, std::vector<DecodedChunk>& decodedChunks);
	bool EncodeChunks(const std::vector<DecodedChunk>& decodedChunks, std::vector<EncodedChunk>& encodedChunks);
	bool RebuildSteamVoicePacket(const uint8_t* original, int originalBytes, const std::vector<EncodedChunk>& chunks, std::vector<uint8_t>& output);

	float CalculateRms(const std::vector<DecodedChunk>& chunks);
	float CalculatePeak(const std::vector<DecodedChunk>& chunks);
	int CountSamples(const std::vector<DecodedChunk>& chunks);
	float CalculateAgcDb(float rms, const VoiceControlSettings& settings);
	void ApplyHighPass(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings);
	float ApplyNoiseGate(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings, float detectorRms);
	int ApplyGainAndLimiter(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings, float gainDb);
	static float SoftClip(float value, float threshold, float ceiling);
	static float ClampFloat(float value, float minValue, float maxValue);
	static float DbToGain(float db);

	static bool ReadInt16(const uint8_t* data, int pos, int limit, int16_t& value);
	static bool ReadUInt32(const uint8_t* data, int pos, int limit, uint32_t& value);
	static const char* OpusErrorToString(int error);

	OpusDecoder* m_decoder = nullptr;
	OpusEncoder* m_encoder = nullptr;
	opus_int32 m_sampleRate = 0;
	float m_smoothedAgcDb = 0.0f;
	bool m_hasAgcState = false;
	float m_highpassPrevInput = 0.0f;
	float m_highpassPrevOutput = 0.0f;
	float m_gateGainDb = 0.0f;
	bool m_gateOpen = true;
	bool m_hasGateState = false;
};
