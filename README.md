# RinCast

[English](README.md) | [简体中文](README.zh-CN.md)

> **Project Relocation & Overhaul Notice**  
> This repository was initially developed as an optimized fork of [TheReal-Flo/RinCast](https://github.com/TheReal-Flo/RinCast). Through deep overhauls across the video streaming pipeline, SteamVR driver stack, cross-brand OpenXR runtime, and more, cumulative changes have exceeded 150+ files and 35,000+ lines of code. Changes of this magnitude far exceed the scope of upstream pull requests. This project is now maintained as an independent open-source project.  
> 
> **Key Enhancements**:
> - **Visual Quality**: Optimized dynamic bitrate and encoder scheduling, alongside refined sharpening filters for richer detail and superior clarity.
> - **Latency Reduction**: Streamlined packet dispatching and pacing to substantially reduce end-to-end streaming latency and minimize frame stuttering.
> - **Bug Fixes & Stability**: Fixed SteamVR multi-layer tearing and ghosting, multi-brand controller button mapping and tracking discrepancies, and localization display issues.
> - **Legacy VR Compatibility (Experimental)**: Attempted compatibility adaptations for legacy headsets (experimental, not fully field-tested).

---

RinCast is a free, open-source way to use your Windows PC from a Meta Quest (3 / 3S / Pro / 2) or a PICO (4 / 4 Pro / 4 Ultra).
See and control all of your monitors in an immersive space, and play PC VR games
through SteamVR. There's no subscription and no store purchase.

- **Desktop:** every monitor as its own floating, curved or flat screen, controlled with the controllers and a virtual keyboard.
- **PC VR:** SteamVR sees a headset and Touch controllers, and you can launch your Steam VR games from the headset.
- **Video:** hardware-encoded H.264, HEVC or AV1 with NVENC, AMF or Media Foundation.
- **Private:** every packet is encrypted, and stream traffic always goes directly between your headset and your PC.
- **No account needed at home:** PCs on the same network appear automatically. An optional account lets you connect from other networks.

## Requirements

- Windows 10 or 11, 64-bit, with an NVIDIA, AMD or Intel GPU that can encode video.
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (already included with Windows 11 and most Windows 10 PCs).
- **Supported Headsets**:
  - **Meta Quest**: Quest 3, Quest 3S, Quest Pro, Quest 2. Requires [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) enabled. The Quest 1 needs its own build, `RinCast-<version>-quest1.apk` (untested).
  - **PICO**: PICO 4, PICO 4 Pro, PICO 4 Ultra. Turn on USB debugging under **Settings > General > About** (tap **Software Version** 7 times), then **Developer > USB Debug**.
  - **Other Experimental Devices (Untested)**:
    - **Samsung Galaxy XR**: Runs the standard build; enable developer options and USB debugging, and allow hand tracking.
    - **Steam Frame**: Experimental native app built from source: see [headset/linux](headset/linux/README.md).
- For VR games: Steam and SteamVR.
- A good network: PC on Ethernet and the headset on 5 GHz or 6 GHz Wi-Fi works best.

## Quick start

1. Download `RinCast-<version>-Setup.exe` and `RinCast-<version>.apk` from the [releases page](../../releases).
2. Run `RinCast-<version>-Setup.exe`. Windows asks for administrator rights.
3. Install the app on the headset: `adb install -r RinCast-<version>.apk`, or use a sideloading tool such as SideQuest.
4. On the headset, open **RinCast** from your apps (on Quest under **Unknown Sources**).
5. Choose your PC from the list, and select **Allow** on the PC the first time.

> **Firewall Note**: The installer opens the firewall for Private and Domain networks. If Windows detects your network as Public, switch to Private or check "Allow headsets on Public networks" in Streamer settings.

## Audio

RinCast streams PC audio to the headset, and sends the headset microphone to the PC:

| Mode | When | What you get |
|---|---|---|
| **Steam's streaming devices** | Default for releases | PC audio and microphone via "Steam Streaming Speakers" and "Steam Streaming Microphone". |
| **RinCast's own driver** | Only with signed driver | Audio via "RinCast Speaker" and "RinCast Microphone". |
| **Default devices** | No virtual devices needed | PC audio from normal playback device, without microphone. |

## Connecting with a USB cable

On Quest or PICO, a USB-C (USB 3.0 recommended) cable can replace Wi-Fi to eliminate wireless interference:
1. Turn on USB connection options in headset settings and plug into PC.
2. Allow USB connection prompt on the headset.
3. Choose the PC listed as "USB cable" in the headset.

## PICO Motion Trackers

On a PICO, **VR > PICO Motion Trackers** forwards tracker data to SteamVR, exposing ankle/waist trackers as standard Vive trackers.

## Connecting through a VPN

If using Tailscale or WireGuard:
1. Connect PC and headset to the VPN.
2. PC Streamer shows its VPN IP address.
3. On headset, choose "Enter PC address" and input the IP.

## Connecting from other networks

Sign in to the same RinCast account in Streamer and on headset. A short pairing code will link your devices. Video streams directly peer-to-peer via IPv6 or UDP hole-punching.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md):

```powershell
scripts\build-all.ps1           # builds everything into dist\
```

## Documentation

- [docs/FEATURES.md](docs/FEATURES.md): feature checklist.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): system architecture.
- [docs/PROTOCOL.md](docs/PROTOCOL.md): wire protocol.
- [docs/BUILDING.md](docs/BUILDING.md): building and releasing.

| Component | Path | What it is |
|---|---|---|
| RinCast Streamer | `pc/streamer` | Windows tray app with WebView2 UI: capture, encoding, input, audio, pairing and SteamVR control. |
| SteamVR driver | `pc/driver-openvr` | Registers RinCast headset and controllers in SteamVR. |
| Audio driver | `pc/audio-driver` | Optional kernel driver for RinCast virtual audio devices. |
| Headset app | `headset` | Native OpenXR app for Quest and PICO. |
| Account server | `server` | Go service for accounts, pairing codes and rendezvous. |
| rincast-core | `shared` | Protocol, crypto, transport, FEC and discovery code. |

## Licence

MIT licence (except `pc/audio-driver/src` under MS-PL). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

RinCast is an independent project. It isn't affiliated with Meta, Oculus, PICO, ByteDance, Virtual Desktop Inc., Valve or Microsoft.
