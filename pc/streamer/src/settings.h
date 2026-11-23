// Persistent streamer settings (%LOCALAPPDATA%\RinCast\settings.json).
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "rc/settings_migration.h"
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

// A84 Q-persist (TASK-A84 §4): the file is saved WHOLE - SettingsStore::SaveLocked
// dumps every key through ToJson, so an existing install already carries an
// explicit "autoTune": false written back when that was still the default. A
// flipped default alone would therefore never reach the machines that predate it.
// The file now stamps which settings schema wrote it: a file with no stamp was
// written before A84 and gets the one-time migration, while a choice made on a
// stamped file (turning the tuner off in the UI) rules from then on, across
// restarts.
//
// A84b §2: the stamp value, the threshold and the judgement moved to
// rc/settings_migration.h - Settings::FromJson compiles into rincast-streamer only,
// so nothing in rincast-core-tests could reach the comparison and the migration had
// no test that could fail. Read that header for why the schema starts at 1 (there
// is no v1 anywhere in this repository, so a 2 was an unsourced magic number) and
// for what an unstamped file means.
constexpr int kSettingsSchema = rc::kCurrentSettingsSchema;

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
    // A84 changed the MEANING, not the key or the values: this is the profile the
    // user will accept as the WORST the stream gets (a floor), not the one they
    // asked to run. With autoTune on, the effective profile is auto's and only
    // ever sits at or above this while the link is calm (rc/auto_tune.h).
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
    // VR encode resolution as a percentage of what the game renders (ROUND18 L2).
    // 0 = follow the selected profile's value (all 100 today); >0 = user override,
    // clamped to [50, 100]. Stored as an int so the settings UI radio values bind
    // directly. Lower = the GPU scales the capture down before the encoder, so the
    // headset decodes fewer pixels without asking the game to render less. 0 by
    // default: existing installs keep encoding exactly as before.
    int encodeScalePct = 0;
    // ROUND19 T3: the decode-side autoscale ladder (rc/autoscale.h). When on, the
    // ladder may step the ENCODE scale down 100 -> 85 -> 67 -> 50 (multiplied onto
    // the base above, so it never raises anything) while decode pressure holds;
    // only the user's manual reset climbs back. Default off: the streamer then
    // never intervenes, exactly like the builds before it.
    bool l7Autoscale = false;
    // PLAN-AUTO-TUNE: the automatic tuner (rc/auto_tune.h). When on, it reads
    // the existing signals (decode hint, queue, loss, wire fill, QP) and acts
    // for the user on the dimensions they have not written themselves: the L7
    // ladder, CBR off, foveation on, and (A84) the picture profile BOTH ways.
    // A84 (TASK-A84-PROFILE-FLOOR-TWO-WAY-AUTOTUNE-2026-11-23) flipped the
    // default to ON: the product decision is that the user states a preference
    // on one slider and the system picks the parameters - with the switch off
    // nothing runs and the slider is an absolute choice again, which is the
    // pre-A84 branch. See AUTO-TUNE-2026-11-19/
    // DECISION-TABLE.md for every threshold's evidence.
    bool autoTune = true;
    // L17 fill mode: when on, the encoder frame-rate rung may follow the slow fps
    // EMA down (floor 45) so a low-fps heavy scene keeps its full per-frame budget.
    // Default on: the real-device A/B (AUTO-TUNE-2026-11-19/ACCEPTANCE-C.md) held
    // the same fps band at +56% bytes per frame, QP>=20 windows 11 -> 1, zero
    // dropped frames. See L17-FILL-DESIGN-2026-11-19/DESIGN.md for the design.
    bool fillMode = true;
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

// The Best-picture profile's full-density centre share (0.8), used by the
// per-profile floor below. The headset stretches whatever size it is sent, so
// the share and the encoded size can never disagree (see client_session.cpp).
constexpr float kQualityProfileFovea = 0.8f;

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
