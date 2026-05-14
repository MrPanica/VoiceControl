#if defined _voicecontrol_included
	#endinput
#endif
#define _voicecontrol_included

enum VoiceControlPreset
{
	VC_Preset_Normal = 0,
	VC_Preset_QuietMic,
	VC_Preset_NoisyMic,
	VC_Preset_LoudMic
};

native bool VC_SetPlayerGain(int target, float gainDb);
native bool VC_ClearPlayerGain(int target);
native float VC_GetPlayerGain(int target);
native bool VC_SetPlayerDuckGain(int target, float gainDb);
native bool VC_ClearPlayerDuckGain(int target);
native float VC_GetPlayerDuckGain(int target);
native bool VC_SetPlayerPreset(int target, VoiceControlPreset preset);
native bool VC_ClearPlayerPreset(int target);
native VoiceControlPreset VC_GetPlayerPreset(int target);
native bool VC_SetAutoLevelEnabled(bool enabled);
native bool VC_GetAutoLevelEnabled();
native bool VC_ReloadSettings();
