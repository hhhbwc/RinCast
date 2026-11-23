#include "client_session.h"
#include "../vr/face_share.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>  // TASK-A80 M1: snprintf for the full-precision geometry readings
#include <cstring>

#include "../app.h"
#include "../audio/audio.h"
#include "../encode/encoder.h"
#include "../input/input_injector.h"
#include "../json_field.h"
#include "../settings.h"
#include "../stream/desktop_stream.h"
#include "../stream/foveator.h"
#include "../stream/stream_config.h"  // the settings -> config builders (TASK-COMBO-SWEEP-2026-11-21)
#include "../stream/vr_stream.h"
#include "../vr/steamvr.h"
#include "../vr/vr_bridge_host.h"
#include "rc/link_policy.h"
#include "rc/platform.h"
#include "rc/stream_profile.h"

namespace rcs {

namespace {

// QualityScale and Align moved to stream/stream_config.h (the pure config builders;
// TASK-COMBO-SWEEP-2026-11-21) so the offline sweep shares the one definition.

// The most the stream takes of SteamVR's resolution, against the headset's panel (the slider on
// the headset stops there too): Virtual Desktop's highest. Headsets before 1.2.0 send their
// runtime's recommended size instead, smaller, and get less. A codec or decoder that can't take
// the size still brings it down below.
constexpr double kMaxVrScale = 1.5;

// A86b: `profileFloor=` stays in the periodic AUTO line and the tier-transition
// line byte-for-byte (analyze_dual.py reads by position). With the slider gone it
// is the tuner's START rung, not a user-set floor - a fixed 1 (balanced). Do not
// remove or rename the field just because the concept behind it was deleted.
constexpr int kStartRungForLog = 1;

// A86b: the settings-file escape hatch is gone (a stale "autoTune": false with no
// UI to reverse it was a silent trap). Remote support can still pin a rung with
// RINCAST_FORCE_RUNG=0|1|2; read once per process, never written to disk, and an
// invalid value is ignored rather than clamped into a different picture.
int ForcedRung() {
    static const int rung = [] {
        const char* text = std::getenv("RINCAST_FORCE_RUNG");
        if (!text || !*text) return -1;
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        return (end && *end == '\0' && value >= 0 && value <= 2) ? int(value) : -1;
    }();
    return rung;
}

// ---------------------------------------------------------------------------
// TASK-A80 stage M1: geometry SHAPE readings. Diagnostics only - nothing below
// changes a size, a field of view, or any decision (the card forbids it, and the
// fix is stage F, which waits for these numbers). Three shapes have to be read
// side by side, and no line ever printed two of them together:
//   announced - DisplayConfig.eyeWidth/eyeHeight, what SteamVR is told per eye
//   fov tan   - the fovTan spans, (tanL+tanR) / (tanU+tanD)
//   rendered  - Shared.renderedEyeWidth/Height, what the game really renders at
// fovTan prints at %.9g on purpose: the earlier %.3f print made the values look
// like they could have come from a lookup table (card 1, Q2).
// ---------------------------------------------------------------------------

// One eye's fovTan: the four raw tangents, their angles, the tan spans, their ratio.
static std::string A80FovShape(const float f[4]) {
    const double kRad = 180.0 / 3.14159265358979323846;
    const double tanH = double(f[0]) + double(f[1]);
    const double tanV = double(f[2]) + double(f[3]);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "tan L%.9g R%.9g U%.9g D%.9g deg %.9g %.9g %.9g %.9g tanH %.9g tanV %.9g tanAspect %.9g",
             f[0], f[1], f[2], f[3], std::atan(double(f[0])) * kRad, std::atan(double(f[1])) * kRad,
             std::atan(double(f[2])) * kRad, std::atan(double(f[3])) * kRad, tanH, tanV,
             tanV > 0 ? tanH / tanV : 0.0);
    return std::string(buf);
}

static double A80TanAspect(const float f[4]) {
    const double tanV = double(f[2]) + double(f[3]);
    return tanV > 0 ? (double(f[0]) + double(f[1])) / tanV : 0.0;
}

// A82-M2: renders an enumerated refresh list in the same 60/72/90 shape the AUTO:
// capability line uses (LogAutoCapability) so the clamp WARN carries the whole list -
// with only the two ends a reader cannot tell a near-miss request from a wildly off
// list. Readings only.
static std::string A82RateList(const std::vector<float>& rates) {
    std::string out;
    for (float r : rates) {
        if (!out.empty()) out += '/';
        out += std::to_string(int(r + 0.5f));
    }
    return out;
}

// The 1 s reading of all three aspects, from the session's existing 1 s tick. The WARN
// the card asks for is rate limited to 10 s because on the affected device the condition
// holds for the whole session (the driver cannot hot-change the resolution) - the aspects
// themselves stay in the 1 s INFO, so nothing is lost between warnings.
static void LogVrShapeReadings() {
    auto& bridge = App::Get().Bridge();
    const rcvr::DisplayConfig dc = bridge.LastConfig();
    if (!dc.eyeWidth || !dc.eyeHeight) return;  // no VR stream configured yet
    const double announced = double(dc.eyeWidth) / double(dc.eyeHeight);
    const double tan = A80TanAspect(dc.fovTan[0]);
    uint32_t rw = 0, rh = 0;
    const bool haveRendered = bridge.SteamVrEyeSize(rw, rh);
    const double rendered = haveRendered ? double(rw) / double(rh) : 0.0;
    const std::string renderedText = haveRendered ? std::to_string(rw) + "x" + std::to_string(rh) : "none yet";
    RC_LOGI("A80-M1 shape 1s: announced %ux%u aspect %.9g | fov e0 [%s] | rendered %s aspect %.9g"
            " | renderEye %ux%u (0x0 = following the headset's size, not SteamVR's)",
            dc.eyeWidth, dc.eyeHeight, announced, A80FovShape(dc.fovTan[0]).c_str(), renderedText.c_str(), rendered,
            dc.renderEyeWidth, dc.renderEyeHeight);
    // The two 2 % tests the card names. They gate THIS WARNING and nothing else - no
    // value here feeds back into dc, the encoder, or the driver.
    const bool fovOff = tan > 0 && std::fabs(announced - tan) / tan > 0.02;
    const bool renderOff = haveRendered && std::fabs(rendered - announced) / announced > 0.02;
    if (!fovOff && !renderOff) return;
    static thread_local int64_t lastWarnNs = 0;
    const int64_t now = rc::NowNs();
    if (lastWarnNs && now - lastWarnNs < 10'000'000'000LL) return;
    lastWarnNs = now;
    RC_LOGW("A80-M1 SHAPE MISMATCH (announced %.9g vs fov-tan %.9g vs rendered %.9g): %s%s", announced, tan, rendered,
            fovOff ? "announced-vs-fov-tan over 2%, " : "", renderOff ? "announced-vs-rendered over 2%" : "");
}

// A19 audit #5: readable names for the synced keys in the ownership notice.
std::string SyncedKeyTitle(const std::string& key) {
    if (key == rc::synced::kCodec) return "Video codec";
    if (key == rc::synced::kTenBit) return "10-bit color";
    if (key == rc::synced::kAdaptiveBitrate) return "Adaptive bitrate";
    if (key == rc::synced::kDesktopFps) return "Desktop frame rate";
    if (key == rc::synced::kStreamProfile) return "Quality profile";
    return key;
}

// A19 audit #10: one short code for "what's wrong on this PC", for headsets that
// surface server.info's diagnosis. Derived from the same checks the PC page's
// health() makes (firewall, USB adapter, SteamVR, the account relay); first match
// wins; empty = nothing to say, and the key stays off the message.
std::string DiagnosisCode() {
    const json fw = App::Get().FirewallJson();
    if (!fw.value("otherNetworksOpen", true)) return "firewall";
    if (!fw.value("usbAdapter", std::string()).empty() && !fw.value("usbOpen", true)) return "usb";
    if (!steamvr::IsRunning()) return "steamvrDown";
    const json cloud = App::Get().Cloud().StatusJson();
    if (cloud.value("signedIn", false) && cloud.value("allowRemote", false) &&
        cloud.value("status", std::string()) == "error")
        return "remoteDown";
    return {};
}

}  // namespace

ClientSession::ClientSession(std::unique_ptr<rc::Connection> connection, const HelloInfo& hello,
                             std::string clientPkHex, std::vector<uint8_t> welcome, bool remote, bool vpn)
    : m_connection(std::move(connection)),
      m_hello(hello),
      m_clientPk(std::move(clientPkHex)),
      m_welcome(std::move(welcome)),
      m_remote(remote),
      m_vpn(vpn),
      m_slowPath(remote || vpn) {
    m_connectedAtNs = rc::NowNs();
    m_connection->SetControlHandler([this](const std::string& text) { OnControl(text); });
    m_connection->SetPacketHandler(
        [this](rc::PacketType type, const uint8_t* p, size_t n) { OnPacket(type, p, n); });
    if (hello.raw.contains("display") || hello.raw.contains("decoders")) {
        json initialInfo = hello.raw;
        initialInfo.erase("_pcRevisions");
        ApplyClientInfo(initialInfo);
    }
    m_inputAllowed = SettingsStore::Get().Snapshot().desktopInput;
}

// Picks up the setting. With announce, tells the headset when it changed, so a click that does
// nothing isn't a mystery.
void ClientSession::ApplyInputSetting(bool announce) {
    const bool allowed = SettingsStore::Get().Snapshot().desktopInput;
    const bool was = m_inputAllowed.exchange(allowed);
    if (!allowed && was) input::ReleaseAll();  // nothing stays held down
    if (announce && allowed != was) {
        SendControl({{"type", rc::msg::kNotice},
                     {"level", "info"},
                     {"text", allowed ? "This PC now lets the headset use its mouse and keyboard."
                                      : "This PC turned off the headset's mouse and keyboard. You can still watch."}});
    }
}

ClientSession::~ClientSession() { Close("destroyed"); }

void ClientSession::Start() {
    m_worker = std::thread([this] { WorkerLoop(); });
}

void ClientSession::Close(const std::string& reason) {
    if (m_closed.exchange(true)) return;
    RC_LOGI("Session with %s closed (%s)", m_hello.deviceName.c_str(), reason.c_str());
    m_connection->SendDisconnect();
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.clear();
    }
    m_queueCv.notify_all();
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();

    std::map<uint8_t, std::unique_ptr<DesktopStream>> desktop;
    std::unique_ptr<VrStream> vr;
    std::unique_ptr<AudioStreamer> audioOut;
    std::unique_ptr<MicPlayer> mic;
    {
        std::lock_guard lock(m_mutex);
        desktop.swap(m_desktop);
        vr.swap(m_vr);
        audioOut.swap(m_audio);
        mic.swap(m_mic);
    }
    desktop.clear();
    vr.reset();
    audioOut.reset();
    mic.reset();
    App::Get().Bridge().SetHeadsetConnected(false);
    audio::RestoreDefaults();
    input::ReleaseAll();
}

void ClientSession::HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    if (m_closed) return;
    m_connection->HandleDatagram(data, size, from);
}

void ClientSession::Tick(int64_t nowNs) {
    if (m_closed) return;
    m_connection->Tick(nowNs);
    static thread_local int64_t lastStats = 0;
    if (nowNs - lastStats > 1'000'000'000LL) {
        lastStats = nowNs;
        Enqueue(json{{"type", "_tick"}});
    }
}

void ClientSession::SendControl(const json& message) {
    if (m_closed) return;
    m_connection->SendControl(message.dump());
}

ClientCaps ClientSession::Caps() {
    std::lock_guard lock(m_mutex);
    return m_caps;
}

ClientStreamSettings ClientSession::StreamSettings() {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

json ClientSession::SyncedSettingsForUi() {
    std::lock_guard lock(m_mutex);
    const Settings s = SettingsStore::Get().Snapshot();
    json out = s.ToJson(true);
    out[rc::synced::kCodec] = s.codec;
    out[rc::synced::kTenBit] = s.tenBit;
    out[rc::synced::kAdaptiveBitrate] = s.adaptiveBitrate;
    out[rc::synced::kDesktopFps] = s.desktopFps;
    out[rc::synced::kStreamProfile] = s.streamProfile;
    for (const char* key : rc::synced::kAll) {
        if (m_ownedSettings.count(key)) continue;  // PC still owns it
        if (std::string(key) == rc::synced::kCodec) out[key] = m_settings.codec;
        else if (std::string(key) == rc::synced::kTenBit) out[key] = m_settings.tenBit;
        else if (std::string(key) == rc::synced::kAdaptiveBitrate) out[key] = m_settings.adaptiveBitrate;
        else if (std::string(key) == rc::synced::kDesktopFps) out[key] = m_settings.desktopFps;
        else if (std::string(key) == rc::synced::kStreamProfile) out[key] = m_settings.streamProfile;
    }
    return out;
}

bool ClientSession::AdoptHeadsetOwnedSettings() {
    // Ownership and persistence share the PC edit's lock, so no newer PC write is overwritten.
    std::lock_guard lock(m_mutex);
    const Settings pc = SettingsStore::Get().Snapshot();
    // A86 (user, 2026-11-23): the picture tier is no longer a synced choice. The PC
    // used to adopt a headset-owned streamProfile into its settings so the headset's
    // slider became the tuner's floor; both sliders are gone now, so a value a
    // headset still sends is IGNORED - never adopted. Adaptive bitrate keeps the
    // pre-A86 rule untouched.
    const bool adaptiveDiff = !m_ownedSettings.count(rc::synced::kAdaptiveBitrate) &&
                              pc.adaptiveBitrate != m_settings.adaptiveBitrate;
    if (!adaptiveDiff) return false;
    SettingsStore::Get().Update([&](Settings& s) { s.adaptiveBitrate = m_settings.adaptiveBitrate; });
    return true;
}

// ---------------------------------------------------------------------------

void ClientSession::OnControl(const std::string& text) {
    json m;
    try {
        m = json::parse(text);
    } catch (...) {
        RC_LOGW("Malformed control message");
        return;
    }
    if (!m.is_object()) return;
    // This runs on the receive thread, so every read here is one that must not throw: a control
    // message whose "type" is a number used to end the process (json_field.h).
    const std::string type = StrField(m, "type");
    if (type.empty()) return;
    if (!m_inputAllowed && (type == rc::msg::kMouseButton || type == rc::msg::kMouseWheel ||
                            type == rc::msg::kKey || type == rc::msg::kText))
        return;
    if (type == rc::msg::kMouseButton) {
        input::MouseButton(IntField(m, "button", 0), BoolField(m, "down", false));
        return;
    }
    if (type == rc::msg::kMouseWheel) {
        input::MouseWheel(FloatField(m, "dx", 0.f), FloatField(m, "dy", 0.f));
        return;
    }
    if (type == rc::msg::kKey) {
        input::Key(uint16_t(std::clamp(IntField(m, "vk", 0), 0, 0xFFFF)), BoolField(m, "down", false));
        return;
    }
    if (type == rc::msg::kText) {
        input::Text(Widen(StrField(m, "text")));
        return;
    }
    if (type.front() == '_') return;
    m.erase("_pcRevisions");
    if (m.contains("settings")) {
        // Enqueue merges this into a queued message; a non-object has nothing to merge.
        if (!m["settings"].is_object()) return;
        std::lock_guard lock(m_mutex);
        m["_pcRevisions"] = json::object();
        for (const char* key : rc::synced::kAll)
            if (m["settings"].contains(key)) m["_pcRevisions"][key] = m_pcKeyRevisions[key];
    }
    Enqueue(std::move(m));
}

void ClientSession::Enqueue(json message) {
    std::lock_guard lock(m_queueMutex);
    if (m_closed) return;
    const auto coalesces = [](const std::string& type) {
        return type == rc::msg::kUpdateSettings || type == "_profile" || type == "_serverInfo" ||
               type == "_audio" || type == "_tick" || type == "_monitors";
    };
    const std::string type = message.value("type", "");
    if (coalesces(type)) {
        for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it) {
            const std::string queuedType = it->value("type", "");
            if (!coalesces(queuedType)) break;
            if (queuedType != type) continue;
            if (type == rc::msg::kUpdateSettings) {
                // Merged without value() so a wrong-typed "settings" reads as nothing to merge
                // instead of throwing (this is reached from the receive thread).
                json& queued = (*it)["settings"];
                if (!queued.is_object()) break;
                const json& incoming = ObjectField(message, "settings");
                if (!incoming.empty()) queued.update(incoming);
                if (const auto revisions = message.find("_pcRevisions");
                    revisions != message.end() && revisions->is_object())
                    (*it)["_pcRevisions"].update(*revisions);
            }
            return;
        }
    }
    m_queue.push_back(std::move(message));
    m_queueCv.notify_one();
}

void ClientSession::ScheduleVideoRestart(bool vr, bool desktop) {
    if (!vr && !desktop) return;
    std::lock_guard lock(m_queueMutex);
    m_restartVr |= vr;
    m_restartDesktop |= desktop;
    const int64_t now = rc::NowNs();
    m_restartDueNs = std::max(now + 200'000'000LL, m_lastSettingsRestartNs + 3'000'000'000LL);
    m_queueCv.notify_one();
}

void ClientSession::OnPacket(rc::PacketType type, const uint8_t* payload, size_t size) {
    switch (type) {
        case rc::PacketType::Tracking: {
            if (size < sizeof(rc::TrackingPayload)) return;
            rc::TrackingPayload t;
            std::memcpy(&t, payload, sizeof(t));
            rcvr::TrackingSnapshot s{};
            static std::atomic<uint64_t> seq{0};
            s.sequence = ++seq;
            s.receivedPcNs = rc::NowNs();
            s.headsetTargetNs = t.targetDisplayTimeNs;
            s.targetDisplayPcNs = m_connection->HasClockOffset()
                                      ? m_connection->RemoteToLocalNs(int64_t(t.targetDisplayTimeNs))
                                      : s.receivedPcNs + 40'000'000;
            s.flags = t.flags;
            std::memcpy(s.hmd.orientation, t.hmd.orientation, sizeof(float) * 4);
            std::memcpy(s.hmd.position, t.hmd.position, sizeof(float) * 3);
            std::memcpy(s.hmd.linearVelocity, t.hmdLinearVelocity, sizeof(float) * 3);
            std::memcpy(s.hmd.angularVelocity, t.hmdAngularVelocity, sizeof(float) * 3);
            for (int h = 0; h < 2; h++) {
                const auto& c = t.controllers[h];
                auto& d = s.controllers[h];
                d.flags = c.flags;
                d.buttons = c.buttons;
                d.trigger = c.trigger;
                d.grip = c.grip;
                d.thumbstick[0] = c.thumbstick[0];
                d.thumbstick[1] = c.thumbstick[1];
                std::memcpy(d.pose.orientation, c.pose.orientation, sizeof(float) * 4);
                std::memcpy(d.pose.position, c.pose.position, sizeof(float) * 3);
                std::memcpy(d.pose.linearVelocity, c.linearVelocity, sizeof(float) * 3);
                std::memcpy(d.pose.angularVelocity, c.angularVelocity, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishTracking(s);
            MeasurePoseWait(s.receivedPcNs);
            // Where the eyes look comes after the poses, from headsets doing foveated encoding.
            if (size >= sizeof(t) + sizeof(rc::GazePayload)) {
                rc::GazePayload g;
                std::memcpy(&g, payload + sizeof(t), sizeof(g));
                std::lock_guard lock(m_mutex);
                if (m_vr) m_vr->SetGaze(g);
            }
            break;
        }
        case rc::PacketType::BodyTracking: {
            if (size < sizeof(rc::BodyTrackingPayload)) return;
            static_assert(rc::kBodyTrackerCount == rcvr::kBodyTrackers);
            rc::BodyTrackingPayload b;
            std::memcpy(&b, payload, sizeof(b));
            rcvr::BodySnapshot s{};
            s.receivedPcNs = rc::NowNs();
            s.validMask = b.validMask;
            for (int i = 0; i < rc::kBodyTrackerCount; i++) {
                std::memcpy(s.poses[i].orientation, b.poses[i].orientation, sizeof(float) * 4);
                std::memcpy(s.poses[i].position, b.poses[i].position, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishBody(s);
            break;
        }
        case rc::PacketType::FaceTracking: {
            if (size < sizeof(rc::FaceTrackingPayload)) return;
            rc::FaceTrackingPayload f;
            std::memcpy(&f, payload, sizeof(f));
            FaceShare::Get().Publish(f);
            break;
        }
        case rc::PacketType::HandSkeleton: {
            if (size < sizeof(rc::HandSkeletonPayload)) return;
            static_assert(rc::kHandBoneCount == rcvr::kHandBones);
            rc::HandSkeletonPayload h;
            std::memcpy(&h, payload, sizeof(h));
            if (h.hand > 1) return;
            rcvr::HandSnapshot s{};
            s.receivedPcNs = rc::NowNs();
            for (int i = 0; i < rc::kHandBoneCount; i++) {
                std::memcpy(s.bones[i].orientation, h.bones[i].orientation, sizeof(float) * 4);
                std::memcpy(s.bones[i].position, h.bones[i].position, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishHand(h.hand, s);
            break;
        }
        case rc::PacketType::Pointer: {
            if (size < sizeof(rc::PointerPayload)) return;
            rc::PointerPayload p;
            std::memcpy(&p, payload, sizeof(p));
            if (!p.visible) return;
            std::lock_guard lock(m_mutex);
            auto it = m_desktop.find(p.monitorStreamId);
            if (it != m_desktop.end()) {
                if (m_inputAllowed) input::MoveMouse(it->second->Monitor(), p.u, p.v);
                if (m_lastMouseMonitor != p.monitorStreamId) {
                    m_lastMouseMonitor = p.monitorStreamId;
                    for (auto& [id, s] : m_desktop) s->SetActive(true);
                }
            }
            break;
        }
        case rc::PacketType::Audio: {
            if (size < sizeof(rc::AudioPacketHeader)) return;
            if (payload[0] != uint8_t(rc::AudioStream::MicToPc)) return;
            std::lock_guard lock(m_mutex);
            if (m_mic) m_mic->PushPacket(payload, size);
            break;
        }
        case rc::PacketType::VideoFeedback: {
            if (size < rc::kVideoFeedbackMinSize) return;
            rc::VideoFeedbackPayload fb{};
            fb.queueDelayMs = -1;  // stays so when an older headset doesn't send it
            std::memcpy(&fb, payload, std::min(size, sizeof(fb)));
            std::lock_guard lock(m_mutex);
            if (fb.streamId == rc::kVrStreamId) {
                if (m_vr) m_vr->OnFeedback(fb);
            } else if (auto it = m_desktop.find(fb.streamId); it != m_desktop.end()) {
                it->second->OnFeedback(fb);
            }
            break;
        }
        default:
            break;
    }
}

void ClientSession::WorkerLoop() {
    ScopedCoInit com;
    // Greet: announce what this PC can do.
    SendServerInfo();
    SendMonitors();
    while (!m_closed) {
        json m;
        {
            std::unique_lock lock(m_queueMutex);
            while (!m_closed && m_queue.empty()) {
                if (!m_restartDueNs) {
                    m_queueCv.wait(lock);
                } else {
                    const int64_t remaining = m_restartDueNs - rc::NowNs();
                    if (remaining <= 0) break;
                    m_queueCv.wait_for(lock, std::chrono::nanoseconds(remaining));
                }
            }
            if (m_closed) break;
            if (!m_queue.empty()) {
                m = std::move(m_queue.front());
                m_queue.pop_front();
            } else {
                m = {{"type", "_rebuild"}, {"vr", m_restartVr}, {"desktop", m_restartDesktop}};
                m_restartVr = m_restartDesktop = false;
                m_restartDueNs = 0;
                m_lastSettingsRestartNs = rc::NowNs();
            }
        }
        try {
            HandleMessage(m);
        } catch (const std::exception& e) {
            RC_LOGW("Error handling %s: %s", m.value("type", "?").c_str(), e.what());
        }
    }
}

void ClientSession::HandleMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "_tick") {
        // Steam VR launch timeout.
        if (m_steamVrLaunchNs && rc::NowNs() - m_steamVrLaunchNs > 90'000'000'000LL) {
            m_steamVrLaunchNs = 0;
            NotifyVrState("error", "steamvr", "", "SteamVR did not start. Check SteamVR on your PC.");
        }
        FollowSteamVrResolution();
        // A80-M1: the announced / fov-tan / rendered aspects on this same 1 s tick.
        // Purely a reading; it decides nothing and changes nothing.
        bool vrStreamLive = false;
        {
            std::lock_guard lock(m_mutex);
            vrStreamLive = m_vr != nullptr;
        }
        if (vrStreamLive) LogVrShapeReadings();
        // Server statistics for the headset's performance overlay.
        json stats{{"type", rc::msg::kServerStats}};
        {
            std::lock_guard lock(m_mutex);
            if (m_vr) {
                auto s = m_vr->Stats();
                stats["vr"] = {{"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps},
                               {"bitrateKbps", s.bitrateKbps}, {"framesLost", s.framesLost},
                               {"qp", s.qp}, {"encWidth", s.encWidth}, {"encHeight", s.encHeight},
                               {"gameFps", s.gameFps}, {"encFps", s.encFps}, {"droppedFps", s.droppedFps}};
                // A19 audit #2: the headset is on the desktop / in a menu, the encoder
                // is paused and these numbers freeze. The additive key marks them; the
                // cadence and the fields are unchanged (old headsets ignore the key).
                if (m_vr->Paused()) stats["vr"]["paused"] = true;
            }
            json desk = json::array();
            for (auto& [id, d] : m_desktop) {
                auto s = d->Stats();
                desk.push_back({{"streamId", id}, {"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps},
                                {"bitrateKbps", s.bitrateKbps}});
            }
            stats["desktop"] = desk;
        }
        stats["rttMs"] = m_connection->Stats().rttMs;
        SendControl(stats);
        return;
    }
    if (type == "_monitors") return SendMonitors();
    if (type == "_audio") return UpdateAudio();
    if (type == "_profile") return ScheduleVideoRestart(true, true);
    if (type == "_rebuild") return RestartVideoForProfile(m.value("vr", false), m.value("desktop", false));
    if (type == "_serverInfo") return SendServerInfo();
    if (type == rc::msg::kClientInfo) {
        ApplyClientInfo(m);
        AdoptHeadsetOwnedSettings();
        LogAutoCapability();
        SendServerInfo();
        SendMonitors();
        StartVr();
        UpdateAudio();
        return;
    }
    if (type == rc::msg::kUpdateSettings) {
        ClientStreamSettings before = StreamSettings();
        ApplyClientInfo(m);
        ClientStreamSettings after = StreamSettings();
        // A82-M3: the PC-visible half of the headset's rate chase. FollowDisplayRate
        // (headset/app/src/main/cpp/app/app.cpp:292-309) pushes the panel's ACTUAL rate
        // through settings.refreshRate after 6 s of divergence; KeepRefreshRate's 2 s retry
        // loop (xr_context.cpp:762-780) never reaches the wire. Readings only.
        if (before.refreshRate != after.refreshRate)
            RC_LOGI("Headset moved refreshRate %.0f -> %.0f Hz (its panel's actual rate) - the stream rebuilds at it",
                    before.refreshRate, after.refreshRate);
        AdoptHeadsetOwnedSettings();
        // A84 §2 (was PLAN-AUTO-TUNE's MarkManual here): a streamProfile the
        // headset sends with a value the PC never pushed is the user moving their
        // slider, which now STATES A FLOOR and hands the dimension back - it does
        // not lock auto out of it. The floor reaches the tuner as
        // AutoTuneInput::profileFloorId, where the change itself clears the lock.
        // (The synced key is still never written by auto: it moves only the
        // session's effective profile, so there is nothing left to fight over.)
        const bool profileChanged = before.streamProfile != after.streamProfile;
        const bool videoChanged = before.codec != after.codec || before.tenBit != after.tenBit ||
                                  before.vrQuality != after.vrQuality || before.vrScale != after.vrScale ||
                                  before.matchSteamVr != after.matchSteamVr || before.gazeFoveation != after.gazeFoveation ||
                                  before.eyesTracked != after.eyesTracked ||
                                  before.frameGeneration != after.frameGeneration || before.refreshRate != after.refreshRate ||
                                  before.debugEncodeScalePct != after.debugEncodeScalePct ||
                                  before.vrBitrateMbps != after.vrBitrateMbps ||
                                  before.adaptiveBitrate != after.adaptiveBitrate;
        const bool desktopChanged = before.codec != after.codec || before.tenBit != after.tenBit ||
                                    before.desktopFps != after.desktopFps ||
                                    before.desktopBitrateMbps != after.desktopBitrateMbps ||
                                    before.adaptiveBitrate != after.adaptiveBitrate;
        // A profile change and its video changes share one restart per stream.
        ScheduleVideoRestart(videoChanged || profileChanged, desktopChanged || profileChanged);
        if (before.streamAudio != after.streamAudio || before.micPassthrough != after.micPassthrough)
            Enqueue(json{{"type", "_audio"}});
        const bool syncedChanged = before.codec != after.codec || before.tenBit != after.tenBit ||
                                   before.adaptiveBitrate != after.adaptiveBitrate ||
                                   before.desktopFps != after.desktopFps || profileChanged;
        if (syncedChanged) App::Get().PushUi(App::Get().StateJson());
        return;
    }
    if (type == rc::msg::kStartDesktop) return StartDesktop(m);
    if (type == rc::msg::kStopDesktop) return StopDesktop();
    if (type == "desktop.active") {
        const std::string active = m.value("monitorId", "");
        std::lock_guard lock(m_mutex);
        for (auto& [id, d] : m_desktop) d->SetActive(active.empty() || d->Monitor().id == active);
        return;
    }
    if (type == "desktop.keyframe") {
        std::lock_guard lock(m_mutex);
        for (auto& [id, d] : m_desktop) d->RequestKeyframe();
        if (m_vr) m_vr->RequestKeyframe();
        return;
    }
    if (type == rc::msg::kLaunchSteamVr) return LaunchSteamVr();
    if (type == "vr.restartSteamVR") {
        steamvr::Restart();
        m_steamVrLaunchNs = rc::NowNs();
        NotifyVrState("launching", "steamvr", "");
        return;
    }
    if (type == rc::msg::kStartVr) return StartVr();
    if (type == rc::msg::kStopVr) {
        // The headset left the VR view. The game keeps running, so don't end the stream.
        if (m_vr) m_vr->SetPaused(true);
        return;
    }
    if (type == rc::msg::kRecenter) {
        App::Get().Bridge().Recenter();
        return;
    }
    if (type == rc::msg::kVrPhase) {
        if (const int64_t slip = m.value("slipNs", int64_t(0)); slip > 0) App::Get().Bridge().SlipVsync(slip);
        else App::Get().Bridge().ShiftVsync(m.value("shiftNs", int64_t(0)));
        return;
    }
    if (type == rc::msg::kListGames) {
        auto games = App::Get().Games().Scan();
        json list = json::array();
        for (const auto& g : games)
            list.push_back({{"id", g.id}, {"name", g.name}, {"source", g.source}, {"hasImage", !g.image.empty()},
                            {"lastPlayed", g.lastPlayed}});
        SendControl({{"type", rc::msg::kGames}, {"games", list}});
        return;
    }
    if (type == "games.image") {
        const std::string id = m.value("id", "");
        auto bytes = App::Get().Games().Image(id);
        if (bytes.size() > 400 * 1024) bytes.clear();  // keep the control channel snappy
        SendControl({{"type", "games.image"}, {"id", id}, {"data", Base64Encode(bytes.data(), bytes.size())}});
        return;
    }
    if (type == rc::msg::kLaunchGame) {
        const std::string id = m.value("id", "");
        const bool ok = App::Get().Games().Launch(id);
        SendControl({{"type", rc::msg::kNotice}, {"level", ok ? "info" : "error"},
                     {"text", ok ? "Launching game…" : "Could not launch the game"}});
        if (ok && !(m_vr && m_vr->Running())) NotifyVrState("launching", "steamvr", "");  // see LaunchSteamVr
        return;
    }
    if (type == rc::msg::kStats) {
        std::lock_guard lock(m_mutex);
        m_clientStats = ObjectField(m, "stats");
        // ROUND19 T3: the decode-side autoscale ladder (default off). The headset's
        // decode average arrives here at ~1 Hz - the natural tick. A missing decodeMs
        // reads as no pressure (the ladder is conservative on absent signals), the
        // PC's own dropped-frame average and the handshake's decode-fps hint fill the
        // rest in. A step rebuilds the VR stream on the existing hot path, at most
        // once per cooldown - never the rebuild storm of 513f150. PLAN-AUTO-TUNE:
        // autoTune drives the same ladder while the user has not locked a manual
        // scale (encodeScalePct > 0 keeps it off - DECISION-TABLE §3.1).
        const Settings pcAuto = SettingsStore::Get().Snapshot();
        if (pcAuto.l7Autoscale || (pcAuto.autoTune && pcAuto.encodeScalePct == 0)) {
            rc::L7Autoscale::Config c;
            const double hintMs = m_caps.maxFpsAtMaxRes > 0 ? 1000.0 / m_caps.maxFpsAtMaxRes : 0.0;
            c.decodeBudgetMs = hintMs > 0 ? hintMs * 0.8 : 20.0;
            m_autoscale.Configure(c);
            const double decodeMs = NumberField(m_clientStats, "decodeMs", -1.0);
            const double dropped = m_vr ? double(m_vr->Stats().droppedFps) : -1.0;
            const auto r = m_autoscale.Update(decodeMs, dropped, rc::NowNs());
            if (r.changed)
                RC_LOGI("L7 autoscale: encode scale -> %d%% (decode %.1f ms vs budget %.1f ms, dropped %.1f fps)",
                        m_autoscale.Pct(), decodeMs, c.decodeBudgetMs, dropped);
            if (r.changed) {
                m_autoTune.NoteLadderStep(rc::NowNs());  // one variable per cooldown window
                OnStreamProfileChanged();
            }
        }
        // PLAN-AUTO-TUNE / A84b §1: the decision layer, called on every stats tick
        // whatever the switch says. The `if (pcAuto.autoTune)` that used to sit here
        // was a second place deriving the enable decision from the settings, and a
        // gate living in a call site no test binary links can never be covered;
        // RunAutoTune now feeds Settings::autoTune into rc::AutoTune (Config::enabled)
        // and the layer decides. The ladder gate above is a different question
        // (which scale is allowed to step - DECISION-TABLE §3.1), not this one.
        RunAutoTune();
        return;
    }
}

void ClientSession::ApplyClientInfo(const json& m) {
    std::lock_guard lock(m_mutex);
    // Everything here comes off the wire, and every read is type-checked (json_field.h). A field
    // of the wrong type used to throw type_error out of the handshake and out of the receive
    // thread, where an uncaught throw is std::terminate.
    if (const auto display = m.find("display"); display != m.end() && display->is_object()) {
        const json& d = *display;
        const uint32_t eyeW = UIntField(d, "eyeWidth", m_caps.recommendedEyeWidth);
        if (rc::PlausibleEyeSize(eyeW)) m_caps.recommendedEyeWidth = eyeW;
        const uint32_t eyeH = UIntField(d, "eyeHeight", m_caps.recommendedEyeHeight);
        if (rc::PlausibleEyeSize(eyeH)) m_caps.recommendedEyeHeight = eyeH;
        const float ipd = FloatField(d, "ipd", m_caps.ipd);
        if (rc::PlausibleIpd(ipd)) m_caps.ipd = ipd;
        if (const auto rates = d.find("refreshRates"); rates != d.end() && rates->is_array()) {
            std::vector<float> valid;
            for (const auto& r : *rates) {
                if (!r.is_number()) continue;
                const float hzF = float(r.get<double>());
                if (rc::PlausibleRefreshRate(hzF)) valid.push_back(hzF);
            }
            if (!valid.empty()) m_caps.refreshRates = std::move(valid);
        }
        // Two eyes, four tan halves each - anything else is not a projection we can use, and the
        // shape has to be checked before indexing into it.
        if (const auto fov = d.find("fov"); fov != d.end() && fov->is_array() && fov->size() == 2 &&
                                         (*fov)[0].is_array() && (*fov)[0].size() == 4 &&
                                         (*fov)[1].is_array() && (*fov)[1].size() == 4) {
            float t[2][4];
            for (int e = 0; e < 2; e++)
                for (int k = 0; k < 4; k++) t[e][k] = FloatAt((*fov)[e], size_t(k), m_caps.fovTan[e][k]);
            // A61: two in-range halves can still sum to zero (0,0 / -1,1); such a pair
            // collapses SteamVR's frustum and makes the fovea placement divide by zero.
            // A block we cannot use keeps the previous one whole.
            if (rc::PlausibleFovPair(t[0][0], t[0][1]) && rc::PlausibleFovPair(t[0][2], t[0][3]) &&
                rc::PlausibleFovPair(t[1][0], t[1][1]) && rc::PlausibleFovPair(t[1][2], t[1][3]))
                std::memcpy(m_caps.fovTan, t, sizeof(t));
        }
    }
    if (const auto decoders = m.find("decoders"); decoders != m.end() && decoders->is_object()) {
        const json& d = *decoders;
        m_caps.h264 = BoolField(d, "h264", true);
        m_caps.hevc = BoolField(d, "hevc", true);
        m_caps.hevc10 = BoolField(d, "hevc10", false);
        m_caps.av1 = BoolField(d, "av1", false);
        m_caps.av110 = BoolField(d, "av110", false);
        const uint32_t mw = UIntField(d, "maxWidth", m_caps.maxDecodeWidth);
        if (rc::PlausibleDecodeSize(mw)) m_caps.maxDecodeWidth = mw;
        const uint32_t mh = UIntField(d, "maxHeight", m_caps.maxDecodeHeight);
        if (rc::PlausibleDecodeSize(mh)) m_caps.maxDecodeHeight = mh;
        // ROUND18 L3: optional fps ceiling. 0/missing = old headset, no warning.
        // Reject malformed values via the shared validator (also unit-tested).
        m_caps.maxFpsAtMaxRes = rc::ValidateDecodeFpsHint(UIntField(d, "maxFpsAtMaxRes", 0u));
    }
    if (const auto info = m.find("settings"); info != m.end() && info->is_object()) {
        json s = *info;
        const json& revisions = ObjectField(m, "_pcRevisions");
        // A headset snapshot queued before a PC edit must not undo that edit later.
        for (const char* key : rc::synced::kAll) {
            const auto rev = revisions.find(key);
            // Compared as a double so an absurd revision cannot overflow a cast: any value at
            // least the PC's means "the headset's copy is newer", which is what the number says.
            const double theirs = rev != revisions.end() && rev->is_number() ? rev->get<double>() : 0.0;
            if (!(theirs >= double(m_pcKeyRevisions[key]))) s.erase(key);
        }
        if (const std::string codec = StrField(s, "codec"); !codec.empty()) m_settings.codec = codec;
        m_settings.tenBit = BoolField(s, "tenBit", m_settings.tenBit);
        if (const std::string quality = StrField(s, "vrQuality"); !quality.empty()) m_settings.vrQuality = quality;
        m_settings.vrScale = FloatField(s, "vrScale", m_settings.vrScale);
        m_settings.matchSteamVr = BoolField(s, "matchSteamVr", m_settings.matchSteamVr);
        m_settings.gazeFoveation = BoolField(s, "gazeFoveation", m_settings.gazeFoveation);
        // headsets before 1.2.1 only foveate tracked
        m_settings.eyesTracked = BoolField(s, "eyesTracked", m_settings.eyesTracked);
        m_settings.frameGeneration = BoolField(s, "frameGeneration", m_settings.frameGeneration);
        const float hz = FloatField(s, "refreshRate", m_settings.refreshRate);
        if (rc::PlausibleRefreshRate(hz)) m_settings.refreshRate = hz;
        const int vrMbps = IntField(s, "vrBitrateMbps", m_settings.vrBitrateMbps);
        m_settings.vrBitrateMbps = vrMbps ? std::clamp(vrMbps, 10, 500) : 0;
        const int desktopMbps = IntField(s, "desktopBitrateMbps", m_settings.desktopBitrateMbps);
        m_settings.desktopBitrateMbps = desktopMbps ? std::clamp(desktopMbps, 5, 200) : 0;
        m_settings.desktopFps = IntField(s, "desktopFps", m_settings.desktopFps);
        m_settings.adaptiveBitrate = BoolField(s, "adaptiveBitrate", m_settings.adaptiveBitrate);
        m_settings.streamProfile = std::clamp(IntField(s, "streamProfile", m_settings.streamProfile), 0, 2);
        m_settings.debugEncodeScalePct =
            std::clamp(IntField(s, "debugEncodeScalePct", m_settings.debugEncodeScalePct), 0, 100);  // ROUND20 L17
        // The preference is a PC-side setting for now (rcs::Settings::streamProfile).
        // The headset will send its own choice once its three-way UI exists.
        m_settings.streamAudio = BoolField(s, "streamAudio", m_settings.streamAudio);
        m_settings.micPassthrough = BoolField(s, "micPassthrough", m_settings.micPassthrough);
        // 头显改了某个可同步项（值与 PC 推过去的不一样）→ 这一项的所有权归它，PC 不再推。
        for (const char* key : rc::synced::kAll) {
            if (!s.contains(key)) continue;
            auto it = m_ownedSettings.find(key);
            if (it == m_ownedSettings.end()) continue;
            if (it->second != s[key]) {
                m_ownedSettings.erase(it);
                // A19 audit #5: an ownership move used to be silent on both ends - say
                // so on the existing notice channel instead. The same key stays quiet
                // for 10 s: one settings batch can move several keys, and repeated
                // edits must not spam the headset.
                const int64_t now = rc::NowNs();
                if (now - m_ownershipNoticeNs[key] > 10'000'000'000LL) {
                    m_ownershipNoticeNs[key] = now;
                    SendControl({{"type", rc::msg::kNotice},
                                 {"level", "info"},
                                 {"text", SyncedKeyTitle(key) + " is now controlled by the headset."}});
                }
            }
        }
    }
}

rc::VideoCodec ClientSession::ChooseCodec(bool forVr) const {
    // The pure decision lives in stream_config.h; this method only gathers the
    // live inputs (TASK-COMBO-SWEEP-2026-11-21).
    std::string pref = m_settings.codec;
    if (pref == "auto") pref = SettingsStore::Get().Snapshot().codec;
    (void)forVr;
    return ChooseStreamCodec(App::Get().Caps(), m_caps, m_settings.tenBit, pref);
}

// ---------------------------------------------------------------------------

void ClientSession::MeasurePoseWait(int64_t receivedNs) {
    int64_t wait = 0, period = 0;
    if (!App::Get().Bridge().PoseWaitNs(receivedNs, wait, period)) {
        m_poseWaits.clear();
        return;
    }
    if (period != m_poseWaitPeriodNs) m_poseWaits.clear();
    m_poseWaitPeriodNs = period;
    m_poseWaits.push_back(wait);
    if (receivedNs - m_poseWaitSentNs < 1'000'000'000LL) return;
    m_poseWaitSentNs = receivedNs;
    if (m_poseWaits.size() < 30) {
        m_poseWaits.clear();
        return;
    }
    // Averaged as angles on the period's circle: a pose that just missed a pickup waits almost
    // a whole period, and is the same story as one that just made it.
    const double p = double(period), k = 6.283185307 / p;
    double sinSum = 0, cosSum = 0;
    for (int64_t w : m_poseWaits) {
        sinSum += std::sin(double(w) * k);
        cosSum += std::cos(double(w) * k);
    }
    double mean = std::atan2(sinSum, cosSum) / k;
    if (mean < 0) mean += p;
    // Each one against that, within half a period either way; the 2% that waited least.
    std::vector<double> waited;
    waited.reserve(m_poseWaits.size());
    for (int64_t w : m_poseWaits) waited.push_back(mean + std::fmod(double(w) - mean + 1.5 * p, p) - p / 2);
    m_poseWaits.clear();
    const size_t low = waited.size() / 50;
    std::nth_element(waited.begin(), waited.begin() + low, waited.end());
    SendControl({{"type", rc::msg::kVrPoseWait},
                 {"waitNs", int64_t(mean)},
                 {"lowNs", int64_t(waited[low])},
                 {"periodNs", period}});
}

void ClientSession::SendServerInfo() {
    // Read under the lock: NotifyVrState rewrites these strings from the VR stream thread.
    std::string vrState;
    {
        std::lock_guard lock(m_mutex);
        vrState = m_vrState;
    }
    const Settings s = SettingsStore::Get().Snapshot();
    const EncoderCaps& caps = App::Get().Caps();
    const auto route = audio::ResolveRoute(s.audioDevices);
    const auto svr = steamvr::SteamVrPath();
    json info{
        {"type", rc::msg::kServerInfo},
        {"pcName", s.pcName},
        {"serverId", s.serverId},
        {"version", App::Get().Version()},
        {"os", WindowsVersion()},
        {"gpu", App::Get().Gpu().name},
        {"vrBitrateMbps", s.vrBitrateMbps},
        {"vrPhaseSlip", true},  // "slipNs" in vr.phase is understood
        {"encoder",
         {{"backend", caps.backend}, {"h264", caps.h264}, {"hevc", caps.hevc}, {"av1", caps.av1},
          {"hevc10", caps.hevc10}, {"av110", caps.av110}}},
        {"steamvr",
         {{"installed", svr.has_value()}, {"running", steamvr::IsRunning()},
          {"driverRegistered", steamvr::IsDriverRegistered()}}},
        {"openxr", {{"rincastActive", openxr::IsRinCastActive()}}},
        {"audioDriver", {{"speaker", route.speaker != nullptr}, {"microphone", route.micFeed != nullptr},
                         {"devices", route.micSet.empty() ? route.speakerSet : route.micSet}}},
        {"account", s.cloudUserName},
        {"remote", m_remote},
        {"vrState", vrState},
        // 可同步的共享设置：只推 PC 仍然拥有的那些项（头显改过的项所有权已收回）。
        // 码率不算：头显已有「跟随 PC」的 0 值语义，双向覆盖会破坏它。
        {"settings", OwnedSettingsJson()},
    };
    // A19 audit #10: an optional additive short code for "what's wrong on this PC"
    // (see DiagnosisCode). A json key, not a protocol field - protocol.h is untouched
    // and old headsets ignore keys they don't know. Absent when nothing is wrong.
    if (const std::string diagnosis = DiagnosisCode(); !diagnosis.empty()) info["diagnosis"] = diagnosis;
    SendControl(info);
}

// The synced settings the PC still owns, as a json object for server.info.
json ClientSession::OwnedSettingsJson() {
    std::lock_guard lock(m_mutex);
    json out = json::object();
    for (const auto& [k, v] : m_ownedSettings) out[k] = v;
    return out;
}

void ClientSession::OwnSyncedSettings(const json& changed, const std::function<void()>& save) {
    std::lock_guard lock(m_mutex);
    save();
    const Settings s = SettingsStore::Get().Snapshot();
    const json values = s.ToJson(true);
    ++m_pcRevision;
    for (const char* key : rc::synced::kAll) {
        if (!changed.contains(key)) continue;
        m_ownedSettings[key] = values[key];
        m_pcKeyRevisions[key] = m_pcRevision;
    }
    if (changed.contains(rc::synced::kCodec)) m_settings.codec = s.codec;
    if (changed.contains(rc::synced::kTenBit)) m_settings.tenBit = s.tenBit;
    if (changed.contains(rc::synced::kAdaptiveBitrate)) m_settings.adaptiveBitrate = s.adaptiveBitrate;
    if (changed.contains(rc::synced::kDesktopFps)) m_settings.desktopFps = s.desktopFps;
    // A86: kStreamProfile is deliberately NOT copied into m_settings. The key still
    // exists on the wire and a headset that has the old slider will still send it,
    // but the tier belongs to the tuner now - reading it here is what used to hand
    // the slider a way back in (AdoptHeadsetOwnedSettings then persisted it).
}

void ClientSession::SendMonitors() {
    auto monitors = EnumerateMonitors();
    json list = json::array();
    for (const auto& mon : monitors)
        list.push_back({{"id", mon.id},
                        {"name", mon.name},
                        {"index", mon.index},
                        {"width", mon.width},
                        {"height", mon.height},
                        {"primary", mon.primary},
                        {"refreshHz", mon.refreshHz}});
    {
        std::lock_guard lock(m_mutex);
        m_monitors = monitors;
    }
    SendControl({{"type", rc::msg::kMonitors}, {"monitors", list}});
}

void ClientSession::OnMonitorsChanged() {
    Enqueue(json{{"type", "_monitors"}});
}

void ClientSession::OnSettingsChanged(const json& changed) {
    // PLAN-AUTO-TUNE: a manual write of a dimension auto owns locks that
    // dimension until autoTune is turned off and back on (DECISION-TABLE §3.1).
    // Enabling autoTune itself is the hand-over, so it clears last.
    // A86 (user, 2026-11-23): streamProfile is no longer a settings key at all - the
    // tier is the tuner's, so there is no floor write to announce and no lock to clear
    // on that dimension. Cbr and Fovea keep the pre-A86 rule untouched.
    bool rebuild = false;
    {
        std::lock_guard lock(m_mutex);
        if (changed.contains("qualityProfileCbr")) m_autoTune.MarkManual(rc::AutoDim::Cbr);
        if (changed.contains("foveatedEncoding")) m_autoTune.MarkManual(rc::AutoDim::Fovea);
        if (changed.contains("autoTune")) {
            const bool on = changed.value("autoTune", false);
            if (on) m_autoTune.ClearLocks();  // the hand-over: every dimension is auto's again
            // A86: neither UI has this switch any more, so this branch is the
            // settings-file escape hatch (remote support). Off pins the stream at
            // the start rung - EffectiveProfileRung's !autoTuneOn branch - and on
            // makes the tuner choose again from scratch.
            m_autoProfileEff = -1;
            rebuild = !on;
        }
    }
    // app.cpp's rebuild list has no autoTune in it, so the switch that stops auto
    // moving the picture has to rebuild the picture itself.
    if (rebuild) OnStreamProfileChanged();
    if (changed.contains("desktopInput")) ApplyInputSetting(true);
    if (changed.contains("streamAudio") || changed.contains("micPassthrough") ||
        changed.contains("audioDevices") || changed.contains("playOnPcToo") || changed.contains("switchDefaultAudio"))
        Enqueue(json{{"type", "_audio"}});
    Enqueue(json{{"type", "_serverInfo"}});
}

void ClientSession::OnStreamProfileChanged() {
    Enqueue(json{{"type", "_profile"}});
}

void ClientSession::ResetAutoscale() {
    {
        std::lock_guard lock(m_mutex);
        m_autoscale.Reset();
        m_autoTune.ResetEpisodes();  // a manual reset starts a fresh episode for auto as well
    }
    RC_LOGI("L7 autoscale: reset to 100% by the user");
    OnStreamProfileChanged();
}

// ---------------------------------------------------------------------------
// PLAN-AUTO-TUNE (P1 + P3). LogAutoCapability: one read-only line per session
// summarising the device's own capability answers and the PC settings auto may
// move. RunAutoTune: called from the kStats handler with m_mutex held, only
// when Settings::autoTune is on - it classifies the tick, logs the signals
// line on a 30 s cadence, and applies at most one action: a settings write plus
// OnStreamProfileChanged for CBR/foveation, the session's own effective profile
// for the picture rung (A84 - auto never writes the slider).
// Design authority: AUTO-TUNE-2026-11-19/DECISION-TABLE.md.
// ---------------------------------------------------------------------------

// A86b: the user-facing slider and its floor are gone. What stays here is the one
// piece of session state the pure rc::EffectiveProfileRung cannot hold: which rung
// auto last chose. -1 = not chosen yet, which the pure rule turns into 1 (balanced).
// Caller rule: the Locked variant runs with m_mutex held.
int ClientSession::EffectiveProfileLocked() {
    if (const int forced = ForcedRung(); forced >= 0) return m_autoProfileEff = forced;
    m_autoProfileEff = rc::EffectiveProfileRung(m_autoProfileEff);
    return m_autoProfileEff;
}

int ClientSession::EffectiveProfile() {
    std::lock_guard lock(m_mutex);
    return EffectiveProfileLocked();
}

Settings ClientSession::EffectiveSettings(const Settings& pc) {
    Settings eff = pc;
    eff.streamProfile = EffectiveProfile();
    return eff;
}

// A84 §2's clean-side evidence, from the same two readings A81 put in the log:
// the brake line the CURRENT rung pays and the device's own idle queue. This is
// queue_guard.h's `dipAt` formula READ (never copied state, never the guard's
// numbers touched) with the floor term at its no-feedback value: the real line
// only ever sits higher (max with floor + floorExtraMs), so demanding the queue
// be under this one is the strict side of the pair - exactly what video_sender.cpp
// prints when the guard has not seen a report yet.
static double ProfileDipAtMs(int profileId) {
    const rc::StreamProfileParams p = rc::ProfileParams(rc::ProfileFromId(profileId));
    return std::max(p.targetQueueMs + p.bandMs, p.floorExtraMs);
}

void ClientSession::LogAutoCapability() {
    ClientCaps caps;
    {
        std::lock_guard lock(m_mutex);
        caps = m_caps;
    }
    const Settings pc = SettingsStore::Get().Snapshot();
    std::string rates;
    for (float r : caps.refreshRates) {
        if (!rates.empty()) rates += '/';
        rates += std::to_string(int(r + 0.5f));
    }
    // A84 §6: what the slider means now (a floor) and what the streams run at. The
    // pair is printed together because they differ during exactly the sessions that
    // matter: a latency-first slider (streamProfile=0) on a link with room. Appended
    // at the END of this line - logpair.py / analyze_dual.py anchor on the fields
    // before it, and the parser regexes here are not end-anchored.
    const int effProfile = EffectiveProfile();
    RC_LOGI("AUTO: capability device=%s serial=%s decoders=h264:%d hevc:%d hevc10:%d av1:%d av110:%d "
            "maxDecode=%ux%u maxFpsAtMaxRes=%u refresh=[%s] eye=%ux%u | autoTune=%d encodeScalePct=%d "
            "l7Autoscale=%d streamProfile=%d qualityProfileCbr=%d profileControlsVideo=%d foveatedEncoding=%d "
            "vrBitrateMbps=%d pcBitrateWins=%d adaptiveBitrate=%d codec=%s tenBit=%d effProfile=%d",
            m_hello.model.c_str(), m_hello.deviceId.c_str(), caps.h264, caps.hevc, caps.hevc10, caps.av1,
            caps.av110, caps.maxDecodeWidth, caps.maxDecodeHeight, caps.maxFpsAtMaxRes, rates.c_str(),
            caps.recommendedEyeWidth, caps.recommendedEyeHeight, int(pc.autoTune), pc.encodeScalePct,
            int(pc.l7Autoscale), pc.streamProfile, int(pc.qualityProfileCbr), 0,  // A86b: profileControlsVideo removed
            int(pc.foveatedEncoding), pc.vrBitrateMbps, int(pc.pcBitrateWins), int(pc.adaptiveBitrate),
            pc.codec.c_str(), int(pc.tenBit), effProfile);
}

void ClientSession::RunAutoTune() {
    const Settings pc = SettingsStore::Get().Snapshot();
    // A84b §1: this used to be `if (!pc.autoTune) return;` - the enable decision
    // made here, where no test can reach it. The decision is fed INTO the layer
    // (rc::AutoTune::Config::enabled) and enforced in rc::AutoTune::Update; what is
    // left below reads the same stored flag back, so the layer is still the one
    // place that decides, and this call site only skips gathering signals and
    // printing the 30 s line for an install that opted out (DECISION-TABLE §4).
    m_autoTune.SetEnabled(pc.autoTune);
    if (!m_autoTune.Enabled()) return;
    const ClientStreamSettings st = m_settings;
    const auto vs = m_vr ? m_vr->Stats() : VideoSenderStats{};

    // The effective rate StartVr built the stream with (its own validation of
    // the headset's refresh choice).
    float nominal = st.refreshRate;
    if (!m_caps.refreshRates.empty() && std::find(m_caps.refreshRates.begin(), m_caps.refreshRates.end(), nominal) ==
                                            m_caps.refreshRates.end())
        nominal = m_caps.refreshRates.back();
    // A82-M2: the clamp fired exactly when the fallback rewrote the request - back() is
    // itself in the list, so an unclamped nominal always equals st.refreshRate. The WARN
    // rides the existing 30 s signals gate below because RunAutoTune runs on every stats
    // tick; unthrottled it would spam. Fallback value untouched (stage F call, A82 §3 F3).
    const bool nominalClamped = nominal != st.refreshRate;

    rc::BottleneckSignal sig;
    sig.decodeMs = NumberField(m_clientStats, "decodeMs", -1.0);
    const double hintMs = m_caps.maxFpsAtMaxRes > 0 ? 1000.0 / m_caps.maxFpsAtMaxRes : 0.0;
    sig.decodeBudgetMs = hintMs > 0 ? hintMs * 0.8 : 20.0;  // the ladder's own budget formula
    sig.droppedFps = m_vr ? double(vs.droppedFps) : -1.0;
    sig.gameFps = double(vs.gameFps);
    sig.nominalFps = double(nominal);
    sig.qp = double(vs.qp);
    sig.mbps = vs.mbps;
    sig.ceilingKbps = vs.bitrateKbps;
    sig.queueMs = vs.queueMs;
    sig.loss = vs.lossAvg;

    // A84 §2: the tuner is fed the RUNG the live stream is built at (the only thing
    // it may move), the slider as the FLOOR, and the brake line that rung pays.
    // A84b §3: BuildAutoTuneInput no longer takes the key-ownership flag - the
    // floor above carries the intent it guarded, see stream_config.h.
    Settings eff = pc;
    eff.streamProfile = EffectiveProfileLocked();
    rc::AutoTuneInput in = BuildAutoTuneInput(eff, st, m_slowPath);
    in.dipAtMs = ProfileDipAtMs(eff.streamProfile);

    // A88: a paused/absent stream reports qp=-1, wire=0, queue=-1, gameFps=0. That
    // is no evidence, not a clean link. Skipping here - rather than adding an
    // Unknown member to Bottleneck - keeps BottleneckName(), the log anchors and
    // every switch untouched, and stops Update() from resetting the climb streak
    // while the headset is showing nothing.
    if (!rc::HasUsableSignals(sig)) return;
    const int64_t now = rc::NowNs();
    const auto r = m_autoTune.Update(sig, in, now);

    // P1: the 30 s signals line (DECISION-TABLE §4) - autoTune only, so an
    // opted-out install sees nothing new beyond the one capability line.
    if (!m_autoSignalLogNs || now - m_autoSignalLogNs >= 30'000'000'000LL) {
        m_autoSignalLogNs = now;
        // A84b §4 (TASK-A84b): the profile pair rides this line because it is the
        // only PERIODIC line this file owns, and the whole point of A84 was to make
        // the next field round attributable. The capability line that used to carry
        // effProfile is printed once per session, and a reader who sees 1407 guard
        // dips cannot tell from it what the streams were built at four minutes later.
        // effProfile is the rung the streams ARE built at (the same number
        // StartVr/StartDesktop feed the encoder, the same word the capability line
        // uses, and the same thing the 3 s `Stream N latency: profile=` field
        // already names - it is built from EffectiveSettings now); profileFloor is
        // the slider the user set, which is the number you need beside it to tell an
        // auto move from a user move. Appended at the END: the existing field names
        // and order are byte-for-byte what the log readers anchor on.
        RC_LOGI("AUTO: signals %s fps=%.0f game=%.0f qp=%.1f wire=%.1f ceiling=%u queue=%.0f loss=%.1f%% "
                "decode=%.1f/%.1f ms dropped=%.1f effProfile=%d profileFloor=%d",
                rc::BottleneckName(r.cls), vs.fps, vs.gameFps, vs.qp, vs.mbps, vs.bitrateKbps, vs.queueMs,
                vs.lossAvg * 100.0, sig.decodeMs, sig.decodeBudgetMs, vs.droppedFps, eff.streamProfile,
                kStartRungForLog);
        if (nominalClamped)
            RC_LOGW("Refresh clamp (autoTune mirror of the StartVr stream-build clamp): %.0f Hz requested, not in "
                    "the device's enumerated list [%s] - nominal %.0f Hz (the top entry)",
                    st.refreshRate, A82RateList(m_caps.refreshRates).c_str(), nominal);
    }

    if (r.action == rc::AutoAction::None) return;
    if (r.action == rc::AutoAction::LogOnly) {
        RC_LOGI("AUTO: %s -> %s", rc::BottleneckName(r.cls), r.reason);
        return;
    }
    const char* dimName = r.action == rc::AutoAction::CbrOff    ? "qualityProfileCbr=false"
                          : r.action == rc::AutoAction::FoveaOn ? "foveatedEncoding=true"
                          : r.action == rc::AutoAction::ProfileUp ? "streamProfile up"
                                                                : "streamProfile down";
    RC_LOGI("AUTO: %s (qp %.1f, wire %.1f of %u kbps, queue %.0f ms, loss %.1f%%) -> %s (%s)",
            rc::BottleneckName(r.cls), sig.qp, sig.mbps, sig.ceilingKbps, sig.queueMs, sig.loss * 100.0, dimName,
            r.reason);
    bool needRebuild = true;
    switch (r.action) {
        case rc::AutoAction::CbrOff:
            SettingsStore::Get().Update([](Settings& s) { s.qualityProfileCbr = false; });
            break;
        case rc::AutoAction::FoveaOn:
            SettingsStore::Get().Update([](Settings& s) { s.foveatedEncoding = true; });
            break;
        // A84: the picture rung is NOT a settings write any more. Moving the synced
        // key would move the user's floor with it - which is how the slider became a
        // ceiling nobody could climb back over. It is session state.
        // A87 (user, 2026-11-23): it used to fall through to the rebuild below. That
        // rebuilt StopVr()+StartVr(), and the 10:17 log shows the cost: 46 s with no
        // VR frame because SteamVR had to reopen the shared textures the game waits
        // on. A tier move only changes the QP floor and the queue-brake line, both
        // of which the live stream can take in place (see VrStream::ApplyProfileRung).
        case rc::AutoAction::ProfileDown:
        case rc::AutoAction::ProfileUp: {
            const int from = eff.streamProfile;
            m_autoProfileEff = rc::ClampProfileId(r.profileId);
            // A84 §6: the transition gets its own line, so the one above keeps every
            // field name and order the log readers anchor on. Old -> new, the
            // direction, how long the evidence behind it had been continuous, the
            // floor it is allowed to sit under only until the link calms, and the
            // two A81 readings that justified it.
            RC_LOGI("AUTO: profile %d -> %d %s (floor %d, %s held %.0f s, dipAt %.0f ms, queue %.0f ms, "
                    "decode %.1f ms, classifier %s)",
                    from, m_autoProfileEff, r.action == rc::AutoAction::ProfileUp ? "up" : "down",
                    kStartRungForLog, r.action == rc::AutoAction::ProfileUp ? "headroom" : "pressure",
                    double(r.evidenceNs) / 1e9, in.dipAtMs, sig.queueMs, sig.decodeMs,
                    rc::BottleneckName(r.cls));
            // m_mutex is held by RunAutoTune, so m_vr is safe to touch here. The
            // QP floor is handed to the encoder thread; only the brake is set now.
            if (m_vr) m_vr->ApplyProfileRung(m_autoProfileEff);
            RC_LOGI("AUTO: tier applied in place - no stream rebuild, no black frame");
            needRebuild = false;
            break;
        }
        default:
            return;
    }
    // CBR/foveation are settings writes: the user's own channel takes this same
    // rebuild path (App::ApplySettingSideEffects -> OnStreamProfileChanged).
    if (needRebuild) OnStreamProfileChanged();
}

void ClientSession::RestartVideoForProfile(bool vr, bool desktop) {
    const bool hadVr = vr && bool(m_vr);
    if (hadVr) RestartVr();
    json monitors = json::array();
    if (desktop) {
        std::lock_guard lock(m_mutex);
        for (auto& [id, d] : m_desktop) monitors.push_back(d->Monitor().id);
    }
    if (!monitors.empty()) StartDesktop(json{{"monitors", monitors}});
    RC_LOGI("Settings batch: rebuilt vr=%d desktop=%zu", int(hadVr), monitors.size());
}

void ClientSession::StartDesktop(const json& m) {
    {
        std::lock_guard lock(m_queueMutex);
        m_restartDesktop = false;
        if (!m_restartVr) m_restartDueNs = 0;
    }
    const Settings pc = SettingsStore::Get().Snapshot();
    std::vector<MonitorInfo> monitors = EnumerateMonitors();
    std::vector<std::string> wanted;
    if (m.contains("monitors") && m["monitors"].is_array())
        for (const auto& id : m["monitors"])
            if (id.is_string()) wanted.push_back(id.get<std::string>());
    StopDesktop();

    ClientCaps caps;
    ClientStreamSettings st;
    rc::VideoCodec codec;
    {
        std::lock_guard lock(m_mutex);
        m_monitors = monitors;
        caps = m_caps;
        st = m_settings;
        codec = ChooseCodec(false);
    }
    std::vector<const MonitorInfo*> selected;
    for (const auto& mon : monitors)
        if (wanted.empty() || std::find(wanted.begin(), wanted.end(), mon.id) != wanted.end()) selected.push_back(&mon);
    if (selected.empty()) return;

    double totalPixels = 0;
    for (auto* mon : selected) totalPixels += double(mon->width) * mon->height;

    // A84: the saved streamProfile is the FLOOR, the stream is built at the effective
    // rung auto may have raised or lowered. Everything else in the snapshot is copied
    // verbatim, and nothing is written back to Settings (the floor is the only value
    // the persisted key, the UI slider and the headset ever see).
    const Settings eff = EffectiveSettings(pc);
    json streams = json::array();
    std::map<uint8_t, std::unique_ptr<DesktopStream>> created;
    for (auto* mon : selected) {
        // The settings -> config synthesis is a pure function now (stream_config.h,
        // TASK-COMBO-SWEEP-2026-11-21); the fps / bitrate / decode-limit fill moved
        // there VERBATIM. Monitor selection, stream ids and creation stay here.
        const DesktopStreamConfig cfg = BuildDesktopStreamConfig(eff, st, codec, m_slowPath, m_vr != nullptr, *mon,
                                                                 totalPixels, int(selected.size()), caps);
        const uint8_t streamId = uint8_t(rc::kFirstMonitorStreamId + mon->index);
        if (streamId >= rc::kMaxStreams) continue;
        auto ds = std::make_unique<DesktopStream>(m_connection.get(), streamId, *mon, cfg);
        streams.push_back(ds->Describe());
        created[streamId] = std::move(ds);
    }
    SendControl({{"type", rc::msg::kStreamConfig}, {"kind", "desktop"}, {"streams", streams}});
    if (!pc.desktopInput)
        SendControl({{"type", rc::msg::kNotice},
                     {"level", "info"},
                     {"text", "This PC has the headset's mouse and keyboard turned off. You can still watch."}});
    for (auto& [id, ds] : created) ds->Start();
    std::lock_guard lock(m_mutex);
    m_desktop = std::move(created);
}

void ClientSession::StopDesktop() {
    std::map<uint8_t, std::unique_ptr<DesktopStream>> old;
    {
        std::lock_guard lock(m_mutex);
        old.swap(m_desktop);
    }
    // Said, so the log shows whether a monitor's encoder was still running next to VR's.
    if (!old.empty()) RC_LOGI("Desktop: stopped %zu monitor stream(s)", old.size());
    old.clear();
}

void ClientSession::StartVr() {
    if (m_vr) {
        m_vr->SetPaused(false);
        return;
    }
    {
        std::lock_guard lock(m_queueMutex);
        m_restartVr = false;
        if (!m_restartDesktop) m_restartDueNs = 0;
    }
    const Settings pc = SettingsStore::Get().Snapshot();
    ClientCaps caps;
    ClientStreamSettings st;
    rc::VideoCodec codec;
    {
        std::lock_guard lock(m_mutex);
        caps = m_caps;
        st = m_settings;
        codec = ChooseCodec(true);
    }
    // Per-eye resolution from the headset's setting, limited by codec/decoder.
    // Internet uploads rarely carry the higher ones: their pixels would only add blur.
    const float picked = st.vrScale > 0 ? std::clamp(st.vrScale, 0.3f, float(kMaxVrScale)) : QualityScale(st.vrQuality);
    const float scale = m_slowPath ? std::min(picked, QualityScale("medium")) : picked;
    double eyeW = caps.recommendedEyeWidth * scale;
    double eyeH = caps.recommendedEyeHeight * scale;
    // Matching SteamVR's resolution: SteamVR is told the headset's own size, applies its
    // resolution setting to it, and the stream takes whatever it then renders at.
    auto& bridge = App::Get().Bridge();
    const bool match = st.matchSteamVr && !m_slowPath;
    uint32_t renderW = 0, renderH = 0, seenW = 0, seenH = 0;
    if (match) {
        renderW = Align(caps.recommendedEyeWidth, 32);
        renderH = Align(caps.recommendedEyeHeight, 32);
        if (bridge.SteamVrEyeSize(seenW, seenH)) {
            eyeW = seenW;
            eyeH = seenH;
        } else {
            eyeW = renderW;  // until SteamVR's first frame
            eyeH = renderH;
        }
        const double most = std::min(kMaxVrScale * caps.recommendedEyeWidth / eyeW, kMaxVrScale * caps.recommendedEyeHeight / eyeH);
        if (most < 1.0) {
            eyeW *= most;
            eyeH *= most;
        }
    }
    m_vrMatchedWidth = seenW;
    m_vrMatchedHeight = seenH;
    m_vrSeenWidth = m_vrSeenHeight = 0;
    const double maxTotalW = std::min<double>(caps.maxDecodeWidth, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const double maxH = std::min<double>(caps.maxDecodeHeight, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const double limit = std::min(maxTotalW / (eyeW * 2), maxH / eyeH);
    if (limit < 1.0) {
        eyeW *= limit;
        eyeH *= limit;
    }
    rcvr::DisplayConfig dc{};
    dc.eyeWidth = Align(uint32_t(eyeW), 32);
    dc.eyeHeight = Align(uint32_t(eyeH), 32);
    if (dc.eyeWidth * 2 > maxTotalW) dc.eyeWidth -= 32;
    if (dc.eyeHeight > maxH) dc.eyeHeight -= 32;
    float rate = st.refreshRate;
    if (!caps.refreshRates.empty() &&
        std::find(caps.refreshRates.begin(), caps.refreshRates.end(), rate) == caps.refreshRates.end())
        rate = caps.refreshRates.back();
    // A82-M2: this fallback was silent - "why is this session encoding at 120 Hz" was
    // undiagnosable from the log (TASK-A82 §1: on a Quest 2 list [60/72/80/90/120] a
    // request of e.g. 85 lands on back() = 120). Readings only: the clamp target
    // (back() = highest, not nearest) is a stage-F judgment (§3 F3) and stays untouched.
    // Fired exactly when rate != st.refreshRate - back() is itself in the list.
    if (rate != st.refreshRate)
        RC_LOGW("Refresh clamp: %.0f Hz requested, not in the device's enumerated list [%s] - the stream "
                "builds at %.0f Hz (the top entry)",
                st.refreshRate, A82RateList(caps.refreshRates).c_str(), rate);
    dc.refreshRate = rate;
    // ROUND18 L3: warn when the requested rate is above the headset's measured
    // decode ceiling at large sizes. Informational only - we deliberately do NOT
    // change the rate or the render size here (that needs a real-device A/B and the
    // user's call). The hint surfaces in this one warning at stream start (and is
    // kept in the caps snapshot for later rebuilds); it is NOT part of the 1 s
    // telemetry - the "rc diag 1s" line carries rate-control stats only.
    if (caps.maxFpsAtMaxRes && rate > float(caps.maxFpsAtMaxRes)) {
        RC_LOGW("Headset decode hint: this device managed about %u fps at large sizes, "
                "stream requests %.0f fps - consider a lower resolution or frame rate",
                caps.maxFpsAtMaxRes, rate);
    }
    // VD-OFFLINE (TASK 2026-11-20 §2.4): an advice line next to the hint - what
    // the ladder would suggest as an encode rate for this device. Advice only:
    // nothing here changes the rate (same deliberate no-op as the warning above).
    if (const float suggest = rc::SuggestStartFps(rate, int(caps.maxFpsAtMaxRes)))
        RC_LOGI("Link policy: device reports about %u fps at large sizes - suggest encoding at %.0f fps instead of %.0f (advice only, not applied this build)",
                caps.maxFpsAtMaxRes, suggest, rate);
    dc.ipd = caps.ipd;
    std::memcpy(dc.fovTan, caps.fovTan, sizeof(dc.fovTan));
    strncpy_s(dc.headsetModel, m_hello.model.empty() ? "Quest" : m_hello.model.c_str(), _TRUNCATE);
    strncpy_s(dc.headsetSerial, m_hello.deviceId.c_str(), _TRUNCATE);
    dc.renderEyeWidth = renderW;
    dc.renderEyeHeight = renderH;
    // A82-M1: the effective nominal rate joins the resolution line, appended at the END
    // (this line's parser, analyze_dual.py RE_RES, is not end-anchored - checked 2026-11-23).
    // Provenance so the three values need no inference: what the headset requested
    // (st.refreshRate), whether the enumerated list clamped it, and what the stream is
    // actually built at (dc.refreshRate). The AUTO: capability refresh=[...] is the device's
    // SUPPORT LIST and reads exactly like an effective value - that misreading produced the
    // wrong-memory episode in TASK-A82 §1. Readings only.
    RC_LOGI("VR stream: %ux%u per eye%s, %.0f Hz requested, effective %.0f Hz (%s)", dc.eyeWidth, dc.eyeHeight,
            !match ? "" : seenW ? (", SteamVR renders " + std::to_string(seenW) + "x" + std::to_string(seenH)).c_str()
                                : ", following SteamVR's resolution once it renders",
            st.refreshRate, dc.refreshRate,
            rate != st.refreshRate ? "clamped: the request is not in the device's list"
                                   : "as requested, present in the device's list");
    // A80-M1: the same shapes at the announce point, before the bridge has anything to
    // follow. Readings only - dc is already fully built above and is not touched here.
    {
        const std::string fov0 = A80FovShape(dc.fovTan[0]);
        const std::string fov1 = A80FovShape(dc.fovTan[1]);
        RC_LOGI("A80-M1 geometry: announced %ux%u per eye aspect %.9g | fovTan e0 [%s] | fovTan e1 [%s]"
                " | ipd %.9g model '%s' serial '%s' matchSteamVr %d renderEye %ux%u",
                dc.eyeWidth, dc.eyeHeight, double(dc.eyeWidth) / double(dc.eyeHeight), fov0.c_str(), fov1.c_str(),
                dc.ipd, dc.headsetModel, dc.headsetSerial, match ? 1 : 0, dc.renderEyeWidth, dc.renderEyeHeight);
    }

    const bool needsRestart = bridge.Configure(dc);
    bridge.SetHeadsetConnected(true);

    int autoscalePct = 100;
    if (pc.l7Autoscale) {
        std::lock_guard lock(m_mutex);
        autoscalePct = m_autoscale.Pct();
    }
    // The settings -> config synthesis is a pure function now (stream_config.h,
    // TASK-COMBO-SWEEP-2026-11-21); this method keeps the bridge, the logs and the
    // wire messages. The block below was moved there VERBATIM. The guarded wrapper
    // (TASK-CAPS-GUARD) moves an H264 stream whose full frame exceeds NVENC's 4096 px
    // width onto HEVC instead of an encoder that could never open; the session owns
    // the log line, the header stays log-free.
    // A84: built from the effective-rung snapshot (see StartDesktop) - the floor stays
    // in pc for every other reader of the settings.
    const Settings eff = EffectiveSettings(pc);
    const VrStreamBuild vrBuild = BuildVrStreamConfigGuarded(eff, st, codec, m_slowPath, match, autoscalePct,
                                                            dc.eyeWidth, dc.eyeHeight, rate, App::Get().Caps(), caps);
    if (vrBuild.h264WidthFallback) {
        RC_LOGW("H264 unsupported at %upx, falling back to HEVC", vrBuild.h264RejectedWidth);
        // A19 audit #4: the headset saw the codec change in its stats but never the
        // reason (the PC page's #codecHint only covers the PC). Same notice channel
        // as the restart-SteamVR message below.
        SendControl({{"type", rc::msg::kNotice},
                     {"level", "warning"},
                     {"text", "H.264 does not support this picture size; the stream switched to HEVC."}});
    }
    VrStreamConfig cfg = vrBuild.cfg;
    const uint32_t codedEyeW = vrBuild.codedEyeWidth;
    const uint32_t codedEyeH = vrBuild.codedEyeHeight;
    if (st.gazeFoveation && cfg.encodeScale >= 1.0f) {
        RC_LOGI("VR stream: foveated %s, encoding %ux%u per eye (scaled from %ux%u)",
                st.eyesTracked ? "where the eyes look" : "at the lens centre",
                codedEyeW, codedEyeH, cfg.foveatedEyeWidth, cfg.foveatedEyeHeight);
    } else if (st.gazeFoveation) {
        // ROUND18 L2-FIX3: the user still has foveation on, but a scaled frame
        // cannot carry it. The full-density centre needs F*eye pixels; below that
        // s = (encEye/eye - F)/(1-F) goes negative and both shaders fold back,
        // stretching the periphery. Keep the eye sizes at 0 so PlaceFovea,
        // the squeeze job, the metadata and the headset inverse all switch off
        // together; codedEyeW/H above already hold the plain scaled size.
        RC_LOGI("VR stream: foveation paused at encode scale %.2f - plain scaled frame", cfg.encodeScale);
    }
    const char* profileName = cfg.profile == rc::StreamProfile::LowLatency ? "latency-first"
                              : cfg.profile == rc::StreamProfile::Quality  ? "quality-first"
                                                                           : "balanced";
    const uint32_t profileMinQp = rc::ProfileParams(cfg.profile).minQp;
    const std::string qpText = profileMinQp ? std::to_string(profileMinQp) : std::string("off");
    // "capped VBR" used to be hardcoded while the encoder actually ran CBR when the
    // quality profile's switch asked for it - a log that lied about the rate control.
    RC_LOGI("VR stream: %s bitrate, %s, ceiling %u kbps, %s (QP floor %s)%s",
            cfg.adaptive ? "adaptive" : "fixed", profileName, cfg.bitrateKbps,
            cfg.cappedVbr ? "capped VBR" : "CBR",
            cfg.cappedVbr ? qpText.c_str() : "n/a",
            cfg.startBitrateKbps ? (", starting at " + std::to_string(cfg.startBitrateKbps) + " kbps").c_str() : "");
    RC_LOGI("VR stream: encode scale %.2f of render size", cfg.encodeScale);

    std::weak_ptr<ClientSession> weak = shared_from_this();
    auto vr = std::make_unique<VrStream>(m_connection.get(), &bridge, cfg,
                                         [weak](const std::string& state, const std::string& producer,
                                                const std::string& app) {
                                             if (auto self = weak.lock()) self->NotifyVrState(state, producer, app);
                                         });
    json vrStream = {{"streamId", rc::kVrStreamId},
                     {"eyeWidth", cfg.eyeWidth},
                     {"eyeHeight", cfg.eyeHeight},
                     // ROUND18 L2-FIX2: the size the encoder actually produced, so the
                     // headset builds its AImageReader for the real frame. These are
                     // PER-EYE sizes: the headset does codedWidth x2 for the full width
                     // (same as it does for foveation.eyeWidth), then Init(full, height).
                     {"codedWidth", codedEyeW},
                     {"codedHeight", codedEyeH},
                     {"fps", cfg.fps},
                     {"codec", int(cfg.codec)},
                     {"tenBit", cfg.tenBit}};
    // See rc::VrFoveationMetadata. Sizes are the SCALED per-eye encode size: the
    // headset derives density s from these, so they must match what foveator.Init
    // in vr_stream.cpp used (both come from ScaledEncodeSize).
    if (cfg.foveatedEyeWidth)
        vrStream["foveation"] = {{"eyeWidth", codedEyeW}, {"eyeHeight", codedEyeH}, {"size", cfg.foveaSize}};
    SendControl({{"type", rc::msg::kStreamConfig}, {"kind", "vr"}, {"streams", json::array({vrStream})}});
    vr->Start();
    {
        std::lock_guard lock(m_mutex);
        m_vr = std::move(vr);
    }
    if (needsRestart) {
        SendControl({{"type", rc::msg::kNotice},
                     {"level", "warning"},
                     {"action", "vr.restartSteamVR"},
                     {"text", "Restart SteamVR to use the new resolution."}});
    }
}

// Matching SteamVR's resolution: once SteamVR renders at another size (its resolution setting
// changed, or a game has its own) and stays there, the stream restarts at it.
//
// A restart is expensive: the encoder, the shared textures (SteamVR waits for the driver to
// reopen them, and the game with it) and the headset's decoder all start over, and the
// picture stops until the next keyframe arrives. A game that changes its size as it goes
// (dynamic resolution, or several sizes while it loads) used to restart the stream every
// couple of seconds: an endless freeze, with the game's audio breaking up on the PC. So a
// size has to differ by more than a few percent and hold for 3 s, and the stream follows
// at most every 20 s; in between it keeps scaling what SteamVR renders.
void ClientSession::FollowSteamVrResolution() {
    bool match;
    {
        std::lock_guard lock(m_mutex);
        match = m_settings.matchSteamVr && !m_slowPath && m_vr;
    }
    uint32_t w = 0, h = 0;
    if (!match || !App::Get().Bridge().SteamVrEyeSize(w, h)) return;
    const auto similar = [](uint32_t a, uint32_t b) {
        const uint32_t d = a > b ? a - b : b - a;
        return d <= std::max<uint32_t>(16, std::max(a, b) / 20);
    };
    if (similar(w, m_vrMatchedWidth) && similar(h, m_vrMatchedHeight)) {
        m_vrSeenWidth = m_vrSeenHeight = 0;
        m_vrSeenTicks = 0;
        return;
    }
    if (!similar(w, m_vrSeenWidth) || !similar(h, m_vrSeenHeight)) {
        m_vrSeenWidth = w;
        m_vrSeenHeight = h;
        m_vrSeenTicks = 0;
        return;
    }
    constexpr int kHoldTicks = 3;                          // ticks are 1 s apart
    constexpr int64_t kMinFollowNs = 20'000'000'000LL;  // between restarts
    if (++m_vrSeenTicks < kHoldTicks) return;
    const int64_t now = rc::NowNs();
    if (m_vrFollowedNs && now - m_vrFollowedNs < kMinFollowNs) return;
    m_vrFollowedNs = now;
    RC_LOGI("SteamVR now renders %ux%u per eye; restarting the VR stream at that size", w, h);
    RestartVr();
}

// A new VR stream with the current settings. One the headset had paused (it is on the
// desktop) stays paused: restarting is no reason to start sending VR video again.
void ClientSession::RestartVr() {
    const bool paused = m_vr && m_vr->Paused();
    StopVr();
    StartVr();
    if (paused && m_vr) m_vr->SetPaused(true);
}

void ClientSession::StopVr() {
    std::unique_ptr<VrStream> old;
    {
        std::lock_guard lock(m_mutex);
        old.swap(m_vr);
    }
    old.reset();
}

void ClientSession::LaunchSteamVr() {
    if (!steamvr::SteamVrPath()) {
        NotifyVrState("error", "steamvr", "", "SteamVR is not installed on this PC.");
        return;
    }
    if (SettingsStore::Get().Snapshot().autoRegisterSteamVrDriver) steamvr::RegisterDriver();
    StartVr();
    // Already streaming, for example when coming back from the desktop: the stream reports
    // only changes, so nothing would ever take the state back from "launching".
    if (m_vr && m_vr->Running()) {
        m_vr->RequestKeyframe();
        return;
    }
    m_steamVrLaunchNs = rc::NowNs();
    NotifyVrState("launching", "steamvr", "");
    {
        std::lock_guard lock(m_mutex);
        if (m_vr) m_vr->RequestKeyframe();
    }
    if (!steamvr::IsRunning()) {
        steamvr::Launch();
    } else if (App::Get().Bridge().ActiveProducer() != rcvr::ProducerKind::SteamVR) {
        // SteamVR is running without our HMD (started before the headset connected
        // or with another driver): restart so it picks up RinCast.
        steamvr::Restart();
    }
}

void ClientSession::NotifyVrState(const std::string& state, const std::string& producer, const std::string& app,
                                  const std::string& error) {
    // While SteamVR is starting, "waiting" means "still launching" to the headset.
    std::string effective = state;
    if (state == "running") m_steamVrLaunchNs = 0;
    if (state == "waiting" && m_steamVrLaunchNs) effective = "launching";
    {
        std::lock_guard lock(m_mutex);
        if (m_vrState == effective && m_vrProducer == producer && m_vrApp == app && error.empty()) return;
        m_vrState = effective;
        m_vrProducer = producer;
        m_vrApp = app;
    }
    json msg{{"type", rc::msg::kVrState}, {"state", effective}, {"producer", producer}, {"app", app}};
    if (!error.empty()) msg["error"] = error;
    SendControl(msg);
    App::Get().OnVrStateChanged(effective, producer, app);
}

void ClientSession::UpdateAudio() {
    const Settings pc = SettingsStore::Get().Snapshot();
    ClientStreamSettings st = StreamSettings();
    const auto route = audio::ResolveRoute(pc.audioDevices);
    const bool streamAudio = pc.streamAudio && st.streamAudio;
    const bool mic = pc.micPassthrough && st.micPassthrough && route.micFeed;

    std::unique_ptr<AudioStreamer> oldAudio;
    std::unique_ptr<MicPlayer> oldMic;
    {
        std::lock_guard lock(m_mutex);
        oldAudio.swap(m_audio);
        oldMic.swap(m_mic);
    }
    oldAudio.reset();
    oldMic.reset();
    audio::RestoreDefaults();

    // "Also play on this PC" keeps the user's speakers: capture the default device instead.
    const wchar_t* speaker = pc.playOnPcToo ? nullptr : route.speaker;
    RC_LOGI("Audio (%s): speaker %s, microphone %s", pc.audioDevices.c_str(),
            speaker ? Narrow(speaker).c_str() : "default device",
            mic ? Narrow(route.mic).c_str() : "off");
    if (pc.switchDefaultAudio) audio::SwitchDefaults(route, streamAudio && speaker, mic);

    std::unique_ptr<AudioStreamer> a;
    std::unique_ptr<MicPlayer> mp;
    if (streamAudio) {
        a = std::make_unique<AudioStreamer>();
        a->Start(m_connection.get(), speaker, m_slowPath ? 96000 : 192000);
    }
    if (mic) {
        mp = std::make_unique<MicPlayer>();
        mp->Start(route.micFeed, route.mic);
    }
    std::lock_guard lock(m_mutex);
    m_audio = std::move(a);
    m_mic = std::move(mp);
}

json ClientSession::StatusJson() {
    json j{{"deviceId", m_hello.deviceId},
           {"deviceName", m_hello.deviceName},
           {"model", m_hello.model},
           {"address", m_connection->Remote().IpString()},
           {"remote", m_remote},
           {"vpn", m_vpn},
           // A Quest on a USB cable reaches the PC by IPv6 link-local only.
           {"usb", m_connection->Remote().IsLinkLocalV6()},
           {"connectedSeconds", (rc::NowNs() - m_connectedAtNs) / 1'000'000'000LL},
           {"rttMs", m_connection->Stats().rttMs}};
    std::lock_guard lock(m_mutex);
    // What the headset chose itself, which goes before this PC's settings.
    j["headsetBitrateMbps"] = m_settings.vrBitrateMbps;
    j["headsetCodec"] = m_settings.codec;
    // The headset's geometric squeeze and the PC's QP map cannot share a frame,
    // so the page greys the QP map switch out when this is on.
    j["headsetGazeFoveation"] = m_settings.gazeFoveation;
    // A19 audit #14 (the read-only tier, no control): frame generation is owned by
    // the headset - the page shows its state next to the other headset-owned picks.
    j["headsetFrameGeneration"] = m_settings.frameGeneration;
    // Read-only hints so the page can grey out CBR, which the encoder ignores when
    // the headset follows SteamVR's resolution or the link is remote/slow.
    j["matchSteamVr"] = m_settings.matchSteamVr;
    j["slowPath"] = m_slowPath;
    j["vrState"] = m_vrState;
    j["vrProducer"] = m_vrProducer;
    j["vrApp"] = m_vrApp;
    if (m_vr) {
        auto s = m_vr->Stats();
        const auto& c = m_vr->Config();
        j["vr"] = {{"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps}, {"bitrateKbps", s.bitrateKbps},
                   {"codec", CodecName(c.codec)}, {"eyeWidth", c.eyeWidth}, {"eyeHeight", c.eyeHeight},
                   {"refreshRate", c.fps}, {"latencyMs", s.clientLatencyMs}, {"decodeMs", s.clientDecodeMs},
                   {"qp", s.qp}, {"encWidth", s.encWidth}, {"encHeight", s.encHeight},
                   {"gameFps", s.gameFps}, {"encFps", s.encFps}, {"droppedFps", s.droppedFps}};
        // A19 audit #2: paused stream (headset on the desktop / in a menu) - the
        // numbers above froze. Additive key; the page stops the poor counts and the
        // trouble card while it is set, and reads the numbers as live again without it.
        if (m_vr->Paused()) j["vr"]["paused"] = true;
        // ROUND19 T3: read-only ladder level for the page (null when the switch is off).
        // No lock here: StatusJson already holds m_mutex (taken at the top), and re-locking
        // it on the same thread throws resource_deadlock_would_occur - the streamer died
        // exactly this way on the first UI tick after a session started (ROUND20 L16).
        j["vr"]["autoscaleLevel"] =
            SettingsStore::Get().Snapshot().l7Autoscale ? json(m_autoscale.Pct()) : json(nullptr);
    }
    json desk = json::array();
    for (auto& [id, d] : m_desktop) {
        auto s = d->Stats();
        desk.push_back({{"name", d->Monitor().name}, {"fps", s.fps}, {"mbps", s.mbps}});
    }
    j["desktop"] = desk;
    j["client"] = m_clientStats;
    return j;
}

}  // namespace rcs
