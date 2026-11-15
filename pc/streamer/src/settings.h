// Persistent streamer settings (%LOCALAPPDATA%\RinCast\settings.json).
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "util.h"

namespace rcs {

struct TrustedDevice {
    std::string id;
    std::string name;
    std::string model;
    int64_t firstSeen = 0;  // unix seconds
    int64_t lastSeen = 0;
    std::string publicKey;  // hex; pinned the first time the headset proves one
};

struct Settings {
    // Identity
    std::string serverId;     // stable random id for this PC
    std::string pcName;

    // General
    bool startWithWindows = true;
    bool startMinimized = true;
    bool preventSleep = true;
    bool requireApproval = true;   // ask before a new headset may connect locally
    bool desktopInput = true;      // headsets may use the mouse and keyboard on the desktop
    std::vector<TrustedDevice> trustedDevices;

    // Video
    std::string codec = "auto";    // auto | h264 | hevc | av1
    bool tenBit = false;
    int vrBitrateMbps = 150;
    // The headset sends its own bitrate choice and it wins by default: it plans its
    // decode buffers around that number, and a PC-side ceiling it didn't expect can
    // starve or flood them. This switch makes the PC's number apply anyway. Opt-in.
    bool pcBitrateWins = false;
    int desktopBitrateMbps = 40;
    bool adaptiveBitrate = true;
    // What the automatic-bitrate switch now selects: 0 = latency, 1 = balanced,
    // 2 = picture quality. Stored as an int so the settings file stays readable.
    // See rc/stream_profile.h for what each value actually changes.
    int streamProfile = 1;
    // How a LAN stream paces its datagrams (PLAN-pacing-fix): 0 = auto (burst on the
    // LAN, the legacy follow on a remote path), 1 = off, 2 = follow the guard cap
    // (the legacy self-locking behavior), 3 = burst. See VideoSender::SetPacingMode.
    int videoPacingMode = 0;
    // CBR for the Best-picture profile: "the number I set is what every frame costs".
    // Measured on real headset frames at +7.5% on stills vs capped VBR (encode-bench,
    // 2026-11-15). The queue risk that retired CBR is back with it - the congestion
    // guard carries fixed bitrate, but the switch stays opt-in and off by default.
    bool qualityProfileCbr = false;
    // When the user lets the Best-picture profile drive video settings, it may raise
    // video-side choices on its own (right now: the fovea's full-density share, see
    // QualityProfileFovea below). It only ever raises; a higher choice of the user's
    // or the headset's stays. Default off: profiles then change rate control only,
    // exactly like the builds before it.
    bool profileControlsVideo = false;
    int fecPercent = 5;
    bool foveatedEncoding = false;  // VR: fewer bits towards the lens edges
    bool adaptiveQuantization = true;
    bool twoPassEncoding = false;  // VR only; desktop streams always use two passes
    int desktopMaxHeight = 0;      // 0 = native
    int desktopFps = 0;            // 0 = follow headset refresh rate (capped at 90)

    // Audio
    bool streamAudio = true;
    bool micPassthrough = true;
    bool switchDefaultAudio = true;
    bool playOnPcToo = false;
    std::string audioDevices = "auto";  // auto | rincast | steam | system

    // VR
    std::string openxrRuntime = "steamvr";  // rincast | steamvr (what we make active while streaming)
    bool autoRegisterSteamVrDriver = true;

    // Cloud
    std::string cloudUrl = "https://rincast.com";
    bool allowRemote = true;
    std::string cloudRefreshTokenProtected;  // DPAPI
    std::string cloudUserName;
    std::string cloudEmail;

    json ToJson(bool forUi) const;
    void FromJson(const json& j);
};

// The fovea's full-density share the Best-picture profile asks for when it may
// drive video settings (rcs::Settings::profileControlsVideo). Pure arithmetic so
// the offline core tests check the same number the session feeds
// Foveator::EncodedSize; the headset stretches whatever size it is sent, so the
// share and the encoded size can never disagree (see client_session.cpp).
constexpr float kQualityProfileFovea = 0.8f;
constexpr float QualityProfileFovea(float current, bool profileControlsVideo, bool qualityProfile) {
    return profileControlsVideo && qualityProfile && current < kQualityProfileFovea ? kQualityProfileFovea
                                                                                    : current;
}

// When the Best-picture profile may drive video settings, it also asks NVENC for a
// QP map: fewer bits towards the lens edges, so the centre keeps more of the same
// ceiling. Only ever raises - a map the user enabled themselves stays on. The map is
// computed from the field of view and needs no geometric squeeze; the stream still
// runs it only on unsqueezed pictures (vr_stream.cpp gates it with !job.foveated),
// see DEVIATIONS D-007 for what that means when the headset's own squeeze is on.
constexpr bool QualityProfileQpMap(bool current, bool profileControlsVideo, bool qualityProfile) {
    return current || (profileControlsVideo && qualityProfile);
}

// The Best-picture profile also wants the quick first pass (better rate control:
// measured offline, keyframes came out 38% smaller at the same ceiling, which is
// what spikes the queue at every scene cut or recovery). Only ever raises - a
// two-pass the user enabled stays on. VR stream only; desktop always two-passes
// anyway (see the desktop stream).
constexpr bool QualityProfileTwoPass(bool current, bool profileControlsVideo, bool qualityProfile) {
    return current || (profileControlsVideo && qualityProfile);
}

// Per-profile fovea floors (the share of each eye that stays full-density). The
// gradient 0.6 / 0.7 / 0.8 is what makes the three profiles differ where it always
// shows - encoded pixels - because the queue budgets alone never bind on a real
// link (measured 2026-11-14: 0 dips across all three). 0.6 mirrors
// Foveator::kFixedFoveaSize, the lens-centre share the headset's own fixed squeeze
// already uses: the latency floor is a no-op there and a small raise only for
// eye-tracked headsets. profile: 0 = latency, 1 = balanced, 2 = quality
// (rc::StreamProfile's ids).
constexpr float kLatencyProfileFovea = 0.6f;
constexpr float kBalancedProfileFovea = 0.7f;
constexpr float ProfileFoveaFloor(int profile) {
    return profile == 2 ? kQualityProfileFovea
           : profile == 1 ? kBalancedProfileFovea
                          : kLatencyProfileFovea;
}
constexpr float QualityProfileFoveaFor(float current, bool profileControlsVideo, int profile) {
    return profileControlsVideo && current < ProfileFoveaFloor(profile) ? ProfileFoveaFloor(profile) : current;
}

// CBR for the Best-picture profile: every frame costs exactly the limit. Measured
// on real headset frames at +7.5% on stills vs capped VBR (encode-bench,
// 2026-11-15) - the queue risk that retired CBR is back with it, so the switch is
// opt-in, off by default, and the congestion guard carries the fixed rate.
constexpr bool QualityProfileCbr(bool cbrSwitch, bool profileControlsVideo, bool qualityProfile) {
    return cbrSwitch && profileControlsVideo && qualityProfile;
}

// Which ceiling a VR stream runs at: the headset's choice wins unless the PC user
// explicitly overrides it (rcs::Settings::pcBitrateWins). A headset that asked for
// 123 planned its buffers around 123; headset <= 0 = it sent no number.
constexpr int EffectiveVrBitrate(int pcMbps, int headsetMbps, bool pcWins) {
    return pcWins || headsetMbps <= 0 ? pcMbps : headsetMbps;
}

class SettingsStore {
   public:
    static SettingsStore& Get();

    Settings Snapshot() const;
    void Update(const std::function<void(Settings&)>& mutate);  // mutates + saves
    void Load();

   private:
    void SaveLocked();
    mutable std::mutex m_mutex;
    Settings m_settings;
};

}  // namespace rcs
