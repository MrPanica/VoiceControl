# VoiceControl

VoiceControl is a SourceMod FULL AI extension and plugin package for TF2 voice processing.

https://www.youtube.com/watch?v=ckvYGmQS1B4

[![YouTube Video Preview](https://img.youtube.com/vi/ckvYGmQS1B4/maxresdefault.jpg)](https://www.youtube.com/watch?v=ckvYGmQS1B4)
```text
.
  AMBuilder / AMBuildScript / configure.py
  Dockerfile / docker-compose.yml
  voicecontrol_extension.cpp
  voicecontrol_processor.cpp
  voicecontrol_extension.h
  voicecontrol_processor.h
  asm/
  CDetour/
  include/
  libudis86/
  sm-public/       Bundled x64 CDetour/asm/libudis86 build support.
  scripts/

SourceMod/
  addons/sourcemod/gamedata/voicecontrol.txt
  addons/sourcemod/scripting/voicecontrol.sp
  addons/sourcemod/scripting/include/voicecontrol.inc

builds/
  tf2-linux-x86/   Ready 32-bit TF2 extension layout.
  tf2-linux-x64/   Ready 64-bit TF2 extension layout.
  runtime/         Shared SourceMod files: gamedata, plugin, scripting include, autoload.
```

## Install

Copy the matching build folder and runtime folder into the game server root.

For TF2 32-bit:

```text
builds/tf2-linux-x86/addons/* -> server/addons/*
builds/runtime/addons/*       -> server/addons/*
```

For TF2 64-bit:

```text
builds/tf2-linux-x64/addons/* -> server/addons/*
builds/runtime/addons/*       -> server/addons/*
```

Then check on the server:

```cfg
sm exts load voicecontrol
sm exts list
```

## Configuration

Start with this baseline in `server.cfg` or a separate cfg loaded after SourceMod:

```cfg
// VoiceControl master switch
vc_enabled 1

// Automatic loudness leveling
vc_agc_enabled 1
vc_agc_target_rms 0.12
vc_agc_noise_floor_rms 0.0015
vc_agc_max_boost_db 18
vc_agc_max_cut_db -12
vc_limiter_ceiling 0.95

// Lightweight cleanup
vc_dsp_enabled 1
vc_highpass_enabled 1
vc_highpass_cutoff_hz 100
vc_noise_gate_enabled 1
vc_noise_gate_threshold_rms 0.002
vc_noise_gate_hysteresis_rms 0.001
vc_noise_gate_atten_db -8
vc_noise_gate_attack_ms 5
vc_noise_gate_release_ms 120
vc_softclip_enabled 1
vc_softclip_threshold 0.85

// Sending and diagnostics
vc_respect_hearing 1
vc_include_sourcetv 0
vc_send_via_netchannel 0
vc_debug 0
vc_debug_recipients 0

// Optional proximity voice
vc_proximity_enabled 0
vc_proximity_max_distance 1200
vc_proximity_falloff_enabled 1
vc_proximity_full_volume_distance 300
vc_proximity_min_gain_db -24
```

## Cvar Reference

### Core

| Cvar | Default | Description |
| --- | ---: | --- |
| `vc_enabled` | `1` | Master switch. `1` enables all VoiceControl processing, `0` disables custom gain, AGC, DSP, and proximity handling. |

### Auto Volume / AGC

| Cvar | Default | Description |
| --- | ---: | --- |
| `vc_agc_enabled` | `1` | Enables automatic gain control. It brings quiet and loud microphones closer to the target loudness. |
| `vc_agc_target_rms` | `0.12` | Target normalized RMS loudness. Raise for louder normalized voice, lower if voices sound too aggressive. |
| `vc_agc_noise_floor_rms` | `0.0015` | Input RMS below which AGC will not boost. Raise this if background noise gets amplified. |
| `vc_agc_max_boost_db` | `18` | Maximum boost for quiet microphones, in dB. Higher values help weak mics but increase noise risk. |
| `vc_agc_max_cut_db` | `-12` | Maximum reduction for loud microphones, in dB. This value should stay negative. |
| `vc_limiter_ceiling` | `0.95` | Final hard limiter ceiling for normalized samples. Prevents full-scale clipping after gain changes. |

Tuning rules:

| Problem | Change |
| --- | --- |
| Quiet players are still too quiet | Increase `vc_agc_target_rms` slightly or raise `vc_agc_max_boost_db`. |
| Background noise becomes loud | Increase `vc_agc_noise_floor_rms` or lower `vc_agc_max_boost_db`. |
| Loud players still clip | Lower `vc_limiter_ceiling` slightly or keep `vc_softclip_enabled 1`. |
| Everyone sounds too compressed | Lower `vc_agc_target_rms` or reduce `vc_agc_max_boost_db`. |

### DSP Cleanup

| Cvar | Default | Description |
| --- | ---: | --- |
| `vc_dsp_enabled` | `1` | Master switch for lightweight cleanup DSP. Controls high-pass and noise gate processing. |
| `vc_highpass_enabled` | `1` | Removes low-frequency rumble, mic bumps, and hum. |
| `vc_highpass_cutoff_hz` | `100` | High-pass cutoff in Hz. `100` is a safe voice-focused default. |
| `vc_noise_gate_enabled` | `1` | Enables soft noise gate/expander for quiet background noise. |
| `vc_noise_gate_threshold_rms` | `0.002` | RMS level below which the gate starts closing. Higher values are more aggressive. |
| `vc_noise_gate_hysteresis_rms` | `0.001` | Extra margin to avoid rapid gate open/close flicker. |
| `vc_noise_gate_atten_db` | `-8` | Gain applied when the gate is closed. Negative value attenuates noise without hard muting. |
| `vc_noise_gate_attack_ms` | `5` | Gate opening time in milliseconds. Lower values react faster to speech. |
| `vc_noise_gate_release_ms` | `120` | Gate closing time in milliseconds. Higher values avoid chopping word endings. |
| `vc_softclip_enabled` | `1` | Enables soft clipping before the limiter, making overloads less harsh. |
| `vc_softclip_threshold` | `0.85` | Level where soft clipping starts before the limiter ceiling. |

### Sending, Hearing Rules, And Debug

| Cvar | Default | Description |
| --- | ---: | --- |
| `vc_respect_hearing` | `1` | Uses engine `IsHearingClient` filtering. Keep `1` to respect normal voice rules and masks. |
| `vc_include_sourcetv` | `0` | Allows SourceTV/Replay to receive custom processed voice. Usually keep disabled. |
| `vc_send_via_netchannel` | `0` | Send transport. `0` uses `IClient::SendNetMsg`; `1` uses `INetChannel::SendNetMsg(..., bVoice=true)` with fallback. |
| `vc_debug` | `0` | Logs per-packet audio stats such as RMS, AGC gain, clips, and gate state. |
| `vc_debug_recipients` | `0` | Logs recipient filtering and send diagnostics. Use this for proximity debugging. |

Debug AGC:

```cfg
vc_debug 1
```

Expected fields: `in_rms`, `out_rms`, `agc`, `clips`, `gate_open`, `gate_db`.

Debug recipients:

```cfg
vc_debug_recipients 1
```

Expected fields: `recipients`, `not_hearing`, `too_far`, `send_ok`, `send_fail`, `send`, `msg_proximity`.

### Proximity Voice

| Cvar | Default | Description |
| --- | ---: | --- |
| `vc_proximity_enabled` | `0` | Enables server-side proximity filtering. The engine proximity flag stays disabled. |
| `vc_proximity_max_distance` | `1200` | Maximum distance in Hammer units for live player-to-player voice. Farther live listeners do not receive the packet. |
| `vc_proximity_falloff_enabled` | `1` | Enables distance-based volume falloff. If `0`, proximity is cutoff-only. |
| `vc_proximity_full_volume_distance` | `300` | Distance where voice stays full volume before falloff starts. |
| `vc_proximity_min_gain_db` | `-24` | Gain at max distance. After max distance the packet is not sent. |

Proximity is implemented by VoiceControl itself:

| Mode | Behavior |
| --- | --- |
| `vc_proximity_enabled 0` | No distance filtering. Voice is processed and sent normally. |
| `vc_proximity_enabled 1` | Extension checks distance and recipients server-side. |
| Engine proximity flag | Always kept off: `m_bProximity = false`. |

Dead players, observers, SourceTV, and Replay are not treated as normal spatial listeners. They bypass live distance cutoff according to the extension rules and cvars.

### SourceMod Plugin

| Cvar / Command | Default | Description |
| --- | ---: | --- |
| `vc_database` | `default` | Database config name from `databases.cfg`. Used by `voicecontrol.smx` to store manual per-player gain. |
| `sm_vc` | n/a | Admin menu for manual player gain and presets. Requires generic admin flag. |
| `vc_dump_stringtables` | n/a | Diagnostic server command for network string table usage. |

## Proximity Tests

```cfg
vc_proximity_enabled 1
vc_proximity_max_distance 100000
vc_debug_recipients 1
vc_send_via_netchannel 1
```

Expected debug line:

```text
[VoiceControl] recipients ... recipients=N ... not_hearing=... msg_proximity=0 ... send_ok=N send=netchan
```

Cutoff test:

```cfg
vc_proximity_max_distance 300
vc_proximity_falloff_enabled 0
```

Falloff test:

```cfg
vc_proximity_falloff_enabled 1
vc_proximity_full_volume_distance 300
vc_proximity_max_distance 1200
vc_proximity_min_gain_db -24
```

## Build

Run from this folder:

```powershell
docker compose run --rm extension-build
docker compose run --rm extension-build-x64
```

Both build scripts use `--voicecontrol-only`.
The x64 build uses the bundled `sm-public/` files, so this folder does not depend on another local project checkout.

Outputs:

```text
build/package/addons/sourcemod/extensions/voicecontrol.ext.2.tf2.so
build-x64/package/addons/sourcemod/extensions/x64/voicecontrol.ext.2.tf2.so
```

Verify format:

```powershell
docker compose run --rm --entrypoint /bin/bash extension-build -lc "file /extension/build/package/addons/sourcemod/extensions/voicecontrol.ext.2.tf2.so"
docker compose run --rm --entrypoint /bin/bash extension-build-x64 -lc "file /extension/build-x64/package/addons/sourcemod/extensions/x64/voicecontrol.ext.2.tf2.so"
```

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


- TF2 builds define `REPLAY_ENABLED`, so `IClient::IsHearingClient()` uses the correct TF2 vtable layout.
- `vc_proximity_enabled` no longer relies on the engine proximity voice flag.
- Debug logging is split into short `audio` and `recipients` lines to avoid console truncation.
- NetChannel sending has safe fallback to `IClient::SendNetMsg()`.
