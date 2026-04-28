#include "voicecontrol_processor.h"
#include "smsdk_ext.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace
{
constexpr float kPi = 3.14159265358979323846f;
}

VoiceControlProcessor::VoiceControlProcessor() = default;

VoiceControlProcessor::~VoiceControlProcessor()
{
	Reset();
}

void VoiceControlProcessor::Reset()
{
	if (m_decoder)
	{
		opus_decoder_destroy(m_decoder);
		m_decoder = nullptr;
	}

	if (m_encoder)
	{
		opus_encoder_destroy(m_encoder);
		m_encoder = nullptr;
	}

	m_sampleRate = 0;
	m_smoothedAgcDb = 0.0f;
	m_hasAgcState = false;
	m_highpassPrevInput = 0.0f;
	m_highpassPrevOutput = 0.0f;
	m_gateGainDb = 0.0f;
	m_gateOpen = true;
	m_hasGateState = false;
}

bool VoiceControlProcessor::Process(const uint8_t* data, int nBytes, const VoiceControlSettings& settings, std::vector<uint8_t>& output, VoiceControlDebugInfo* debugInfo)
{
	if (!data || nBytes <= 0 || nBytes > MAX_PACKET_SIZE)
	{
		return false;
	}

	if (debugInfo)
	{
		*debugInfo = VoiceControlDebugInfo();
		debugInfo->inputBytes = nBytes;
		debugInfo->manualGainDb = settings.manualGainDb;
		debugInfo->duckGainDb = settings.duckGainDb;
		debugInfo->dspEnabled = settings.dspEnabled;
		debugInfo->highpassEnabled = settings.dspEnabled && settings.highpassEnabled;
		debugInfo->softClipEnabled = settings.softClipEnabled;
		debugInfo->preset = settings.preset;
	}

	opus_int32 sampleRate = 0;
	std::vector<EncodedChunk> encodedChunks;
	if (!ParseSteamVoicePacket(data, nBytes, sampleRate, encodedChunks) || encodedChunks.empty())
	{
		return false;
	}

	if (debugInfo)
	{
		debugInfo->sampleRate = sampleRate;
		debugInfo->chunkCount = static_cast<int>(encodedChunks.size());
	}

	if (!EnsureCodec(sampleRate))
	{
		return false;
	}

	std::vector<DecodedChunk> decodedChunks;
	if (!DecodeChunks(encodedChunks, decodedChunks))
	{
		return false;
	}

	float inputRms = CalculateRms(decodedChunks);
	float inputPeak = CalculatePeak(decodedChunks);
	float rmsAfterHighpass = inputRms;
	float rmsAfterGate = inputRms;
	float gateGainDb = 0.0f;

	if (settings.dspEnabled)
	{
		if (settings.highpassEnabled)
		{
			ApplyHighPass(decodedChunks, settings);
			rmsAfterHighpass = CalculateRms(decodedChunks);
			rmsAfterGate = rmsAfterHighpass;
		}

		if (settings.noiseGateEnabled)
		{
			gateGainDb = ApplyNoiseGate(decodedChunks, settings, rmsAfterHighpass);
			rmsAfterGate = CalculateRms(decodedChunks);
		}
	}

	float agcGainDb = 0.0f;
	float finalGainDb = settings.manualGainDb;
	if (settings.agcEnabled)
	{
		agcGainDb = CalculateAgcDb(rmsAfterHighpass, settings);
		finalGainDb += agcGainDb;
	}
	else
	{
		m_hasAgcState = false;
		m_smoothedAgcDb = 0.0f;
	}

	finalGainDb += settings.duckGainDb;
	int clipCount = ApplyGainAndLimiter(decodedChunks, settings, finalGainDb);

	if (debugInfo)
	{
		debugInfo->inputRms = inputRms;
		debugInfo->outputRms = CalculateRms(decodedChunks);
		debugInfo->inputPeak = inputPeak;
		debugInfo->outputPeak = CalculatePeak(decodedChunks);
		debugInfo->sampleCount = CountSamples(decodedChunks);
		debugInfo->agcGainDb = agcGainDb;
		debugInfo->finalGainDb = finalGainDb;
		debugInfo->gateGainDb = gateGainDb;
		debugInfo->gateOpen = m_gateOpen;
		debugInfo->rmsAfterHighpass = rmsAfterHighpass;
		debugInfo->rmsAfterGate = rmsAfterGate;
		debugInfo->clipCount = clipCount;
	}

	std::vector<EncodedChunk> processedChunks;
	if (!EncodeChunks(decodedChunks, processedChunks))
	{
		return false;
	}

	bool rebuilt = RebuildSteamVoicePacket(data, nBytes, processedChunks, output);
	if (rebuilt && debugInfo)
	{
		debugInfo->outputBytes = static_cast<int>(output.size());
	}

	return rebuilt;
}

bool VoiceControlProcessor::ParseSteamVoicePacket(const uint8_t* data, int nBytes, opus_int32& sampleRate, std::vector<EncodedChunk>& chunks)
{
	if (nBytes < STEAM_HEADER_SIZE + 2 + CRC_SIZE)
	{
		return false;
	}

	const int dataLen = nBytes - CRC_SIZE;

	uint32_t expectedCrc = 0;
	if (!ReadUInt32(data, dataLen, nBytes, expectedCrc))
	{
		return false;
	}

	uint32_t actualCrc = CRC::Calculate(data, dataLen, CRC::CRC_32());
	if (actualCrc != expectedCrc)
	{
		return false;
	}

	uint32_t steamCommunity = 0;
	if (!ReadUInt32(data, 4, dataLen, steamCommunity) || steamCommunity != 0x1100001)
	{
		return false;
	}

	int pos = 8;
	while (pos < dataLen)
	{
		uint8_t payloadType = data[pos++];
		switch (payloadType)
		{
		case 11:
		{
			int16_t rate = 0;
			if (!ReadInt16(data, pos, dataLen, rate) || rate <= 0)
			{
				return false;
			}

			sampleRate = static_cast<opus_int32>(rate);
			pos += 2;
			break;
		}
		case 6:
		{
			if (sampleRate <= 0)
			{
				return false;
			}

			int16_t dataSize = 0;
			if (!ReadInt16(data, pos, dataLen, dataSize) || dataSize <= 0)
			{
				return false;
			}

			pos += 2;
			if (pos + dataSize > dataLen)
			{
				return false;
			}

			int chunkPos = pos;
			const int maxPos = pos + dataSize;
			while (chunkPos < maxPos)
			{
				int16_t encodedSize = 0;
				int16_t index = 0;
				if (!ReadInt16(data, chunkPos, maxPos, encodedSize) || encodedSize <= 0 || encodedSize > MAX_PACKET_SIZE)
				{
					return false;
				}

				chunkPos += 2;
				if (!ReadInt16(data, chunkPos, maxPos, index))
				{
					return false;
				}

				chunkPos += 2;
				if (chunkPos + encodedSize > maxPos)
				{
					return false;
				}

				EncodedChunk chunk;
				chunk.index = index;
				chunk.data.assign(data + chunkPos, data + chunkPos + encodedSize);
				chunks.push_back(chunk);
				chunkPos += encodedSize;
			}

			return true;
		}
		case 0:
			return true;
		default:
			return false;
		}
	}

	return false;
}

bool VoiceControlProcessor::EnsureCodec(opus_int32 sampleRate)
{
	if (sampleRate <= 0)
	{
		return false;
	}

	if (m_sampleRate == sampleRate && m_decoder && m_encoder)
	{
		return true;
	}

	Reset();
	m_sampleRate = sampleRate;

	int err = OPUS_OK;
	m_decoder = opus_decoder_create(sampleRate, CHANNELS, &err);
	if (err < 0 || !m_decoder)
	{
		smutils->LogError(myself, "[VoiceControl] opus_decoder_create failed: %s", OpusErrorToString(err));
		Reset();
		return false;
	}

	m_encoder = opus_encoder_create(sampleRate, CHANNELS, APPLICATION, &err);
	if (err < 0 || !m_encoder)
	{
		smutils->LogError(myself, "[VoiceControl] opus_encoder_create failed: %s", OpusErrorToString(err));
		Reset();
		return false;
	}

	const int complexity = 5;
	if (opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(BITRATE)) < 0 ||
		opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) < 0 ||
		opus_encoder_ctl(m_encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_SUPERWIDEBAND)) < 0 ||
		opus_encoder_ctl(m_encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS)) < 0 ||
		opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(complexity)) < 0)
	{
		smutils->LogError(myself, "[VoiceControl] failed to configure opus encoder");
		Reset();
		return false;
	}

	return true;
}

bool VoiceControlProcessor::DecodeChunks(const std::vector<EncodedChunk>& encodedChunks, std::vector<DecodedChunk>& decodedChunks)
{
	int16_t pcm[MAX_FRAMEBUFFER_SAMPLES];

	for (const EncodedChunk& encoded : encodedChunks)
	{
		int samples = opus_decode(m_decoder, encoded.data.data(), static_cast<opus_int32>(encoded.data.size()), pcm, MAX_FRAMEBUFFER_SAMPLES, 0);
		if (samples < 0)
		{
			smutils->LogError(myself, "[VoiceControl] opus_decode failed: %s", OpusErrorToString(samples));
			return false;
		}

		DecodedChunk decoded;
		decoded.index = encoded.index;
		decoded.data.assign(pcm, pcm + samples);
		decodedChunks.push_back(decoded);
	}

	return !decodedChunks.empty();
}

bool VoiceControlProcessor::EncodeChunks(const std::vector<DecodedChunk>& decodedChunks, std::vector<EncodedChunk>& encodedChunks)
{
	uint8_t buffer[MAX_PACKET_SIZE];

	for (const DecodedChunk& decoded : decodedChunks)
	{
		if (decoded.data.empty())
		{
			return false;
		}

		int compressedBytes = opus_encode(m_encoder, decoded.data.data(), static_cast<int>(decoded.data.size()), buffer, MAX_PACKET_SIZE);
		if (compressedBytes < 0)
		{
			smutils->LogError(myself, "[VoiceControl] opus_encode failed: %s", OpusErrorToString(compressedBytes));
			return false;
		}

		EncodedChunk encoded;
		encoded.index = decoded.index;
		encoded.data.assign(buffer, buffer + compressedBytes);
		encodedChunks.push_back(encoded);
	}

	return !encodedChunks.empty();
}

bool VoiceControlProcessor::RebuildSteamVoicePacket(const uint8_t* original, int originalBytes, const std::vector<EncodedChunk>& chunks, std::vector<uint8_t>& output)
{
	if (originalBytes < STEAM_HEADER_SIZE + 2 || originalBytes > MAX_PACKET_SIZE)
	{
		return false;
	}

	output.assign(MAX_PACKET_SIZE, 0);
	std::copy(original, original + originalBytes, output.begin());

	int encPos = STEAM_HEADER_SIZE + 2;
	for (const EncodedChunk& chunk : chunks)
	{
		if (chunk.data.empty() || chunk.data.size() > INT16_MAX)
		{
			return false;
		}

		if (encPos + 2 + 2 + static_cast<int>(chunk.data.size()) + CRC_SIZE > MAX_PACKET_SIZE)
		{
			return false;
		}

		int16_t encodedSize = static_cast<int16_t>(chunk.data.size());
		std::memcpy(&output[encPos], &encodedSize, sizeof(encodedSize));
		encPos += 2;

		std::memcpy(&output[encPos], &chunk.index, sizeof(chunk.index));
		encPos += 2;

		std::copy(chunk.data.begin(), chunk.data.end(), output.begin() + encPos);
		encPos += static_cast<int>(chunk.data.size());
	}

	int16_t voiceDataLength = static_cast<int16_t>(encPos - STEAM_HEADER_SIZE - 2);
	if (voiceDataLength <= 0)
	{
		return false;
	}

	std::memcpy(&output[STEAM_HEADER_SIZE], &voiceDataLength, sizeof(voiceDataLength));

	uint32_t crc = CRC::Calculate(output.data(), encPos, CRC::CRC_32());
	std::memcpy(&output[encPos], &crc, sizeof(crc));
	encPos += CRC_SIZE;

	output.resize(encPos);
	return true;
}

float VoiceControlProcessor::CalculateRms(const std::vector<DecodedChunk>& chunks)
{
	double sumSquares = 0.0;
	size_t sampleCount = 0;

	for (const DecodedChunk& chunk : chunks)
	{
		for (int16_t sample : chunk.data)
		{
			double normalized = static_cast<double>(sample) / 32768.0;
			sumSquares += normalized * normalized;
			sampleCount++;
		}
	}

	if (sampleCount == 0)
	{
		return 0.0f;
	}

	return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount)));
}

float VoiceControlProcessor::CalculatePeak(const std::vector<DecodedChunk>& chunks)
{
	float peak = 0.0f;
	for (const DecodedChunk& chunk : chunks)
	{
		for (int16_t sample : chunk.data)
		{
			float normalized = std::fabs(static_cast<float>(sample) / 32768.0f);
			peak = std::max(peak, normalized);
		}
	}

	return peak;
}

int VoiceControlProcessor::CountSamples(const std::vector<DecodedChunk>& chunks)
{
	size_t sampleCount = 0;
	for (const DecodedChunk& chunk : chunks)
	{
		sampleCount += chunk.data.size();
	}

	return sampleCount > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(sampleCount);
}

float VoiceControlProcessor::CalculateAgcDb(float rms, const VoiceControlSettings& settings)
{
	if (settings.agcNoiseFloorRms > 0.0f && rms < settings.agcNoiseFloorRms)
	{
		m_hasAgcState = false;
		m_smoothedAgcDb = 0.0f;
		return 0.0f;
	}

	if (rms <= 0.001f || settings.agcTargetRms <= 0.001f)
	{
		return m_hasAgcState ? m_smoothedAgcDb : 0.0f;
	}

	float desiredDb = 20.0f * std::log10(settings.agcTargetRms / rms);
	desiredDb = std::max(settings.agcMaxCutDb, std::min(settings.agcMaxBoostDb, desiredDb));

	if (!m_hasAgcState)
	{
		m_smoothedAgcDb = desiredDb;
		m_hasAgcState = true;
		return m_smoothedAgcDb;
	}

	const float smoothing = desiredDb < m_smoothedAgcDb ? 0.30f : 0.08f;
	m_smoothedAgcDb += (desiredDb - m_smoothedAgcDb) * smoothing;
	return m_smoothedAgcDb;
}

void VoiceControlProcessor::ApplyHighPass(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings)
{
	if (m_sampleRate <= 0)
	{
		return;
	}

	float cutoff = ClampFloat(settings.highpassCutoffHz, 20.0f, 300.0f);
	cutoff = std::min(cutoff, static_cast<float>(m_sampleRate) * 0.45f);
	if (cutoff <= 0.0f)
	{
		return;
	}

	const float dt = 1.0f / static_cast<float>(m_sampleRate);
	const float rc = 1.0f / (2.0f * kPi * cutoff);
	const float alpha = rc / (rc + dt);

	for (DecodedChunk& chunk : chunks)
	{
		for (int16_t& sample : chunk.data)
		{
			float input = static_cast<float>(sample) / 32768.0f;
			float output = alpha * (m_highpassPrevOutput + input - m_highpassPrevInput);
			m_highpassPrevInput = input;
			m_highpassPrevOutput = output;
			output = ClampFloat(output, -1.0f, 1.0f);
			sample = static_cast<int16_t>(ClampFloat(output * 32767.0f, -32768.0f, 32767.0f));
		}
	}
}

float VoiceControlProcessor::ApplyNoiseGate(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings, float detectorRms)
{
	float threshold = ClampFloat(settings.noiseGateThresholdRms, 0.0f, 1.0f);
	if (threshold <= 0.0f)
	{
		m_gateOpen = true;
		m_gateGainDb = 0.0f;
		m_hasGateState = true;
		return 0.0f;
	}

	float hysteresis = std::max(0.0f, settings.noiseGateHysteresisRms);
	if (!m_hasGateState)
	{
		m_gateOpen = detectorRms >= threshold;
		m_gateGainDb = m_gateOpen ? 0.0f : ClampFloat(settings.noiseGateAttenDb, -36.0f, 0.0f);
		m_hasGateState = true;
	}
	else if (m_gateOpen)
	{
		if (detectorRms < threshold)
		{
			m_gateOpen = false;
		}
	}
	else if (detectorRms > threshold + hysteresis)
	{
		m_gateOpen = true;
	}

	float targetDb = m_gateOpen ? 0.0f : ClampFloat(settings.noiseGateAttenDb, -36.0f, 0.0f);
	int sampleCount = CountSamples(chunks);
	float durationMs = m_sampleRate > 0 ? (static_cast<float>(sampleCount) * 1000.0f / static_cast<float>(m_sampleRate)) : 20.0f;
	float timeMs = targetDb > m_gateGainDb ? settings.noiseGateAttackMs : settings.noiseGateReleaseMs;
	timeMs = ClampFloat(timeMs, 1.0f, 2000.0f);
	float smoothing = 1.0f - std::exp(-durationMs / timeMs);
	smoothing = ClampFloat(smoothing, 0.0f, 1.0f);
	m_gateGainDb += (targetDb - m_gateGainDb) * smoothing;

	float gain = DbToGain(m_gateGainDb);
	for (DecodedChunk& chunk : chunks)
	{
		for (int16_t& sample : chunk.data)
		{
			float value = (static_cast<float>(sample) / 32768.0f) * gain;
			sample = static_cast<int16_t>(ClampFloat(value * 32767.0f, -32768.0f, 32767.0f));
		}
	}

	return m_gateGainDb;
}

int VoiceControlProcessor::ApplyGainAndLimiter(std::vector<DecodedChunk>& chunks, const VoiceControlSettings& settings, float gainDb)
{
	float gain = DbToGain(gainDb);
	float ceiling = ClampFloat(settings.limiterCeiling, 0.1f, 1.0f);
	float softClipThreshold = ClampFloat(settings.softClipThreshold, 0.1f, ceiling);
	int clipCount = 0;

	for (DecodedChunk& chunk : chunks)
	{
		for (int16_t& sample : chunk.data)
		{
			float value = (static_cast<float>(sample) / 32768.0f) * gain;
			if (std::fabs(value) > (settings.softClipEnabled ? softClipThreshold : ceiling))
			{
				clipCount++;
			}

			if (settings.softClipEnabled)
			{
				value = SoftClip(value, softClipThreshold, ceiling);
			}
			else
			{
				value = ClampFloat(value, -ceiling, ceiling);
			}

			sample = static_cast<int16_t>(ClampFloat(value * 32767.0f, -32768.0f, 32767.0f));
		}
	}

	return clipCount;
}

float VoiceControlProcessor::SoftClip(float value, float threshold, float ceiling)
{
	float absValue = std::fabs(value);
	if (absValue <= threshold)
	{
		return value;
	}

	float sign = value < 0.0f ? -1.0f : 1.0f;
	float range = std::max(0.001f, ceiling - threshold);
	float excess = (absValue - threshold) / range;
	float shaped = threshold + range * std::tanh(excess);
	return sign * ClampFloat(shaped, 0.0f, ceiling);
}

float VoiceControlProcessor::ClampFloat(float value, float minValue, float maxValue)
{
	return std::max(minValue, std::min(maxValue, value));
}

float VoiceControlProcessor::DbToGain(float db)
{
	return std::pow(10.0f, db / 20.0f);
}

bool VoiceControlProcessor::ReadInt16(const uint8_t* data, int pos, int limit, int16_t& value)
{
	if (pos < 0 || pos + static_cast<int>(sizeof(value)) > limit)
	{
		return false;
	}

	std::memcpy(&value, data + pos, sizeof(value));
	return true;
}

bool VoiceControlProcessor::ReadUInt32(const uint8_t* data, int pos, int limit, uint32_t& value)
{
	if (pos < 0 || pos + static_cast<int>(sizeof(value)) > limit)
	{
		return false;
	}

	std::memcpy(&value, data + pos, sizeof(value));
	return true;
}

const char* VoiceControlProcessor::OpusErrorToString(int error)
{
	switch (error)
	{
	case OPUS_OK:
		return "OK";
	case OPUS_BAD_ARG:
		return "Bad argument";
	case OPUS_BUFFER_TOO_SMALL:
		return "Buffer too small";
	case OPUS_INTERNAL_ERROR:
		return "Internal error";
	case OPUS_INVALID_PACKET:
		return "Invalid packet";
	case OPUS_UNIMPLEMENTED:
		return "Unimplemented";
	case OPUS_INVALID_STATE:
		return "Invalid state";
	case OPUS_ALLOC_FAIL:
		return "Allocation failed";
	default:
		return "Unknown error";
	}
}
