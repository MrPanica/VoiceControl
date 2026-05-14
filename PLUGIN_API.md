# Plugin API / Include

VoiceControl can be used from other SourceMod plugins through its include file:

```sourcepawn
#include <voicecontrol>
```

Install path:

```text
addons/sourcemod/scripting/include/voicecontrol.inc
```

The include must be available to `spcomp` when compiling your plugin. The VoiceControl extension must also be loaded on the server:

```cfg
sm exts load voicecontrol
```

If the extension is not loaded, plugins that call VoiceControl natives will not work correctly.

## Presets

```sourcepawn
enum VoiceControlPreset
{
    VC_Preset_Normal = 0,
    VC_Preset_QuietMic,
    VC_Preset_NoisyMic,
    VC_Preset_LoudMic
};
```

| Preset | Description |
| --- | --- |
| `VC_Preset_Normal` | Normal processing without a special preset. |
| `VC_Preset_QuietMic` | For players with quiet microphones. Helps make the voice more audible. |
| `VC_Preset_NoisyMic` | For players with noisy microphones. Uses processing that is safer around background noise. |
| `VC_Preset_LoudMic` | For players with loud or clipping microphones. Reduces overload risk. |

## Main Natives

| Native | Description |
| --- | --- |
| `VC_SetPlayerGain(client, gainDb)` | Sets manual voice gain for a player in dB. Positive values make voice louder, negative values make it quieter. |
| `VC_ClearPlayerGain(client)` | Clears the player's manual gain. |
| `VC_GetPlayerGain(client)` | Returns the player's current manual gain in dB. |
| `VC_SetPlayerPreset(client, preset)` | Applies a VoiceControl preset to a player. |
| `VC_ClearPlayerPreset(client)` | Clears the player's preset. |
| `VC_GetPlayerPreset(client)` | Returns the player's current preset. |
| `VC_SetAutoLevelEnabled(enabled)` | Globally enables or disables auto-level / AGC. |
| `VC_GetAutoLevelEnabled()` | Returns whether auto-level / AGC is enabled. |
| `VC_ReloadSettings()` | Reloads extension-side runtime settings. |

## Duck Gain

Duck gain is a separate additional gain layer. It is useful for temporary volume changes, for example lowering a player's voice during an event without changing their main gain.

| Native | Description |
| --- | --- |
| `VC_SetPlayerDuckGain(client, gainDb)` | Sets additional temporary gain in dB. |
| `VC_ClearPlayerDuckGain(client)` | Clears duck gain. |
| `VC_GetPlayerDuckGain(client)` | Returns the player's current duck gain in dB. |

Example:

```sourcepawn
VC_SetPlayerGain(client, 6.0);       // main gain: +6 dB
VC_SetPlayerDuckGain(client, -12.0); // temporary duck: -12 dB
```

The final processed voice uses both layers.

## Example Plugin

```sourcepawn
#include <sourcemod>
#include <voicecontrol>

public void OnPluginStart()
{
    RegAdminCmd("sm_voice_quiet", Command_QuietMic, ADMFLAG_GENERIC);
    RegAdminCmd("sm_voice_loud", Command_LoudMic, ADMFLAG_GENERIC);
    RegAdminCmd("sm_voice_reset", Command_ResetVoice, ADMFLAG_GENERIC);
}

public Action Command_QuietMic(int client, int args)
{
    if (!IsValidClient(client))
    {
        return Plugin_Handled;
    }

    VC_SetPlayerPreset(client, VC_Preset_QuietMic);
    VC_SetPlayerGain(client, 6.0);

    PrintToChat(client, "[VoiceControl] Quiet microphone preset enabled.");
    return Plugin_Handled;
}

public Action Command_LoudMic(int client, int args)
{
    if (!IsValidClient(client))
    {
        return Plugin_Handled;
    }

    VC_SetPlayerPreset(client, VC_Preset_LoudMic);
    VC_SetPlayerGain(client, -6.0);

    PrintToChat(client, "[VoiceControl] Loud microphone preset enabled.");
    return Plugin_Handled;
}

public Action Command_ResetVoice(int client, int args)
{
    if (!IsValidClient(client))
    {
        return Plugin_Handled;
    }

    VC_ClearPlayerPreset(client);
    VC_ClearPlayerGain(client);
    VC_ClearPlayerDuckGain(client);

    PrintToChat(client, "[VoiceControl] Voice settings reset.");
    return Plugin_Handled;
}

bool IsValidClient(int client)
{
    return client > 0 && client <= MaxClients && IsClientInGame(client);
}
```

## Notes

- `client` must be a valid player index from `1` to `MaxClients`.
- `gainDb` values are in decibels.
- `0.0` dB means no volume change.
- Positive gain makes voice louder.
- Negative gain makes voice quieter.
- To fully reset a player, call `VC_ClearPlayerPreset`, `VC_ClearPlayerGain`, and `VC_ClearPlayerDuckGain`.
