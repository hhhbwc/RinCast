# RinCast — capped-VBR fork

<!-- FORK-DOC: fork-specific doc, lives alongside the upstream README.md. Chinese: FORK-README.zh-CN.md -->

[English](FORK-README.md) | [中文](FORK-README.zh-CN.md)

This is `next/capped-vbr`, a fork of [TheReal-Flo/RinCast](https://github.com/TheReal-Flo/RinCast).
Same product — PC streamer plus Quest/PICO headset app; games, input, audio and discovery are all upstream's.
What changed is how the video bitrate gets spent: a capped-VBR controller with a queue-feedback guard,
and three quality profiles you can actually pick in the UI. If something breaks that isn't video-related,
it's probably an upstream bug — file it there unless you can reproduce it on this branch.

## What changed

79 commits ahead of upstream `main` as of base `cf9a590` — that's this document's snapshot;
run `git rev-list --count origin/main..HEAD` for the current number. By theme:

| Area | Change |
|---|---|
| Rate control | capped VBR: the average target stays at the ceiling so complex scenes can spend it; a QP floor stops still frames from wasting bits; VBV is sized from the *nominal* frame rate — frame-rate compensation is frozen at nominal (501e185) |
| Feedback guard | watches headset-reported queue delay and holds the send rate just under what the path carries; dips fast, climbs slow, with a dead band and a startup grace for the first keyframe |
| Profiles | latency-first / balanced / quality-first — each sets a queue budget and a QP floor |
| Pacing | burst pacer follows the user's ceiling, not the guard's temporary cap; wire overhead is accounted for |
| Observability | 1 Hz logs on both ends (`rc diag 1s` on PC, `rc vr rx 1s` / `rc vr diag 1s` on headset) so latency, QP, queue, loss and frames can be lined up on one timeline |
| Fixes | settings rebuild storms, a handful of i18n copy bugs, invalid NVENC QP samples kept out of telemetry |

## What this was tuned on

Every number below was measured on one specific setup. They are not universal constants.

- PC: Windows 11, RTX 4060 Laptop, NVENC HEVC.
- Headset: PICO 4 (A8110), 2688×2688 per eye, 72–90 fps target.
- Network: PC on Ethernet into a 192.168.x.1 router, headset on that router's 5 GHz AP, same L2 segment,
  nothing proxied in between. Clean link: 0% loss, ~1 ms RTT. Tests used a 100 Mbps ceiling.
- Content: SteamVR games, mostly rendering ~32–58 fps (GPU-bound), moderate to heavy scenes.

Different machine? The design still applies; the numbers may not be optimal. That's the next section.

## Will it work on my machine?

Three layers, honestly:

1. **Architecture — portable.** VBV = ceiling × per-profile frame-percent ÷ nominal fps. Pure arithmetic,
   scales with resolution and frame rate: 150% (1.5 frames) for latency-first and balanced, 200% (2 frames)
   for quality-first. (The 150% constant you'll see in the encoder code is only the legacy CBR fallback.)
   The queue guard works in relative adjustments against the device's own idle queue; half-rate is decided
   by bits-per-pixel. None of that cares what hardware you have.
2. **Calibrated numbers — need re-measuring.** The 15/45/60 ms queue budgets came from this link's ~27–30 ms
   idle AP queue; the QP floors 24/16/12 and the dead-band/climb timings were tuned by looking at a real
   headset. Change the network, resolution or content and they're not necessarily optimal — not broken,
   just untuned.
3. **Encoder behaviour — hardware-bound.** "VBV needs at least 1.5 frames to converge" is an NVENC
   measurement. AMD AMF and Intel QuickSync behave differently.

## Known limitations

Read these before judging the branch:

- **Under heavy load the three profiles converge.** On the tuned GPU, complex scenes plus ~55 fps render
  push the encoder near its limit (~17–20 ms per frame encode, gpu-wait climbing). All profiles end up
  around QP 20 and no ceiling change separates them. That's an encoder ceiling, not a parameter bug —
  the only real fixes are lower resolution/frame-rate targets or a faster GPU.
- **Quality-first breathing: floor added, effect unverified.** With no QP floor, QP swung 3–25 in a
  light-load session and 9–29 under the pressure map (both measured 2026-11-17, before the floor existed) —
  you can see it as a clear/soft cycle. c298f1f added a mild floor of 12, intending to stop the swing;
  whether it actually does hasn't been verified on a real device yet. The floor applies in capped-VBR mode,
  which is the default — the CBR switch is opt-in, off by default, and with CBR on the floor doesn't apply.
  Balanced (floor 16) and latency-first (floor 24) are steadier.
- **"Bitrate won't fill the ceiling" is usually the input frame rate, not a bug.** At 45 fps the same content
  spends about half the bits of 90 fps — roughly 125 KB/frame either way.
- **This branch is about latency/bitrate behaviour, not sharper pictures.** At identical settings don't expect
  more detail than upstream; expect steadier behaviour near the link limit.
- **Headset build** here is the standard PICO 4 debug APK. Other headsets are upstream's scope.

## Reproducing the measurements

The toolchain lives in [`tools/diagnostics/`](tools/diagnostics/README.md): log capture, an offline extreme
matrix, an encode bench, and a dual-end log analyzer. The offline matrix can prove "nothing got worse".
It cannot prove the picture looks good — that takes a real headset and a real game.

## License

MIT, see [`LICENSE`](LICENSE). The `pc/audio-driver/src` exception noted there still applies.
Upstream third-party notices are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).