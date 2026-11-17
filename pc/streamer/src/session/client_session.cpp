#include "client_session.h"
#include "../vr/face_share.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

#include "../app.h"
#include "../audio/audio.h"
#include "../encode/encoder.h"
#include "../input/input_injector.h"
#include "../settings.h"
#include "../stream/desktop_stream.h"
#include "../stream/foveator.h"
#include "../stream/vr_stream.h"
#include "../vr/steamvr.h"
#include "../vr/vr_bridge_host.h"
#include "rc/platform.h"
#include "rc/stream_profile.h"

namespace rcs {

namespace {

float QualityScale(const std::string& q) {
    if (q == "low") return 0.6f;
    if (q == "medium") return 0.75f;
    if (q == "ultra") return 1.0f;
    if (q == "godlike") return 1.2f;
    return 0.9f;  // high
}

uint32_t Align(uint32_t v, uint32_t a) { return std::max(a, (v + a / 2) / a * a); }

// The most the stream takes of SteamVR's resolution, against the headset's panel (the slider on
// the headset stops there too): Virtual Desktop's highest. Headsets before 1.2.0 send their
// runtime's recommended size instead, smaller, and get less. A codec or decoder that can't take
// the size still brings it down below.
constexpr double kMaxVrScale = 1.5;

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
    const bool profileDiff = !m_ownedSettings.count(rc::synced::kStreamProfile) &&
                             pc.streamProfile != m_settings.streamProfile;
    const bool adaptiveDiff = !m_ownedSettings.count(rc::synced::kAdaptiveBitrate) &&
                              pc.adaptiveBitrate != m_settings.adaptiveBitrate;
    if (!profileDiff && !adaptiveDiff) return false;
    SettingsStore::Get().Update([&](Settings& s) {
        if (profileDiff) s.streamProfile = m_settings.streamProfile;
        if (adaptiveDiff) s.adaptiveBitrate = m_settings.adaptiveBitrate;
    });
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
    if (!m.is_object() || !m.contains("type")) return;
    // Input events are latency sensitive and cheap: handle them inline.
    const std::string type = m["type"].get<std::string>();
    if (!m_inputAllowed && (type == rc::msg::kMouseButton || type == rc::msg::kMouseWheel ||
                            type == rc::msg::kKey || type == rc::msg::kText))
        return;
    if (type == rc::msg::kMouseButton) {
        input::MouseButton(m.value("button", 0), m.value("down", false));
        return;
    }
    if (type == rc::msg::kMouseWheel) {
        input::MouseWheel(m.value("dx", 0.f), m.value("dy", 0.f));
        return;
    }
    if (type == rc::msg::kKey) {
        input::Key(uint16_t(m.value("vk", 0)), m.value("down", false));
        return;
    }
    if (type == rc::msg::kText) {
        input::Text(Widen(m.value("text", "")));
        return;
    }
    if (type.empty() || type.front() == '_') return;
    m.erase("_pcRevisions");
    if (m.contains("settings")) {
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
                if (!(*it)["settings"].is_object() || !message.value("settings", json::object()).is_object()) break;
                (*it)["settings"].update(message.value("settings", json::object()));
                if (message.contains("_pcRevisions")) (*it)["_pcRevisions"].update(message["_pcRevisions"]);
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
        // First connect: adopt the headset's profile/bitrate picks before StartVr
        // builds cfg from the PC copy, so the page and the encoder agree from frame 1.
        AdoptHeadsetOwnedSettings();
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
        AdoptHeadsetOwnedSettings();
        const bool profileChanged = before.streamProfile != after.streamProfile;
        const bool videoChanged = before.codec != after.codec || before.tenBit != after.tenBit ||
                                  before.vrQuality != after.vrQuality || before.vrScale != after.vrScale ||
                                  before.matchSteamVr != after.matchSteamVr || before.gazeFoveation != after.gazeFoveation ||
                                  before.eyesTracked != after.eyesTracked ||
                                  before.frameGeneration != after.frameGeneration || before.refreshRate != after.refreshRate ||
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
        m_clientStats = m.value("stats", json::object());
        // ROUND19 T3: the decode-side autoscale ladder (default off). The headset's
        // decode average arrives here at ~1 Hz - the natural tick. A missing decodeMs
        // reads as no pressure (the ladder is conservative on absent signals), the
        // PC's own dropped-frame average and the handshake's decode-fps hint fill the
        // rest in. A step rebuilds the VR stream on the existing hot path, at most
        // once per cooldown - never the rebuild storm of 513f150.
        if (SettingsStore::Get().Snapshot().l7Autoscale) {
            rc::L7Autoscale::Config c;
            const double hintMs = m_caps.maxFpsAtMaxRes > 0 ? 1000.0 / m_caps.maxFpsAtMaxRes : 0.0;
            c.decodeBudgetMs = hintMs > 0 ? hintMs * 0.8 : 20.0;
            m_autoscale.Configure(c);
            const double decodeMs = m_clientStats.value("decodeMs", -1.0);
            const double dropped = m_vr ? double(m_vr->Stats().droppedFps) : -1.0;
            const auto r = m_autoscale.Update(decodeMs, dropped, rc::NowNs());
            if (r.changed)
                RC_LOGI("L7 autoscale: encode scale -> %d%% (decode %.1f ms vs budget %.1f ms, dropped %.1f fps)",
                        m_autoscale.Pct(), decodeMs, c.decodeBudgetMs, dropped);
            if (r.changed) OnStreamProfileChanged();
        }
        return;
    }
}

void ClientSession::ApplyClientInfo(const json& m) {
    std::lock_guard lock(m_mutex);
    if (m.contains("display")) {
        const auto& d = m["display"];
        m_caps.recommendedEyeWidth = d.value("eyeWidth", m_caps.recommendedEyeWidth);
        m_caps.recommendedEyeHeight = d.value("eyeHeight", m_caps.recommendedEyeHeight);
        m_caps.ipd = d.value("ipd", m_caps.ipd);
        if (d.contains("refreshRates")) m_caps.refreshRates = d["refreshRates"].get<std::vector<float>>();
        if (d.contains("fov") && d["fov"].size() == 2)
            for (int e = 0; e < 2; e++)
                for (int k = 0; k < 4; k++) m_caps.fovTan[e][k] = d["fov"][e][k].get<float>();
    }
    if (m.contains("decoders")) {
        const auto& d = m["decoders"];
        m_caps.h264 = d.value("h264", true);
        m_caps.hevc = d.value("hevc", true);
        m_caps.hevc10 = d.value("hevc10", false);
        m_caps.av1 = d.value("av1", false);
        m_caps.av110 = d.value("av110", false);
        m_caps.maxDecodeWidth = d.value("maxWidth", 4096u);
        m_caps.maxDecodeHeight = d.value("maxHeight", 4096u);
        // ROUND18 L3: optional fps ceiling. 0/missing = old headset, no warning.
        // Reject malformed values via the shared validator (also unit-tested).
        m_caps.maxFpsAtMaxRes = rc::ValidateDecodeFpsHint(d.value("maxFpsAtMaxRes", 0u));
    }
    if (m.contains("settings")) {
        auto s = m["settings"];
        const auto revisions = m.value("_pcRevisions", json::object());
        // A headset snapshot queued before a PC edit must not undo that edit later.
        for (const char* key : rc::synced::kAll)
            if (revisions.value(key, uint64_t(0)) < m_pcKeyRevisions[key]) s.erase(key);
        m_settings.codec = s.value("codec", m_settings.codec);
        m_settings.tenBit = s.value("tenBit", m_settings.tenBit);
        m_settings.vrQuality = s.value("vrQuality", m_settings.vrQuality);
        m_settings.vrScale = s.value("vrScale", m_settings.vrScale);
        m_settings.matchSteamVr = s.value("matchSteamVr", m_settings.matchSteamVr);
        m_settings.gazeFoveation = s.value("gazeFoveation", m_settings.gazeFoveation);
        m_settings.eyesTracked = s.value("eyesTracked", m_settings.eyesTracked);  // headsets before 1.2.1 only foveate tracked
        m_settings.frameGeneration = s.value("frameGeneration", m_settings.frameGeneration);
        m_settings.refreshRate = s.value("refreshRate", m_settings.refreshRate);
        m_settings.vrBitrateMbps = s.value("vrBitrateMbps", m_settings.vrBitrateMbps);
        m_settings.desktopBitrateMbps = s.value("desktopBitrateMbps", m_settings.desktopBitrateMbps);
        m_settings.desktopFps = s.value("desktopFps", m_settings.desktopFps);
        m_settings.adaptiveBitrate = s.value("adaptiveBitrate", m_settings.adaptiveBitrate);
        m_settings.streamProfile = std::clamp(s.value("streamProfile", m_settings.streamProfile), 0, 2);
        // The preference is a PC-side setting for now (rcs::Settings::streamProfile).
        // The headset will send its own choice once its three-way UI exists.
        m_settings.streamAudio = s.value("streamAudio", m_settings.streamAudio);
        m_settings.micPassthrough = s.value("micPassthrough", m_settings.micPassthrough);
        // 头显改了某个可同步项（值与 PC 推过去的不一样）→ 这一项的所有权归它，PC 不再推。
        for (const char* key : rc::synced::kAll) {
            if (!s.contains(key)) continue;
            auto it = m_ownedSettings.find(key);
            if (it == m_ownedSettings.end()) continue;
            if (it->second != s[key]) m_ownedSettings.erase(it);
        }
    }
}

rc::VideoCodec ClientSession::ChooseCodec(bool forVr) const {
    const EncoderCaps& enc = App::Get().Caps();
    const ClientCaps& dec = m_caps;
    const bool tenBit = m_settings.tenBit;
    auto ok = [&](rc::VideoCodec c) {
        switch (c) {
            case rc::VideoCodec::H264: return enc.h264 && dec.h264 && !tenBit;
            case rc::VideoCodec::HEVC: return enc.hevc && dec.hevc && (!tenBit || (enc.hevc10 && dec.hevc10));
            case rc::VideoCodec::AV1: return enc.av1 && dec.av1 && (!tenBit || (enc.av110 && dec.av110));
        }
        return false;
    };
    std::string pref = m_settings.codec;
    if (pref == "auto") pref = SettingsStore::Get().Snapshot().codec;
    if (pref == "av1" && ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    if (pref == "h264" && (enc.h264 && dec.h264)) return rc::VideoCodec::H264;
    if (pref == "hevc" && ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    // Auto: HEVC is the best latency/quality trade-off on Quest; AV1 when HEVC is missing.
    if (ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    if (ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    (void)forVr;
    return rc::VideoCodec::H264;
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
    const Settings s = SettingsStore::Get().Snapshot();
    const EncoderCaps& caps = App::Get().Caps();
    const auto route = audio::ResolveRoute(s.audioDevices);
    const auto svr = steamvr::SteamVrPath();
    SendControl({
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
        {"vrState", m_vrState},
        // 可同步的共享设置：只推 PC 仍然拥有的那些项（头显改过的项所有权已收回）。
        // 码率不算：头显已有「跟随 PC」的 0 值语义，双向覆盖会破坏它。
        {"settings", OwnedSettingsJson()},
    });
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
    if (changed.contains(rc::synced::kStreamProfile)) m_settings.streamProfile = s.streamProfile;
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
    }
    RC_LOGI("L7 autoscale: reset to 100% by the user");
    OnStreamProfileChanged();
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
    StopDesktop();
    const Settings pc = SettingsStore::Get().Snapshot();
    std::vector<MonitorInfo> monitors = EnumerateMonitors();
    std::vector<std::string> wanted;
    if (m.contains("monitors") && m["monitors"].is_array())
        for (const auto& id : m["monitors"]) wanted.push_back(id.get<std::string>());

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

    float fps = st.desktopFps > 0 ? float(st.desktopFps) : (pc.desktopFps > 0 ? float(pc.desktopFps) : st.refreshRate);
    fps = std::clamp(fps, 30.f, 120.f);
    const int totalMbps = st.desktopBitrateMbps > 0 ? st.desktopBitrateMbps : pc.desktopBitrateMbps;
    double totalPixels = 0;
    for (auto* mon : selected) totalPixels += double(mon->width) * mon->height;

    const uint32_t maxW = std::min<uint32_t>(caps.maxDecodeWidth, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const uint32_t maxH = std::min<uint32_t>(caps.maxDecodeHeight, codec == rc::VideoCodec::H264 ? 4096 : 8192);

    json streams = json::array();
    std::map<uint8_t, std::unique_ptr<DesktopStream>> created;
    for (auto* mon : selected) {
        double w = mon->width, h = mon->height;
        double scale = 1.0;
        if (pc.desktopMaxHeight > 0 && h > pc.desktopMaxHeight) scale = pc.desktopMaxHeight / h;
        scale = std::min({scale, maxW / w, maxH / h});
        DesktopStreamConfig cfg;
        cfg.cheapEncode = m_vr != nullptr;  // VR in flight: the desktop skips two-pass
        cfg.codec = codec;
        cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
        cfg.width = Align(uint32_t(w * scale), 2);
        cfg.height = Align(uint32_t(h * scale), 2);
        cfg.fps = fps;
        cfg.bitrateKbps = uint32_t(totalMbps * 1000.0 * (double(mon->width) * mon->height / totalPixels));
        cfg.bitrateKbps = std::max<uint32_t>(cfg.bitrateKbps, 5000);
        // Internet paths lose packets and have no retransmissions: recover more of them locally.
        cfg.fecPercent = m_slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
        // The desktop stream keeps the legacy pacing for now (PLAN-pacing-fix §7: it
        // was not part of the diagnosed lock). NOTE: the send thread back-pressures the
        // capacity-1 send slot when the pacer is slower than the encode - the old claim
        // that "encoding overlaps the send for free" only holds while the send is faster.
        cfg.pace = true;
        cfg.adaptiveQuant = pc.adaptiveQuantization;
        // Over the internet a fixed bitrate only fills queues or loses frames: always adapt.
        cfg.adaptive = m_slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);
        cfg.profile = rc::ProfileFromId(pc.streamProfile);
        // Over the internet the upload is often far below the LAN bitrate: start low and
        // let the bitrate control find what the path carries, instead of filling queues.
        if (m_slowPath) cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, std::max<uint32_t>(3000, 8000 / uint32_t(selected.size())));
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
    dc.ipd = caps.ipd;
    std::memcpy(dc.fovTan, caps.fovTan, sizeof(dc.fovTan));
    strncpy_s(dc.headsetModel, m_hello.model.empty() ? "Quest" : m_hello.model.c_str(), _TRUNCATE);
    strncpy_s(dc.headsetSerial, m_hello.deviceId.c_str(), _TRUNCATE);
    dc.renderEyeWidth = renderW;
    dc.renderEyeHeight = renderH;
    RC_LOGI("VR stream: %ux%u per eye%s", dc.eyeWidth, dc.eyeHeight,
            !match ? "" : seenW ? (", SteamVR renders " + std::to_string(seenW) + "x" + std::to_string(seenH)).c_str()
                                : ", following SteamVR's resolution once it renders");

    const bool needsRestart = bridge.Configure(dc);
    bridge.SetHeadsetConnected(true);

    VrStreamConfig cfg;
    cfg.codec = codec;
    cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
    cfg.eyeWidth = dc.eyeWidth;
    cfg.eyeHeight = dc.eyeHeight;
    cfg.fps = rate;
    cfg.bitrateKbps = uint32_t(rcs::EffectiveVrBitrate(pc.vrBitrateMbps, st.vrBitrateMbps, pc.pcBitrateWins) * 1000);
    if (m_slowPath) cfg.bitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 60000);
    cfg.fecPercent = m_slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
    // PLAN-pacing-fix WP2b: the legacy "pace = true" made the pacer follow the guard's
    // cap, which self-locked (cap down -> pacer slower -> one frame stretched -> the
    // capacity-1 send slot back-pressures the encoder -> the queue climbs -> cap down;
    // the real device's downlink hit 76.7 ms). The user's ceiling drives the pacer now,
    // and a frame leaves within its own frame period. A remote path keeps the legacy.
    if (m_slowPath || pc.videoPacingMode == 2) cfg.pacingMode = 2;
    else if (pc.videoPacingMode == 1) cfg.pacingMode = 1;
    else cfg.pacingMode = 3;  // burst: the LAN default
    cfg.pace = cfg.pacingMode != 1;
    cfg.adaptive = m_slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);  // see above
    cfg.profile = rc::ProfileFromId(pc.streamProfile);
    // ROUND18 L2: the user's encode scale overrides the profile's value (both 1.0
    // today). 0 = follow the profile. The percentage is stored as an int for the UI.
    cfg.encodeScale = pc.encodeScalePct > 0 ? rc::ClampEncodeScale(pc.encodeScalePct / 100.0f)
                                            : rc::ProfileParams(cfg.profile).encodeScale;
    // ROUND19 T3: the autoscale ladder multiplies the base above (never raises it);
    // the level survives until the user's manual reset. The frame size itself still
    // comes out of ScaledEncodeSize, the one formula shared with the foveation config.
    if (pc.l7Autoscale) {
        std::lock_guard lock(m_mutex);
        cfg.encodeScale = rc::ClampEncodeScale(cfg.encodeScale * m_autoscale.Pct() / 100.0f);
    }
    if (m_slowPath) cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 12000);
    // QP map is the user's own switch, just like two-pass. A profile no longer
    // turns it on behind their back. The encoder runs it only when the headset's
    // geometric squeeze is off (vr_stream.cpp gates it on !job.foveated) - the two
    // cannot share a frame - so the page says so when that squeeze is on.
    cfg.foveated = pc.foveatedEncoding;
    cfg.adaptiveQuant = pc.adaptiveQuantization;
    // Two-pass is the user's own switch, never forced by a profile. The Best-picture
    // profile used to turn it on by itself (and the page greyed it out), which
    // surprised users who had deliberately left it off; the user decides.
    cfg.twoPass = pc.twoPassEncoding;
    // CBR is the quality profile's "spend exactly this" mode; opt-in, quality only.
    cfg.cappedVbr = !rcs::QualityProfileCbr(pc.qualityProfileCbr, pc.profileControlsVideo && !match && !m_slowPath,
                                            cfg.profile == rc::StreamProfile::Quality);
    cfg.motionField = st.frameGeneration;
    // ROUND18 L2-FIX2: the real encoded PER-EYE size after encodeScale. One value
    // feeds the foveation metadata, the coded-size hint and the log, so the headset
    // can never undo foveation with a different density than the encoder used.
    // Start from the plain (un-foveated) base; the foveated block overrides it.
    uint32_t codedEyeW = rc::ScaledEncodeSize(cfg.eyeWidth * 2, cfg.encodeScale) / 2;
    uint32_t codedEyeH = rc::ScaledEncodeSize(cfg.eyeHeight, cfg.encodeScale);
    if (st.gazeFoveation && cfg.encodeScale >= 1.0f) {
        cfg.foveaSize = st.eyesTracked ? Foveator::kFoveaSize : Foveator::kFixedFoveaSize;
        // When the user lets the profiles drive video settings, each one floors the
        // full-density centre at its own share (latency 0.6, balanced 0.7, quality 0.8)
        // and only ever raises; matched-SteamVR sizes and the slow internet path pick
        // their own. This runs BEFORE the EncodedSize calls, so the sizes encoded - and
        // the share sent to the headset - come from the same value.
        cfg.foveaSize = QualityProfileFoveaFor(cfg.foveaSize, pc.profileControlsVideo && !match && !m_slowPath,
                                               int(cfg.profile));
        cfg.foveatedEyeWidth = Foveator::EncodedSize(cfg.eyeWidth, cfg.foveaSize);
        cfg.foveatedEyeHeight = Foveator::EncodedSize(cfg.eyeHeight, cfg.foveaSize);
        codedEyeW = rc::ScaledEncodeSize(cfg.foveatedEyeWidth * 2, cfg.encodeScale) / 2;
        codedEyeH = rc::ScaledEncodeSize(cfg.foveatedEyeHeight, cfg.encodeScale);
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
