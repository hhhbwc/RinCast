// One connected headset: control protocol, desktop & VR video streams,
// input injection and audio in both directions.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../capture/monitors.h"
#include "../net/stream_server.h"
#include "../settings.h"  // A84: EffectiveSettings works on a Settings snapshot
#include "../util.h"
#include "rc/auto_tune.h"
#include "rc/autoscale.h"
#include "rc/connection.h"
#include "rc/protocol.h"

namespace rcs {

class DesktopStream;
class VrStream;
class AudioStreamer;
class MicPlayer;

struct ClientCaps {
    bool h264 = true, hevc = true, hevc10 = false, av1 = false, av110 = false;
    uint32_t maxDecodeWidth = 4096, maxDecodeHeight = 4096;
    uint32_t maxFpsAtMaxRes = 0;  // ROUND18 L3: headset's measured fps ceiling at max size, 0 = unknown
    std::vector<float> refreshRates{72, 90};
    uint32_t recommendedEyeWidth = 1832, recommendedEyeHeight = 1920;  // 100% of the resolution: the panel (the runtime's recommended size before 1.2.0)
    float fovTan[2][4] = {{1.0f, 0.9f, 0.95f, 1.1f}, {0.9f, 1.0f, 0.95f, 1.1f}};
    float ipd = 0.063f;
};

struct ClientStreamSettings {
    std::string codec = "auto";
    bool tenBit = false;
    bool frameGeneration = false;
    std::string vrQuality = "high";   // low | medium | high | ultra | godlike (headsets before 1.1.0)
    float vrScale = 0;                // pixels per eye against the headset's own; 0 = from vrQuality
    bool matchSteamVr = false;        // stream at the size SteamVR renders at
    bool gazeFoveation = false;       // foveated encoding around where the eyes look
    int debugEncodeScalePct = 0;      // ROUND20 L17: the headset's force_encode_scale debug switch (0 off)
    bool eyesTracked = true;          // false: the headset can't track them; fixed at the lens centre
    float refreshRate = 72;  // ROUND18 L4 default (was 90); 90 stays available
    int vrBitrateMbps = 0;            // 0 = PC default
    int desktopBitrateMbps = 0;
    int desktopFps = 0;
    bool adaptiveBitrate = true;
    int streamProfile = 1;             // Legacy wire field; A86: ignored, tuner owns the live rung.
    bool streamAudio = true;
    bool micPassthrough = true;
};

class ClientSession : public std::enable_shared_from_this<ClientSession> {
   public:
    ClientSession(std::unique_ptr<rc::Connection> connection, const HelloInfo& hello, std::string clientPkHex,
                  std::vector<uint8_t> welcome, bool remote, bool vpn);
    ~ClientSession();

    void Start();
    void Close(const std::string& reason);

    void HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from);
    void Tick(int64_t nowNs);

    const std::string& DeviceId() const { return m_hello.deviceId; }
    const std::string& DeviceKey() const { return m_hello.deviceKey; }
    const std::string& DeviceName() const { return m_hello.deviceName; }
    const std::string& Model() const { return m_hello.model; }
    const std::string& ClientPublicKeyHex() const { return m_clientPk; }
    const std::vector<uint8_t>& WelcomePacket() const { return m_welcome; }
    int64_t LastReceiveNs() const { return m_connection->LastReceiveNs(); }
    bool PeerDisconnected() const { return m_connection->PeerDisconnected(); }
    bool IsRemote() const { return m_remote; }
    rc::Connection* Connection() { return m_connection.get(); }

    void SendControl(const json& message);
    json StatusJson();

    // Called by the VR stream when the producer state changes.
    void NotifyVrState(const std::string& state, const std::string& producer, const std::string& app,
                       const std::string& error = {}, const std::string& errorKey = {});
    void OnSettingsChanged(const json& changed);
    void OwnSyncedSettings(const json& changed, const std::function<void()>& save);
    // Rebuild streams affected by build-time settings; live picture-tier moves do not call this.
    void OnStreamProfileChanged();
    void OnMonitorsChanged();
    // ROUND19 T3: the user's manual reset is the only way the autoscale ladder
    // climbs back to 100 %; the rebuilt stream picks the new level up on the hot path.
    void ResetAutoscale();

    rc::VideoCodec ChooseCodec(bool forVr) const;
    ClientCaps Caps();
    ClientStreamSettings StreamSettings();
    // The synced values the PC UI should show: PC-owned keys come from the saved
    // settings, headset-owned keys from this session. Never writes anything back.
    json SyncedSettingsForUi();

   private:
    void OnControl(const std::string& text);
    void OnPacket(rc::PacketType type, const uint8_t* payload, size_t size);
    void WorkerLoop();
    void HandleMessage(const json& m);
    void Enqueue(json message);
    void ScheduleVideoRestart(bool vr, bool desktop);

    void SendServerInfo();
    void SendMonitors();
    void StartDesktop(const json& m);
    void StopDesktop();
    void StartVr();
    void StopVr();
    void RestartVr();
    void RestartVideoForProfile(bool vr, bool desktop);
    void LaunchSteamVr();
    void UpdateAudio();
    void ApplyClientInfo(const json& m);
    // PLAN-AUTO-TUNE P1: the one-per-session capability summary (read-only) and
    // the decision layer behind Settings::autoTune (A84: default ON - settings.h;
    // an install that turns it off is the pre-A84 branch, in which nothing here
    // runs). RunAutoTune
    // runs under m_mutex from the kStats handler; LogAutoCapability from the
    // kClientInfo handler. A84b §1: the enable decision is stored in rc::AutoTune
    // (fed here from the settings snapshot), and the kStats handler calls
    // RunAutoTune on every tick whatever the switch says.
    void LogAutoCapability();
    void RunAutoTune();
    // A86b: the picture tier is session state owned by the tuner - no slider, no
    // floor, no settings write-back. The Locked variant runs with m_mutex held
    // (RunAutoTune); the other takes it (the stream builds). -1 means "not chosen
    // yet" and resolves to 1 (balanced), the starting rung, not a lower bound.
    int EffectiveProfile();
    int EffectiveProfileLocked();
    // A stream-build snapshot with the effective rung substituted: identical to the
    // saved settings except for streamProfile. Takes m_mutex, so it must not be
    // called from under it (RunAutoTune uses EffectiveProfileLocked for that).
    Settings EffectiveSettings(const Settings& pc);
    // The headset's adaptive-bitrate pick is PC-backed and wins unless a newer PC
    // write exists. Used on both first connect and later changes. Returns true if written back.
    bool AdoptHeadsetOwnedSettings();
    void MeasurePoseWait(int64_t receivedNs);
    json OwnedSettingsJson();

    std::unique_ptr<rc::Connection> m_connection;
    // Network thread only: how long each tracking packet waited for SteamVR to take a pose,
    // reported to the headset once a second (rc::msg::kVrPoseWait).
    std::vector<int64_t> m_poseWaits;
    int64_t m_poseWaitPeriodNs = 0, m_poseWaitSentNs = 0;
    HelloInfo m_hello;
    std::string m_clientPk;
    std::vector<uint8_t> m_welcome;
    bool m_remote;    // through the account, with its proof
    bool m_vpn;       // no account, but through a tunnel
    bool m_slowPath;  // either way: smaller packets, a careful bitrate
    std::atomic<bool> m_closed{false};
    // "Let headsets use the mouse and keyboard": checked on every input event, so not a
    // settings snapshot each time.
    std::atomic<bool> m_inputAllowed{true};
    void ApplyInputSetting(bool announce);

    // Control work happens off the network thread.
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<json> m_queue;
    bool m_restartVr = false, m_restartDesktop = false;
    int64_t m_restartDueNs = 0, m_lastSettingsRestartNs = 0;

    std::mutex m_mutex;  // protects everything below
    ClientCaps m_caps;
    ClientStreamSettings m_settings;
    rc::L7Autoscale m_autoscale;  // ROUND19 T3: stepped down by decode pressure, reset by hand
    rc::AutoTune m_autoTune;      // PLAN-AUTO-TUNE / A84b §1: Settings::autoTune is
                                  // fed in as Config::enabled by RunAutoTune, and the
                                  // layer is what goes inert - no gate at this site
    // A86b: the rung auto has put the live streams at (-1 = not chosen yet).
    // Guarded by m_mutex like m_autoTune. Nothing else writes it now: auto may move
    // it down under pressure and up with headroom, and the apply path is in-place
    // (A87), so no stream rebuild is involved.
    int m_autoProfileEff = -1;
    int64_t m_autoSignalLogNs = 0;  // the 30 s AUTO: signals cadence
    // The synced settings the PC currently owns (key -> the value it pushed). The
    // headset takes a key back by sending a different value for it.
    std::map<std::string, json> m_ownedSettings;
    uint64_t m_pcRevision = 0;
    std::map<std::string, uint64_t> m_pcKeyRevisions;
    // A19 audit #5: when each synced key last announced its ownership move; a key
    // that just moved stays quiet for 10 s so a settings batch can't spam notices.
    std::map<std::string, int64_t> m_ownershipNoticeNs;
    std::vector<MonitorInfo> m_monitors;
    std::map<uint8_t, std::unique_ptr<DesktopStream>> m_desktop;  // by stream id
    std::unique_ptr<VrStream> m_vr;
    std::unique_ptr<AudioStreamer> m_audio;
    std::unique_ptr<MicPlayer> m_mic;
    std::string m_vrState = "idle";
    std::string m_vrProducer;
    std::string m_vrApp;
    std::atomic<int64_t> m_steamVrLaunchNs{0};
    // Matching SteamVR's resolution: the size the VR stream was started for, a new one
    // SteamVR has rendered at since, for how many ticks (s) it held, and when the stream
    // last restarted to follow one.
    uint32_t m_vrMatchedWidth = 0, m_vrMatchedHeight = 0;
    uint32_t m_vrSeenWidth = 0, m_vrSeenHeight = 0;
    int m_vrSeenTicks = 0;
    int64_t m_vrFollowedNs = 0;
    void FollowSteamVrResolution();
    int64_t m_connectedAtNs = 0;
    json m_clientStats;
    uint32_t m_lastMouseMonitor = 0;
};

}  // namespace rcs
