# VoiceControl

VoiceControl is a SourceMod extension and plugin package for TF2 voice processing.

This folder is standalone and contains only VoiceControl files.

## Layout

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

## Main Cvars

```cfg
vc_enabled 1

vc_agc_enabled 1
vc_agc_target_rms 0.12
vc_agc_noise_floor_rms 0.0015
vc_agc_max_boost_db 18
vc_agc_max_cut_db -12
vc_limiter_ceiling 0.95

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

vc_debug 0
vc_debug_recipients 0
vc_respect_hearing 1
vc_include_sourcetv 0
vc_send_via_netchannel 0

vc_proximity_enabled 0
vc_proximity_max_distance 1200
vc_proximity_falloff_enabled 1
vc_proximity_full_volume_distance 300
vc_proximity_min_gain_db -24
```

## Auto Volume / AGC

Recommended baseline:

```cfg
vc_enabled 1
vc_agc_enabled 1
vc_agc_target_rms 0.12
vc_agc_noise_floor_rms 0.0015
vc_agc_max_boost_db 18
vc_agc_max_cut_db -12
vc_limiter_ceiling 0.95

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
```

`vc_agc_target_rms` is the target voice loudness. Raise it slightly for louder voice, lower it if voice sounds too aggressive.

`vc_agc_max_boost_db` limits how much quiet microphones can be boosted. If background noise is amplified too much, lower this value or raise `vc_agc_noise_floor_rms`.

`vc_agc_max_cut_db` limits how much loud microphones can be reduced. It should stay negative.

`vc_limiter_ceiling` and `vc_softclip_enabled` protect against clipping after gain changes.

`vc_dsp_enabled` enables lightweight cleanup. High-pass removes low rumble; noise gate attenuates quiet background noise when the player is not speaking.

Debug AGC on a live server:

```cfg
vc_debug 1
```

Expected debug fields include `in_rms`, `out_rms`, `agc`, `clips`, `gate_open`, and `gate_db`.

`vc_send_via_netchannel 1` sends processed voice through `INetChannel::SendNetMsg(..., bVoice=true)` and falls back to `IClient::SendNetMsg()` if netchannel sending fails.

`vc_proximity_enabled 1` uses server-side distance filtering. The packet `m_bProximity` flag stays false; the extension controls recipients and falloff itself.

## Proximity Test

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

## Current Fix Notes

- TF2 builds define `REPLAY_ENABLED`, so `IClient::IsHearingClient()` uses the correct TF2 vtable layout.
- `vc_proximity_enabled` no longer relies on the engine proximity voice flag.
- Debug logging is split into short `audio` and `recipients` lines to avoid console truncation.
- NetChannel sending has safe fallback to `IClient::SendNetMsg()`.
