#include "voicecontrol_extension.h"
#include "inetmessage.h"
#include <algorithm>
#include <cmath>
#include <edict.h>
#include <engine/ICollideable.h>
#include <game/server/iplayerinfo.h>
#include <inetchannel.h>
#include <networkstringtabledefs.h>

ConVar vc_enabled("vc_enabled", "1", FCVAR_NONE, "Enables voice control processing");
ConVar vc_agc_enabled("vc_agc_enabled", "1", FCVAR_NONE, "Enables automatic voice level control");
ConVar vc_agc_target_rms("vc_agc_target_rms", "0.12", FCVAR_NONE, "Target normalized RMS for AGC");
ConVar vc_agc_noise_floor_rms("vc_agc_noise_floor_rms", "0.0015", FCVAR_NONE, "RMS below which AGC will not boost voice");
ConVar vc_agc_max_boost_db("vc_agc_max_boost_db", "18", FCVAR_NONE, "Maximum AGC boost in dB");
ConVar vc_agc_max_cut_db("vc_agc_max_cut_db", "-12", FCVAR_NONE, "Maximum AGC cut in dB");
ConVar vc_limiter_ceiling("vc_limiter_ceiling", "0.95", FCVAR_NONE, "Normalized limiter ceiling");
ConVar vc_dsp_enabled("vc_dsp_enabled", "1", FCVAR_NONE, "Enables lightweight voice cleanup DSP");
ConVar vc_highpass_enabled("vc_highpass_enabled", "1", FCVAR_NONE, "Enables low-rumble high-pass filtering");
ConVar vc_highpass_cutoff_hz("vc_highpass_cutoff_hz", "100", FCVAR_NONE, "High-pass cutoff frequency in Hz");
ConVar vc_noise_gate_enabled("vc_noise_gate_enabled", "1", FCVAR_NONE, "Enables soft noise gate/expander");
ConVar vc_noise_gate_threshold_rms("vc_noise_gate_threshold_rms", "0.002", FCVAR_NONE, "RMS threshold below which background noise is attenuated");
ConVar vc_noise_gate_hysteresis_rms("vc_noise_gate_hysteresis_rms", "0.001", FCVAR_NONE, "RMS hysteresis used to avoid noise gate chatter");
ConVar vc_noise_gate_atten_db("vc_noise_gate_atten_db", "-8", FCVAR_NONE, "Noise gate attenuation in dB when closed");
ConVar vc_noise_gate_attack_ms("vc_noise_gate_attack_ms", "5", FCVAR_NONE, "Noise gate opening time in milliseconds");
ConVar vc_noise_gate_release_ms("vc_noise_gate_release_ms", "120", FCVAR_NONE, "Noise gate closing time in milliseconds");
ConVar vc_softclip_enabled("vc_softclip_enabled", "1", FCVAR_NONE, "Enables soft clipping before the hard limiter ceiling");
ConVar vc_softclip_threshold("vc_softclip_threshold", "0.85", FCVAR_NONE, "Normalized soft clipping threshold");
ConVar vc_debug("vc_debug", "0", FCVAR_NONE, "Logs per-packet voice processing details to the server console");
ConVar vc_debug_recipients("vc_debug_recipients", "0", FCVAR_NONE, "Logs voice recipient scan details to the server console");
ConVar vc_respect_hearing("vc_respect_hearing", "1", FCVAR_NONE, "Uses engine IsHearingClient filtering when sending processed voice");
ConVar vc_include_sourcetv("vc_include_sourcetv", "0", FCVAR_NONE, "Includes SourceTV and Replay clients when sending processed voice");
ConVar vc_proximity_enabled("vc_proximity_enabled", "0", FCVAR_NONE, "Enables server-side proximity voice cutoff for live players");
ConVar vc_proximity_max_distance("vc_proximity_max_distance", "1200", FCVAR_NONE, "Maximum distance in Hammer units for proximity voice");
ConVar vc_proximity_falloff_enabled("vc_proximity_falloff_enabled", "1", FCVAR_NONE, "Enables server-side proximity voice volume falloff");
ConVar vc_proximity_full_volume_distance("vc_proximity_full_volume_distance", "300", FCVAR_NONE, "Distance in Hammer units where proximity voice stays at full volume");
ConVar vc_proximity_min_gain_db("vc_proximity_min_gain_db", "-24", FCVAR_NONE, "Minimum proximity voice gain at max distance");
ConVar vc_send_via_netchannel("vc_send_via_netchannel", "0", FCVAR_NONE, "Sends processed proximity voice through INetChannel with bVoice=true");

DECL_DETOUR(VC_SV_BroadcastVoiceData);

VoiceControlExt g_VoiceControl;
IGameConfig* g_pVoiceControlGameConf = nullptr;
ISDKTools* g_pVoiceControlSDKTools = nullptr;
IServer* g_pVoiceControlServer = nullptr;
INetworkStringTableContainer* g_pVoiceControlStringTables = nullptr;

static void Command_DumpStringTables(const CCommand& args);
static ConCommand vc_dump_stringtables_cmd("vc_dump_stringtables", Command_DumpStringTables, "Dumps network string table usage for VoiceControl diagnostics", FCVAR_NONE);

struct RuntimePlayerSettings
{
	float gainDb = 0.0f;
	bool enabled = false;
	float duckGainDb = 0.0f;
	bool duckEnabled = false;
	int preset = 0;
};

static std::array<VoiceControlProcessor, MAXPLAYERS + 1> g_processors;
static constexpr int kMaxProximityBuckets = 32;
static constexpr float kProximityBucketStepDb = 3.0f;
static std::array<std::array<VoiceControlProcessor, kMaxProximityBuckets>, MAXPLAYERS + 1> g_proximityProcessors;
static std::array<RuntimePlayerSettings, MAXPLAYERS + 1> g_playerSettings;

SMEXT_LINK(&g_VoiceControl);

static void ResetClientProcessors(int client)
{
	if (client < 1 || client > MAXPLAYERS)
	{
		return;
	}

	g_processors[client].Reset();
	for (VoiceControlProcessor& processor : g_proximityProcessors[client])
	{
		processor.Reset();
	}
}

static bool IsValidClientIndex(int client)
{
	return client >= 1 && client <= playerhelpers->GetMaxClients();
}

static bool HasMeaningfulGain(float gainDb)
{
	return std::fabs(gainDb) > 0.001f;
}

static bool IsProximityEnabled()
{
	return vc_proximity_enabled.GetBool() && vc_proximity_max_distance.GetFloat() > 0.0f;
}

static bool IsProximityFalloffEnabled()
{
	return IsProximityEnabled() && vc_proximity_falloff_enabled.GetBool();
}

static bool IsDspProcessingRequired()
{
	return vc_dsp_enabled.GetBool() && (vc_highpass_enabled.GetBool() || vc_noise_gate_enabled.GetBool());
}

static void ApplyPresetToSettings(int preset, VoiceControlSettings& settings)
{
	switch (preset)
	{
	case 1: // Quiet mic: allow more boost while keeping the gate gentle.
		settings.agcNoiseFloorRms = std::min(settings.agcNoiseFloorRms, 0.0010f);
		settings.agcMaxBoostDb = std::max(settings.agcMaxBoostDb, 20.0f);
		settings.noiseGateThresholdRms = std::min(settings.noiseGateThresholdRms, 0.0015f);
		settings.noiseGateAttenDb = std::max(settings.noiseGateAttenDb, -6.0f);
		break;
	case 2: // Noisy mic: avoid boosting background noise and gate harder.
		settings.dspEnabled = true;
		settings.highpassEnabled = true;
		settings.highpassCutoffHz = std::max(settings.highpassCutoffHz, 120.0f);
		settings.noiseGateEnabled = true;
		settings.noiseGateThresholdRms = std::max(settings.noiseGateThresholdRms, 0.0040f);
		settings.noiseGateAttenDb = std::min(settings.noiseGateAttenDb, -14.0f);
		settings.noiseGateReleaseMs = std::max(settings.noiseGateReleaseMs, 160.0f);
		settings.agcNoiseFloorRms = std::max(settings.agcNoiseFloorRms, 0.0035f);
		settings.agcMaxBoostDb = std::min(settings.agcMaxBoostDb, 12.0f);
		break;
	case 3: // Loud/clipping mic: reduce level and clamp peaks more gently.
		settings.manualGainDb -= 3.0f;
		settings.agcMaxBoostDb = std::min(settings.agcMaxBoostDb, 6.0f);
		settings.agcMaxCutDb = std::min(settings.agcMaxCutDb, -18.0f);
		settings.limiterCeiling = std::min(settings.limiterCeiling, 0.85f);
		settings.softClipEnabled = true;
		settings.softClipThreshold = std::min(settings.softClipThreshold, 0.75f);
		break;
	default:
		break;
	}

	settings.preset = preset;
}

static bool IsAudioProcessingRequired(int client)
{
	if (!IsValidClientIndex(client))
	{
		return false;
	}

	const RuntimePlayerSettings& settings = g_playerSettings[client];
	return vc_agc_enabled.GetBool() ||
		IsDspProcessingRequired() ||
		(settings.enabled && HasMeaningfulGain(settings.gainDb)) ||
		(settings.duckEnabled && HasMeaningfulGain(settings.duckGainDb)) ||
		settings.preset != 0;
}

static bool ShouldInterceptVoice(int client)
{
	if (!vc_enabled.GetBool() || !IsValidClientIndex(client))
	{
		return false;
	}

	return IsAudioProcessingRequired(client) || IsProximityEnabled();
}

static VoiceControlSettings BuildSettingsForClient(int client)
{
	VoiceControlSettings settings;
	settings.manualGainDb = g_playerSettings[client].enabled ? g_playerSettings[client].gainDb : 0.0f;
	settings.duckGainDb = g_playerSettings[client].duckEnabled ? g_playerSettings[client].duckGainDb : 0.0f;
	settings.agcEnabled = vc_agc_enabled.GetBool();
	settings.agcTargetRms = vc_agc_target_rms.GetFloat();
	settings.agcNoiseFloorRms = vc_agc_noise_floor_rms.GetFloat();
	settings.agcMaxBoostDb = vc_agc_max_boost_db.GetFloat();
	settings.agcMaxCutDb = vc_agc_max_cut_db.GetFloat();
	settings.limiterCeiling = vc_limiter_ceiling.GetFloat();
	settings.dspEnabled = vc_dsp_enabled.GetBool();
	settings.highpassEnabled = vc_highpass_enabled.GetBool();
	settings.highpassCutoffHz = vc_highpass_cutoff_hz.GetFloat();
	settings.noiseGateEnabled = vc_noise_gate_enabled.GetBool();
	settings.noiseGateThresholdRms = vc_noise_gate_threshold_rms.GetFloat();
	settings.noiseGateHysteresisRms = vc_noise_gate_hysteresis_rms.GetFloat();
	settings.noiseGateAttenDb = vc_noise_gate_atten_db.GetFloat();
	settings.noiseGateAttackMs = vc_noise_gate_attack_ms.GetFloat();
	settings.noiseGateReleaseMs = vc_noise_gate_release_ms.GetFloat();
	settings.softClipEnabled = vc_softclip_enabled.GetBool();
	settings.softClipThreshold = vc_softclip_threshold.GetFloat();
	ApplyPresetToSettings(g_playerSettings[client].preset, settings);
	return settings;
}

struct VoiceRecipientScan
{
	int candidates = 0;
	int connected = 0;
	int fake = 0;
	int sourcetv = 0;
	int replay = 0;
	int sourcetvSent = 0;
	int replaySent = 0;
	int nullClient = 0;
	int notHearing = 0;
	int tooFar = 0;
	int originFail = 0;
	int bypassDead = 0;
	int bypassObserver = 0;
	int liveDistanceChecked = 0;
	int recipients = -1;
	int bucketCount = 0;
	int sendOk = 0;
	int sendFail = 0;
	int netchanNull = 0;
	bool msgProximity = false;
	bool falloffEnabled = false;
	float minDistance = -1.0f;
	float maxDistance = 0.0f;
};

static void LogVoiceDebug(int client, int fromClientSlot, const VoiceControlDebugInfo& debugInfo, const VoiceRecipientScan& scan, const char* sendPath)
{
	const char* name = "unknown";
	IGamePlayer* player = playerhelpers->GetGamePlayer(client);
	if (player && player->IsConnected())
	{
		name = player->GetName();
	}

	META_CONPRINTF("[VoiceControl] audio speaker=%d slot=%d name=\"%s\" in_rms=%.4f rms_hp=%.4f rms_gate=%.4f out_rms=%.4f peak_in=%.4f peak_out=%.4f clips=%d dsp=%d hp=%d gate_open=%d gate_db=%.1fdb softclip=%d preset=%d manual=%.1fdb agc=%.1fdb duck=%.1fdb final=%.1fdb sr=%d chunks=%d samples=%d bytes=%d->%d\n",
		client,
		fromClientSlot,
		name,
		debugInfo.inputRms,
		debugInfo.rmsAfterHighpass,
		debugInfo.rmsAfterGate,
		debugInfo.outputRms,
		debugInfo.inputPeak,
		debugInfo.outputPeak,
		debugInfo.clipCount,
		debugInfo.dspEnabled ? 1 : 0,
		debugInfo.highpassEnabled ? 1 : 0,
		debugInfo.gateOpen ? 1 : 0,
		debugInfo.gateGainDb,
		debugInfo.softClipEnabled ? 1 : 0,
		debugInfo.preset,
		debugInfo.manualGainDb,
		debugInfo.agcGainDb,
		debugInfo.duckGainDb,
		debugInfo.finalGainDb,
		debugInfo.sampleRate,
		debugInfo.chunkCount,
		debugInfo.sampleCount,
		debugInfo.inputBytes,
		debugInfo.outputBytes);

	META_CONPRINTF("[VoiceControl] recipients speaker=%d slot=%d recipients=%d candidates=%d connected=%d fake=%d sourcetv=%d replay=%d sourcetv_sent=%d replay_sent=%d null=%d not_hearing=%d too_far=%d origin_fail=%d bypass_dead=%d bypass_observer=%d live_distance_checked=%d msg_proximity=%d falloff=%d bucket_count=%d min_dist=%.1f max_dist=%.1f send_ok=%d send_fail=%d netchan_null=%d send=%s\n",
		client,
		fromClientSlot,
		scan.recipients,
		scan.candidates,
		scan.connected,
		scan.fake,
		scan.sourcetv,
		scan.replay,
		scan.sourcetvSent,
		scan.replaySent,
		scan.nullClient,
		scan.notHearing,
		scan.tooFar,
		scan.originFail,
		scan.bypassDead,
		scan.bypassObserver,
		scan.liveDistanceChecked,
		scan.msgProximity ? 1 : 0,
		scan.falloffEnabled ? 1 : 0,
		scan.bucketCount,
		scan.minDistance,
		scan.maxDistance,
		scan.sendOk,
		scan.sendFail,
		scan.netchanNull,
		sendPath);
}

static bool SendVoiceControlDataMsg(int fromClientSlot, IClient* pToClient, uint8_t* data, int nBytes, int64 xuid, bool proximity, VoiceRecipientScan& scan)
{
	if (pToClient == nullptr)
	{
		scan.sendFail++;
		return false;
	}

	SVC_VoiceData msg;
	msg.m_bProximity = proximity;
	scan.msgProximity = proximity;
	msg.m_nLength = nBytes * 8;
	msg.m_xuid = xuid;
	msg.m_nFromClient = fromClientSlot;
	msg.m_DataOut = data;

	INetChannel* netChannel = pToClient->GetNetChannel();
	msg.SetNetChannel(netChannel);

	if (vc_send_via_netchannel.GetBool())
	{
		if (netChannel != nullptr)
		{
			bool sent = netChannel->SendNetMsg(msg, false, true);
			if (sent)
			{
				scan.sendOk++;
				return true;
			}
		}
		else
		{
			scan.netchanNull++;
		}
	}

	bool sent = pToClient->SendNetMsg(msg, false);
	sent ? scan.sendOk++ : scan.sendFail++;
	return sent;
}

static bool VoiceControlIsHearingClient(IClient* pToClient, int fromClientSlot)
{
	return pToClient != nullptr && pToClient->IsHearingClient(fromClientSlot);
}

struct StringTableUsage
{
	const char* name = "";
	int entries = 0;
	int maxEntries = 0;
	int entryBits = 0;
	long long userDataBytes = 0;
};

static void Command_DumpStringTables(const CCommand& args)
{
	if (g_pVoiceControlStringTables == nullptr)
	{
		META_CONPRINTF("[VoiceControl] Network string table container is not available.\n");
		return;
	}

	std::vector<StringTableUsage> usages;
	int tableCount = g_pVoiceControlStringTables->GetNumTables();
	META_CONPRINTF("[VoiceControl] Dumping %d network string tables:\n", tableCount);

	for (int tableIndex = 0; tableIndex < tableCount; tableIndex++)
	{
		INetworkStringTable* table = g_pVoiceControlStringTables->GetTable(tableIndex);
		if (table == nullptr)
		{
			continue;
		}

		StringTableUsage usage;
		usage.name = table->GetTableName();
		usage.entries = table->GetNumStrings();
		usage.maxEntries = table->GetMaxStrings();
		usage.entryBits = table->GetEntryBits();

		for (int entry = 0; entry < usage.entries; entry++)
		{
			int userDataLength = 0;
			const void* userData = table->GetStringUserData(entry, &userDataLength);
			if (userData != nullptr && userDataLength > 0)
			{
				usage.userDataBytes += userDataLength;
			}
		}

		usages.push_back(usage);
		META_CONPRINTF("[VoiceControl] stringtable name=\"%s\" entries=%d max=%d entry_bits=%d userdata_bytes=%lld\n",
			usage.name,
			usage.entries,
			usage.maxEntries,
			usage.entryBits,
			usage.userDataBytes);
	}

	std::sort(usages.begin(), usages.end(), [](const StringTableUsage& left, const StringTableUsage& right) {
		return left.userDataBytes > right.userDataBytes;
	});

	int heavyCount = std::min(5, static_cast<int>(usages.size()));
	META_CONPRINTF("[VoiceControl] Heaviest string tables by userdata bytes:\n");
	for (int i = 0; i < heavyCount; i++)
	{
		const StringTableUsage& usage = usages[i];
		META_CONPRINTF("[VoiceControl] heavy #%d name=\"%s\" entries=%d max=%d entry_bits=%d userdata_bytes=%lld\n",
			i + 1,
			usage.name,
			usage.entries,
			usage.maxEntries,
			usage.entryBits,
			usage.userDataBytes);
	}
}

static bool IsFiniteVector(const Vector& origin)
{
	return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
}

static IPlayerInfo* GetClientPlayerInfo(int client)
{
	IGamePlayer* pGamePlayer = playerhelpers->GetGamePlayer(client);
	if (pGamePlayer == nullptr || !pGamePlayer->IsConnected())
	{
		return nullptr;
	}

	return pGamePlayer->GetPlayerInfo();
}

static bool IsDeadOrObserverForProximity(IPlayerInfo* playerInfo, VoiceRecipientScan& scan)
{
	if (playerInfo == nullptr)
	{
		return false;
	}

	bool bypass = false;
	if (playerInfo->IsDead())
	{
		scan.bypassDead++;
		bypass = true;
	}

	if (playerInfo->IsObserver())
	{
		scan.bypassObserver++;
		bypass = true;
	}

	return bypass;
}

static bool GetLiveClientOrigin(int client, Vector& origin, VoiceRecipientScan& scan)
{
	IPlayerInfo* playerInfo = GetClientPlayerInfo(client);
	if (playerInfo == nullptr)
	{
		scan.originFail++;
		return false;
	}

	if (IsDeadOrObserverForProximity(playerInfo, scan))
	{
		return false;
	}

	origin = playerInfo->GetAbsOrigin();
	if (!IsFiniteVector(origin))
	{
		scan.originFail++;
		return false;
	}

	return true;
}

static float GetDistanceSqr(const Vector& a, const Vector& b)
{
	float dx = a.x - b.x;
	float dy = a.y - b.y;
	float dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

static float CalculateProximityGainDb(float distance)
{
	float maxDistance = std::max(1.0f, vc_proximity_max_distance.GetFloat());
	float fullDistance = std::max(0.0f, vc_proximity_full_volume_distance.GetFloat());
	float minGainDb = std::max(-60.0f, std::min(0.0f, vc_proximity_min_gain_db.GetFloat()));

	if (!IsProximityFalloffEnabled() || distance <= fullDistance || maxDistance <= fullDistance)
	{
		return 0.0f;
	}

	float t = (distance - fullDistance) / (maxDistance - fullDistance);
	t = std::max(0.0f, std::min(1.0f, t));
	return minGainDb * t;
}

static float QuantizeProximityGainDb(float gainDb)
{
	float minGainDb = std::max(-60.0f, std::min(0.0f, vc_proximity_min_gain_db.GetFloat()));
	float quantized = std::round(gainDb / kProximityBucketStepDb) * kProximityBucketStepDb;
	return std::max(minGainDb, std::min(0.0f, quantized));
}

static int GetProximityBucket(float gainDb)
{
	float positiveCutDb = std::max(0.0f, -gainDb);
	int bucket = static_cast<int>(std::round(positiveCutDb / kProximityBucketStepDb));
	return std::max(0, std::min(kMaxProximityBuckets - 1, bucket));
}

struct VoiceRecipient
{
	IClient* client = nullptr;
	bool sourcetv = false;
	bool replay = false;
	float distance = 0.0f;
	float gainDb = 0.0f;
	int bucket = 0;
};

static void UpdateDistanceStats(VoiceRecipientScan& scan, float distance)
{
	if (scan.minDistance < 0.0f || distance < scan.minDistance)
	{
		scan.minDistance = distance;
	}

	if (distance > scan.maxDistance)
	{
		scan.maxDistance = distance;
	}
}

static std::vector<VoiceRecipient> GetVoiceRecipients(int fromClientIndex, bool applyProximity, bool allowTvReplay, VoiceRecipientScan& scan)
{
	std::vector<VoiceRecipient> recipients;
	int fromClientSlot = fromClientIndex - 1;
	Vector speakerOrigin;
	bool hasSpeakerOrigin = !applyProximity || GetLiveClientOrigin(fromClientIndex, speakerOrigin, scan);
	float maxDistance = vc_proximity_max_distance.GetFloat();
	float maxDistanceSqr = maxDistance * maxDistance;

	for (int client = 1; client <= playerhelpers->GetMaxClients(); client++)
	{
		scan.candidates++;
		IGamePlayer* pGamePlayer = playerhelpers->GetGamePlayer(client);
		if (pGamePlayer == nullptr || !pGamePlayer->IsConnected())
		{
			continue;
		}

		scan.connected++;
		if (pGamePlayer->IsFakeClient())
		{
			if (pGamePlayer->IsSourceTV())
			{
				scan.sourcetv++;
				if (!allowTvReplay || !vc_include_sourcetv.GetBool())
				{
					scan.fake++;
					continue;
				}
			}
			else if (pGamePlayer->IsReplay())
			{
				scan.replay++;
				if (!allowTvReplay || !vc_include_sourcetv.GetBool())
				{
					scan.fake++;
					continue;
				}
			}
			else
			{
				scan.fake++;
				continue;
			}
		}

		IClient* pToClient = g_pVoiceControlServer->GetClient(client - 1);
		if (pToClient == nullptr)
		{
			scan.nullClient++;
			continue;
		}

		bool isTvOrReplay = pGamePlayer->IsSourceTV() || pGamePlayer->IsReplay();
		if (!isTvOrReplay && vc_respect_hearing.GetBool() && !VoiceControlIsHearingClient(pToClient, fromClientSlot))
		{
			scan.notHearing++;
			continue;
		}

		VoiceRecipient recipient;
		recipient.client = pToClient;
		recipient.sourcetv = pGamePlayer->IsSourceTV();
		recipient.replay = pGamePlayer->IsReplay();

		if (recipient.sourcetv)
		{
			scan.sourcetvSent++;
			recipients.push_back(recipient);
			continue;
		}

		if (recipient.replay)
		{
			scan.replaySent++;
			recipients.push_back(recipient);
			continue;
		}

		if (applyProximity)
		{
			IPlayerInfo* listenerInfo = GetClientPlayerInfo(client);
			if (listenerInfo == nullptr)
			{
				scan.originFail++;
				recipients.push_back(recipient);
				continue;
			}

			if (IsDeadOrObserverForProximity(listenerInfo, scan))
			{
				recipients.push_back(recipient);
				continue;
			}

			Vector listenerOrigin = listenerInfo->GetAbsOrigin();
			if (!hasSpeakerOrigin || !IsFiniteVector(listenerOrigin))
			{
				scan.originFail++;
				recipients.push_back(recipient);
				continue;
			}

			float distanceSqr = GetDistanceSqr(speakerOrigin, listenerOrigin);
			scan.liveDistanceChecked++;
			if (distanceSqr > maxDistanceSqr)
			{
				scan.tooFar++;
				continue;
			}

			recipient.distance = std::sqrt(distanceSqr);
			UpdateDistanceStats(scan, recipient.distance);
			recipient.gainDb = QuantizeProximityGainDb(CalculateProximityGainDb(recipient.distance));
			recipient.bucket = GetProximityBucket(recipient.gainDb);
		}

		recipients.push_back(recipient);
	}

	scan.recipients = static_cast<int>(recipients.size());
	return recipients;
}

DETOUR_DECL_STATIC4(VC_SV_BroadcastVoiceData, void, IClient*, pClient, int, nBytes, uint8_t*, data, int64, xuid)
{
	int fromClientSlot = pClient->GetPlayerSlot();
	int fromClientIndex = fromClientSlot + 1;

	if (!ShouldInterceptVoice(fromClientIndex))
	{
		DETOUR_STATIC_CALL(VC_SV_BroadcastVoiceData)(pClient, nBytes, data, xuid);
		return;
	}

	std::vector<uint8_t> processedData;
	VoiceControlDebugInfo debugInfo;
	VoiceControlDebugInfo* debugInfoPtr = (vc_debug.GetBool() || vc_debug_recipients.GetBool()) ? &debugInfo : nullptr;
	VoiceRecipientScan proximityBypassScan;
	bool hasProximityBypassScan = false;
	bool proximityEnabled = IsProximityEnabled();
	if (proximityEnabled)
	{
		Vector speakerOrigin;
		if (!GetLiveClientOrigin(fromClientIndex, speakerOrigin, proximityBypassScan))
		{
			proximityEnabled = false;
			hasProximityBypassScan = true;
		}
	}

	if (proximityEnabled)
	{
		VoiceRecipientScan scan;
		scan.msgProximity = false;
		scan.falloffEnabled = IsProximityFalloffEnabled();
		std::vector<VoiceRecipient> recipients = GetVoiceRecipients(fromClientIndex, true, true, scan);
		const char* sendPath = vc_send_via_netchannel.GetBool() ? "netchan" : "client";

		if (recipients.empty())
		{
			if (debugInfoPtr)
			{
				debugInfo.inputBytes = nBytes;
				debugInfo.outputBytes = nBytes;
				LogVoiceDebug(fromClientIndex, fromClientSlot, debugInfo, scan, sendPath);
			}

			return;
		}

		if (scan.falloffEnabled)
		{
			std::map<int, std::vector<const VoiceRecipient*>> buckets;
			for (const VoiceRecipient& recipient : recipients)
			{
				buckets[recipient.bucket].push_back(&recipient);
			}

			scan.bucketCount = static_cast<int>(buckets.size());
			bool capturedDebug = false;
			for (const auto& bucket : buckets)
			{
				int bucketIndex = bucket.first;
				float bucketGainDb = bucket.second.empty() ? 0.0f : bucket.second.front()->gainDb;
				std::vector<uint8_t> bucketData;
				VoiceControlDebugInfo bucketDebug;
				VoiceControlDebugInfo* bucketDebugPtr = debugInfoPtr ? &bucketDebug : nullptr;

				if (bucketIndex == 0 && !IsAudioProcessingRequired(fromClientIndex))
				{
					bucketData.assign(data, data + nBytes);
					if (bucketDebugPtr)
					{
						bucketDebug.inputBytes = nBytes;
						bucketDebug.outputBytes = nBytes;
					}
				}
				else
				{
					VoiceControlSettings settings = BuildSettingsForClient(fromClientIndex);
					settings.duckGainDb += bucketGainDb;
					if (!g_proximityProcessors[fromClientIndex][bucketIndex].Process(data, nBytes, settings, bucketData, bucketDebugPtr))
					{
						bucketData.assign(data, data + nBytes);
						if (bucketDebugPtr)
						{
							bucketDebug = VoiceControlDebugInfo();
							bucketDebug.inputBytes = nBytes;
							bucketDebug.outputBytes = nBytes;
							bucketDebug.duckGainDb = settings.duckGainDb;
							bucketDebug.finalGainDb = settings.duckGainDb;
						}
					}
				}

				if (debugInfoPtr && (!capturedDebug || bucketIndex == 0))
				{
					debugInfo = bucketDebug;
					capturedDebug = true;
				}

				for (const VoiceRecipient* recipient : bucket.second)
				{
					SendVoiceControlDataMsg(fromClientSlot, recipient->client, bucketData.data(), static_cast<int>(bucketData.size()), xuid, false, scan);
				}
			}

			if (debugInfoPtr)
			{
				LogVoiceDebug(fromClientIndex, fromClientSlot, debugInfo, scan, sendPath);
			}

			return;
		}

		if (IsAudioProcessingRequired(fromClientIndex))
		{
			VoiceControlSettings settings = BuildSettingsForClient(fromClientIndex);
			if (!g_processors[fromClientIndex].Process(data, nBytes, settings, processedData, debugInfoPtr))
			{
				processedData.assign(data, data + nBytes);
				if (debugInfoPtr)
				{
					debugInfo = VoiceControlDebugInfo();
					debugInfo.inputBytes = nBytes;
					debugInfo.outputBytes = nBytes;
				}
			}
		}
		else
		{
			processedData.assign(data, data + nBytes);
			if (debugInfoPtr)
			{
				debugInfo.inputBytes = nBytes;
				debugInfo.outputBytes = nBytes;
			}
		}

		scan.bucketCount = recipients.empty() ? 0 : 1;
		for (const VoiceRecipient& recipient : recipients)
		{
			SendVoiceControlDataMsg(fromClientSlot, recipient.client, processedData.data(), static_cast<int>(processedData.size()), xuid, false, scan);
		}

		if (debugInfoPtr)
		{
			LogVoiceDebug(fromClientIndex, fromClientSlot, debugInfo, scan, sendPath);
		}

		return;
	}

	if (IsAudioProcessingRequired(fromClientIndex))
	{
		VoiceControlSettings settings = BuildSettingsForClient(fromClientIndex);
		if (!g_processors[fromClientIndex].Process(data, nBytes, settings, processedData, debugInfoPtr))
		{
			DETOUR_STATIC_CALL(VC_SV_BroadcastVoiceData)(pClient, nBytes, data, xuid);
			return;
		}
	}
	else
	{
		processedData.assign(data, data + nBytes);
		if (debugInfoPtr)
		{
			debugInfo.inputBytes = nBytes;
			debugInfo.outputBytes = nBytes;
		}
	}

	if (vc_debug_recipients.GetBool())
	{
		VoiceRecipientScan scan = hasProximityBypassScan ? proximityBypassScan : VoiceRecipientScan();
		GetVoiceRecipients(fromClientIndex, false, true, scan);
		if (debugInfoPtr)
		{
			LogVoiceDebug(fromClientIndex, fromClientSlot, debugInfo, scan, "engine");
		}
	}
	else if (debugInfoPtr)
	{
		VoiceRecipientScan scan = hasProximityBypassScan ? proximityBypassScan : VoiceRecipientScan();
		LogVoiceDebug(fromClientIndex, fromClientSlot, debugInfo, scan, "engine");
	}

	DETOUR_STATIC_CALL(VC_SV_BroadcastVoiceData)(pClient, static_cast<int>(processedData.size()), processedData.data(), xuid);
}

static cell_t VC_SetPlayerGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	float gainDb = sp_ctof(params[2]);
	gainDb = std::max(-24.0f, std::min(24.0f, gainDb));

	g_playerSettings[target].gainDb = gainDb;
	g_playerSettings[target].enabled = HasMeaningfulGain(gainDb);
	return true;
}

static cell_t VC_ClearPlayerGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	g_playerSettings[target].gainDb = 0.0f;
	g_playerSettings[target].enabled = false;
	return true;
}

static cell_t VC_GetPlayerGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return sp_ftoc(0.0f);
	}

	return sp_ftoc(g_playerSettings[target].enabled ? g_playerSettings[target].gainDb : 0.0f);
}

static cell_t VC_SetPlayerDuckGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	float gainDb = sp_ctof(params[2]);
	gainDb = std::max(-24.0f, std::min(0.0f, gainDb));

	g_playerSettings[target].duckGainDb = gainDb;
	g_playerSettings[target].duckEnabled = HasMeaningfulGain(gainDb);
	return true;
}

static cell_t VC_ClearPlayerDuckGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	g_playerSettings[target].duckGainDb = 0.0f;
	g_playerSettings[target].duckEnabled = false;
	return true;
}

static cell_t VC_GetPlayerDuckGain(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return sp_ftoc(0.0f);
	}

	return sp_ftoc(g_playerSettings[target].duckEnabled ? g_playerSettings[target].duckGainDb : 0.0f);
}

static cell_t VC_SetPlayerPreset(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	int preset = std::max(0, std::min(3, params[2]));
	g_playerSettings[target].preset = preset;
	ResetClientProcessors(target);
	return true;
}

static cell_t VC_ClearPlayerPreset(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return false;
	}

	g_playerSettings[target].preset = 0;
	ResetClientProcessors(target);
	return true;
}

static cell_t VC_GetPlayerPreset(IPluginContext* pContext, const cell_t* params)
{
	int target = params[1];
	if (!IsValidClientIndex(target))
	{
		return 0;
	}

	return g_playerSettings[target].preset;
}

static cell_t VC_SetAutoLevelEnabled(IPluginContext* pContext, const cell_t* params)
{
	vc_agc_enabled.SetValue(params[1] ? 1 : 0);
	return true;
}

static cell_t VC_GetAutoLevelEnabled(IPluginContext* pContext, const cell_t* params)
{
	return vc_agc_enabled.GetBool();
}

static cell_t VC_ReloadSettings(IPluginContext* pContext, const cell_t* params)
{
	for (int client = 1; client <= MAXPLAYERS; client++)
	{
		ResetClientProcessors(client);
	}

	return true;
}

const sp_nativeinfo_t g_VoiceControlNatives[] = {
	{ "VC_SetPlayerGain", VC_SetPlayerGain },
	{ "VC_ClearPlayerGain", VC_ClearPlayerGain },
	{ "VC_GetPlayerGain", VC_GetPlayerGain },
	{ "VC_SetPlayerDuckGain", VC_SetPlayerDuckGain },
	{ "VC_ClearPlayerDuckGain", VC_ClearPlayerDuckGain },
	{ "VC_GetPlayerDuckGain", VC_GetPlayerDuckGain },
	{ "VC_SetPlayerPreset", VC_SetPlayerPreset },
	{ "VC_ClearPlayerPreset", VC_ClearPlayerPreset },
	{ "VC_GetPlayerPreset", VC_GetPlayerPreset },
	{ "VC_SetAutoLevelEnabled", VC_SetAutoLevelEnabled },
	{ "VC_GetAutoLevelEnabled", VC_GetAutoLevelEnabled },
	{ "VC_ReloadSettings", VC_ReloadSettings },
	{ nullptr, nullptr },
};

void VoiceControlExt::SDK_OnAllLoaded()
{
	SM_GET_LATE_IFACE(SDKTOOLS, g_pVoiceControlSDKTools);

	g_pVoiceControlServer = g_pVoiceControlSDKTools->GetIServer();
}

bool VoiceControlExt::SDK_OnLoad(char* error, size_t maxlength, bool late)
{
	sharesys->AddDependency(myself, "sdktools.ext", true, true);
	sharesys->AddNatives(myself, g_VoiceControlNatives);

	char conf_error[255];
	if (!gameconfs->LoadGameConfigFile("voicecontrol", &g_pVoiceControlGameConf, conf_error, sizeof(conf_error)))
	{
		if (conf_error[0])
		{
			snprintf(error, maxlength, "Could not read config file voicecontrol.txt: %s", conf_error);
		}

		return false;
	}

	CDetourManager::Init(smutils->GetScriptingEngine(), g_pVoiceControlGameConf);

	bool detoursInited = false;
	CREATE_DETOUR_STATIC(VC_SV_BroadcastVoiceData, "SV_BroadcastVoiceData", detoursInited);

	return true;
}

bool VoiceControlExt::RegisterConCommandBase(ConCommandBase* pCommand)
{
	META_REGCVAR(pCommand);
	return true;
}

bool VoiceControlExt::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	g_pVoiceControlStringTables = static_cast<INetworkStringTableContainer*>(ismm->GetEngineFactory()(INTERFACENAME_NETWORKSTRINGTABLESERVER, nullptr));
	ConVar_Register(0, this);
	return true;
}
