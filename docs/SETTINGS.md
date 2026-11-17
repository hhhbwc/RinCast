# Settings key contract (settings.json)

Generated from the code - `pc/streamer/src/settings.h` (field + meaning), `pc/streamer/src/settings.cpp` (serialize/deserialize/clamp line numbers) and the hot-rebuild whitelist in `pc/streamer/src/app.cpp` (`ApplySettingSideEffects`). Every key below exists in both ToJson and FromJson; the counts are asserted by the generator.

Keys: **38** (settings.cpp ToJson at line 16, FromJson at line 67).

| key | type | default | range / clamp | hot path | meaning |
|---|---|---|---|---|---|
| `adaptiveBitrate` | bool | true | — | **yes** | (no comment) (settings.h:43) |
| `adaptiveQuantization` | bool | true | — | **yes** | (no comment) (settings.h:78) |
| `allowRemote` | bool | true | — | **yes** | (no comment) (settings.h:96) |
| `audioDevices` | string | "auto" | — |  | (no comment) (settings.h:88) |
| `autoRegisterSteamVrDriver` | bool | true | — |  | (no comment) (settings.h:92) |
| `cloudEmail` | string | — | — |  | (no comment) (settings.h:99) |
| `cloudRefreshTokenProtected` | string | — | — |  | (no comment) (settings.h:97) |
| `cloudUrl` | string | "https://rincast.com" | — | **yes** | Cloud (settings.h:95) |
| `cloudUserName` | string | — | — |  | (no comment) (settings.h:98) |
| `codec` | string | "auto" | — | **yes** | Video (settings.h:35) |
| `desktopBitrateMbps` | int | 40 | [5, 200] (settings.cpp:119) | **yes** | (no comment) (settings.h:42) |
| `desktopFps` | int | 0 | — | **yes** | (no comment) (settings.h:81) |
| `desktopInput` | bool | true | — |  | (no comment) (settings.h:31) |
| `desktopMaxHeight` | int | 0 | — | **yes** | (no comment) (settings.h:80) |
| `encodeScalePct` | int | 0 | 0, else [50, 100] (settings.cpp:123) | **yes** | VR encode resolution as a percentage of what the game renders (ROUND18 L2). 0 = follow the selected profile's value (all 100 today); >0 = user override, clamped (settings.h:63) |
| `fecPercent` | int | 5 | [0, 25] (settings.cpp:120) | **yes** | (no comment) (settings.h:76) |
| `foveatedEncoding` | bool | false | — | **yes** | (no comment) (settings.h:77) |
| `l7Autoscale` | bool | false | — | **yes** | ROUND19 T3: the decode-side autoscale ladder (rc/autoscale.h). When on, the ladder may step the ENCODE scale down 100 -> 85 -> 67 -> 50 (multiplied onto the bas (settings.h:69) |
| `micPassthrough` | bool | true | — |  | (no comment) (settings.h:85) |
| `openxrRuntime` | string | "steamvr" | — |  | VR (settings.h:91) |
| `pcBitrateWins` | bool | false | — | **yes** | The headset sends its own bitrate choice and it wins by default: it plans its decode buffers around that number, and a PC-side ceiling it didn't expect can star (settings.h:41) |
| `pcName` | string | — | — | **yes** | (no comment) (settings.h:24) |
| `playOnPcToo` | bool | false | — |  | (no comment) (settings.h:87) |
| `preventSleep` | bool | true | — |  | (no comment) (settings.h:29) |
| `profileControlsVideo` | bool | false | — | **yes** | When the user lets the Best-picture profile drive video settings, it may raise video-side choices on its own (right now: the fovea's full-density share, see Qua (settings.h:75) |
| `qualityProfileCbr` | bool | false | — | **yes** | CBR for the Best-picture profile: "the number I set is what every frame costs". Measured on real headset frames at +7.5% on stills vs capped VBR (encode-bench,  (settings.h:56) |
| `requireApproval` | bool | true | — |  | (no comment) (settings.h:30) |
| `serverId` | string | — | — |  | Identity (settings.h:23) |
| `startMinimized` | bool | true | — |  | (no comment) (settings.h:28) |
| `startWithWindows` | bool | true | — | **yes** | General (settings.h:27) |
| `streamAudio` | bool | true | — |  | Audio (settings.h:84) |
| `streamProfile` | int | 1 | [0, 2] (settings.cpp:121) | **yes** | What the automatic-bitrate switch now selects: 0 = latency, 1 = balanced, 2 = picture quality. Stored as an int so the settings file stays readable. See rc/stre (settings.h:47) |
| `switchDefaultAudio` | bool | true | — |  | (no comment) (settings.h:86) |
| `tenBit` | bool | false | — | **yes** | (no comment) (settings.h:36) |
| `trustedDevices` | vector<TrustedDevice> | — | — |  | (no comment) (settings.h:32) |
| `twoPassEncoding` | bool | false | — | **yes** | (no comment) (settings.h:79) |
| `videoPacingMode` | int | 0 | — | **yes** | How a LAN stream paces its datagrams (PLAN-pacing-fix): 0 = auto (burst on the LAN, the legacy follow on a remote path), 1 = off, 2 = follow the guard cap (the  (settings.h:51) |
| `vrBitrateMbps` | int | 150 | [10, 500] (settings.cpp:118) | **yes** | (no comment) (settings.h:37) |

Hot path = changing the key rebuilds the live VR stream via `ClientSession::OnStreamProfileChanged` (worker-queued, VR stream only). Non-hot keys apply without a rebuild.

Not persisted here: `trustedDevices` entries (managed by the approval flow, StreamServer) and the DPAPI-protected cloud token (`cloudRefreshTokenProtected`, settings.h).
