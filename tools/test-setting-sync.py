"""Compile production settings methods with CPU-only stream/store stubs; never contacts a device."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO = Path(__file__).resolve().parents[1]
CPP = (REPO / "pc/streamer/src/session/client_session.cpp").read_text(encoding="utf-8")
HEADER = (REPO / "pc/streamer/src/session/client_session.h").read_text(encoding="utf-8")


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|(?<![\w])\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]')
    depth = 0
    for match in tokens.finditer(source, opening):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end()]
    raise AssertionError(signature)


PRELUDE = r'''
#include <nlohmann/json.hpp>
#include "json_field.h"
#include "rc/auto_tune.h"
#include "rc/stream_profile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using json = nlohmann::json;
using namespace rcs;
using namespace std::chrono_literals;
#define RC_LOGI(...) ((void)0)
#define RC_LOGW(...) (++errors)
namespace rc {
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
namespace msg { constexpr auto kUpdateSettings = "settings.update"; constexpr auto kNotice = "notice"; }
namespace synced {
constexpr auto kCodec="codec", kTenBit="tenBit", kAdaptiveBitrate="adaptiveBitrate", kDesktopFps="desktopFps", kStreamProfile="streamProfile";
constexpr const char* kAll[]={kCodec,kTenBit,kAdaptiveBitrate,kDesktopFps,kStreamProfile};
}
}
struct ScopedCoInit {};
struct Settings {
    std::string codec="auto";
    bool tenBit=false, adaptiveBitrate=true;
    int desktopFps=60, streamProfile=1;
    json ToJson(bool) const { return {{"codec",codec},{"tenBit",tenBit},{"adaptiveBitrate",adaptiveBitrate},{"desktopFps",desktopFps},{"streamProfile",streamProfile},{"pcName","fixture"}}; }
};
struct SettingsStore {
    std::mutex mutex;
    Settings settings;
    static SettingsStore& Get() { static SettingsStore s; return s; }
    Settings Snapshot() { std::lock_guard lock(mutex); return settings; }
    void Update(const std::function<void(Settings&)>& f) { std::lock_guard lock(mutex); f(settings); }
};
struct App {
    static App& Get() { static App a; return a; }
    void PushUi(const json&) {}
    json StateJson() { return {}; }
};
std::string SyncedKeyTitle(const std::string& key) { return key; }
std::string A82RateList(const std::vector<float>& rates) {
    std::string out;
    for (float rate : rates) { if (!out.empty()) out += '/'; out += std::to_string(int(rate + 0.5f)); }
    return out;
}
float EffectiveRefreshRate(float requested, const std::vector<float>& rates) {
    return rates.empty() || std::find(rates.begin(), rates.end(), requested) != rates.end() ? requested : rates.back();
}
struct FakeDesktop {
    struct Info { std::string id="monitor"; };
    Info Monitor() { return {}; }
};
'''

CLASS = r'''
struct ClientSession {
    std::mutex m_mutex, m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<json> m_queue;
    std::thread m_worker;
    std::atomic<bool> m_closed{false};
    bool m_restartVr=false, m_restartDesktop=false;
    int64_t m_restartDueNs=0, m_lastSettingsRestartNs=0;
    uint64_t m_pcRevision=0;
    std::map<std::string,uint64_t> m_pcKeyRevisions;
    std::map<std::string,json> m_ownedSettings;
    std::map<std::string,int64_t> m_ownershipNoticeNs;
    rc::AutoTune m_autoTune;
    ClientCaps m_caps;
    ClientStreamSettings m_settings;
    bool m_vr=true;
    std::map<int,std::unique_ptr<FakeDesktop>> m_desktop;
    std::atomic<int> vrRestarts{0}, desktopRestarts{0}, handled{0}, errors{0};
    ClientStreamSettings StreamSettings() { std::lock_guard lock(m_mutex); return m_settings; }
    void RestartVr() { ++vrRestarts; }
    void StopDesktop() {}
    void StartDesktop(const json&) { ++desktopRestarts; }
    void ApplyInputSetting(bool) {}
    void SendServerInfo() {}
    void SendControl(const json&) {}
    void SendMonitors() {}
    void Enqueue(json);
    void ScheduleVideoRestart(bool,bool);
    void WorkerLoop();
    void ApplyClientInfo(const json&);
    bool AdoptHeadsetOwnedSettings();
    void OwnSyncedSettings(const json&,const std::function<void()>&);
    json SyncedSettingsForUi();
    json OwnedSettingsJson();
    void OnSettingsChanged(const json&);
    void OnStreamProfileChanged();
    void RestartVideoForProfile(bool,bool);
    void BaselineRestartVideoForProfile();
    void HandleMessage(const json& m);
    void Start() { m_worker=std::thread([this]{WorkerLoop();}); }
    void Close() {
        { std::lock_guard lock(m_queueMutex); m_closed=true; }
        m_queueCv.notify_all();
        if(m_worker.joinable()) m_worker.join();
    }
    ~ClientSession() { Close(); }
};
'''

TESTS = r'''
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
json report(ClientSession& s,json settings) {
    std::lock_guard lock(s.m_mutex);
    json revisions=json::object();
    for(auto key:rc::synced::kAll) if(settings.contains(key)) revisions[key]=s.m_pcKeyRevisions[key];
    return {{"type",rc::msg::kUpdateSettings},{"settings",settings},{"_pcRevisions",revisions}};
}
int main() {
try {
    ClientSession baseline;
    for(int i=0;i<20;++i) baseline.BaselineRestartVideoForProfile();
    check(baseline.vrRestarts==20,"baseline does not expose 20 restarts");
    ClientSession queue;
    for(int i=0;i<200;++i) {
        queue.Enqueue(report(queue,{{"adaptiveBitrate",bool(i%2)}}));
        queue.OnStreamProfileChanged();
        queue.OnSettingsChanged({{"adaptiveBitrate",bool(i%2)}});
    }
    check(queue.m_queue.size()==3,"pending queue did not coalesce");
    check(queue.m_queue.front()["settings"]["adaptiveBitrate"]==true,"latest value lost");
    queue.Enqueue({{"type","lifecycle-barrier"}});
    queue.OnStreamProfileChanged();
    check(queue.m_queue.size()==5,"coalescing crossed lifecycle barrier");
    ClientSession partial;
    partial.Enqueue(report(partial,{{"codec","hevc"}}));
    partial.OwnSyncedSettings({{"codec","av1"}},[]{SettingsStore::Get().Update([](Settings& s){s.codec="av1";});});
    partial.Enqueue(report(partial,{{"adaptiveBitrate",false}}));
    check(partial.m_queue.size()==1,"partial merge failed");
    partial.ApplyClientInfo(partial.m_queue.front());
    partial.AdoptHeadsetOwnedSettings();
    check(partial.StreamSettings().codec=="av1","merged report upgraded stale field revision");
    check(!partial.StreamSettings().adaptiveBitrate,"partial merge dropped another key");
    check(partial.m_ownedSettings.contains("codec"),"stale report stole ownership");
    for(int i=0;i<40;++i) {
        auto stale=report(partial,{{"adaptiveBitrate",true},{"streamProfile",2}});
        partial.OwnSyncedSettings({{"adaptiveBitrate",false},{"streamProfile",0}},[]{
            SettingsStore::Get().Update([](Settings& s){s.adaptiveBitrate=false;s.streamProfile=0;});
        });
        partial.ApplyClientInfo(stale);
        partial.AdoptHeadsetOwnedSettings();
        check(!SettingsStore::Get().Snapshot().adaptiveBitrate,"stale headset overwrote PC");
        check(partial.SyncedSettingsForUi()["streamProfile"]==0,"UI differs from latest PC setting");
        auto fresh=report(partial,{{"adaptiveBitrate",true},{"streamProfile",2}});
        partial.ApplyClientInfo(fresh);
        partial.AdoptHeadsetOwnedSettings();
        check(SettingsStore::Get().Snapshot().adaptiveBitrate,"headset true did not persist");
        check(SettingsStore::Get().Snapshot().streamProfile==0,"legacy headset profile regained PC ownership");
        check(partial.SyncedSettingsForUi()["adaptiveBitrate"]==true,"UI differs from latest headset setting");
        check(partial.SyncedSettingsForUi()["streamProfile"]==0,"legacy profile leaked into UI");
        check(partial.StreamSettings().streamProfile==1,"legacy profile changed session state");
        check(!partial.m_ownedSettings.contains("streamProfile"),"legacy profile retained ownership");
        check(partial.m_pcKeyRevisions["streamProfile"]==0,"legacy profile created a PC revision");
        check(!partial.OwnedSettingsJson().contains("streamProfile"),"legacy profile was pushed back");
    }
    ClientSession legacyOnly;
    legacyOnly.m_desktop[1]=std::make_unique<FakeDesktop>();
    legacyOnly.HandleMessage(report(legacyOnly,{{"streamProfile",2}}));
    check(!legacyOnly.m_restartVr && !legacyOnly.m_restartDesktop,"legacy profile rebuilt video");
    check(legacyOnly.m_queue.empty(),"legacy profile pushed UI");
    ClientSession concurrent;
    auto waitingReport=report(concurrent,{{"adaptiveBitrate",true}});
    std::promise<void> saving, reporting;
    auto savingStarted=saving.get_future();
    auto reportStarted=reporting.get_future();
    auto pc=std::async(std::launch::async,[&]{
        concurrent.OwnSyncedSettings({{"adaptiveBitrate",false}},[&]{
            saving.set_value();
            reportStarted.wait();
            SettingsStore::Get().Update([](Settings& s){s.adaptiveBitrate=false;});
        });
    });
    savingStarted.wait();
    auto headset=std::async(std::launch::async,[&]{
        reporting.set_value();
        concurrent.ApplyClientInfo(waitingReport);
        concurrent.AdoptHeadsetOwnedSettings();
    });
    pc.get(); headset.get();
    check(!SettingsStore::Get().Snapshot().adaptiveBitrate,"concurrent old report overwrote PC save");
    check(concurrent.SyncedSettingsForUi()["adaptiveBitrate"]==false,"concurrent UI snapshot mismatch");
    ClientSession cooldown;
    cooldown.m_lastSettingsRestartNs=rc::NowNs();
    cooldown.ScheduleVideoRestart(true,false);
    check(cooldown.m_restartDueNs>=cooldown.m_lastSettingsRestartNs+3'000'000'000LL,"cooldown was shortened");
    cooldown.ScheduleVideoRestart(false,true);
    check(cooldown.m_restartVr && cooldown.m_restartDesktop,"cooldown dropped a dirty stream");
    ClientSession audio;
    audio.OnSettingsChanged({{"pcName","name"}});
    check(audio.m_queue.size()==1 && audio.m_queue.front()["type"]=="_serverInfo","unrelated edit rebuilt audio");
    ClientSession batch;
    batch.m_desktop[1]=std::make_unique<FakeDesktop>();
    batch.HandleMessage(report(batch,{{"codec","hevc"}}));
    check(batch.m_restartVr && batch.m_restartDesktop,"combined change lost a stream restart");
    check(batch.vrRestarts==0,"headset handler restarted immediately");
    ClientSession burst;
    burst.m_desktop[1]=std::make_unique<FakeDesktop>();
    burst.Start();
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<20;++i) {
        burst.Enqueue(report(burst,{{"adaptiveBitrate",bool(i%2)}}));
        burst.OnStreamProfileChanged();
        std::this_thread::sleep_for(100ms);
    }
    const auto last=std::chrono::steady_clock::now();
    std::this_thread::sleep_for(500ms);
    const auto drain=std::chrono::steady_clock::now();
    {
        std::lock_guard lock(burst.m_queueMutex);
        check(burst.m_queue.empty() && burst.m_restartDueNs==0,"burst not drained");
    }
    check(burst.vrRestarts>0 && burst.vrRestarts<=2,"burst exceeds two VR rebuilds");
    check(burst.desktopRestarts==burst.vrRestarts,"batch split VR and desktop rebuilds");
    check(burst.errors==0,"worker exception");
    burst.Close();
    ClientSession idle;
    idle.m_vr=false;
    idle.RestartVideoForProfile(true,true);
    check(idle.vrRestarts==0 && idle.desktopRestarts==0,"idle edit started streaming");
    ClientSession cancel;
    cancel.ScheduleVideoRestart(true,true);
    const auto closing=std::chrono::steady_clock::now();
    cancel.Start();
    cancel.Close();
    check(std::chrono::steady_clock::now()-closing<100ms,"close waits for debounce");
    json result={{"kind","production-method CPU harness; streams/store stubbed"},{"baselineVrRestarts",baseline.vrRestarts.load()},
        {"burstVrRestarts",burst.vrRestarts.load()},{"burstDesktopRestarts",burst.desktopRestarts.load()},
        {"clicks",20},{"burstMs",std::chrono::duration_cast<std::chrono::milliseconds>(last-start).count()},
        {"drainCheckMs",std::chrono::duration_cast<std::chrono::milliseconds>(drain-last).count()},
        {"alternatingRounds",40},{"concurrentSaveReport","PASS"},{"round7StaticGuards","PASS"},{"pendingAfter200Edits",3},{"status","PASS"}};
    std::puts(result.dump(2).c_str());
    return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    baseline = subprocess.check_output(["git", "-C", str(REPO), "show", "636f9a4:pc/streamer/src/session/client_session.cpp"], encoding="utf-8")
    assert 'if (type == "_profile") return RestartVideoForProfile();' in baseline
    methods = ["void ClientSession::Enqueue(", "void ClientSession::ScheduleVideoRestart(",
               "void ClientSession::WorkerLoop(", "void ClientSession::ApplyClientInfo(",
               "bool ClientSession::AdoptHeadsetOwnedSettings(", "void ClientSession::OwnSyncedSettings(",
               "json ClientSession::SyncedSettingsForUi(", "json ClientSession::OwnedSettingsJson(",
               "void ClientSession::OnSettingsChanged(",
               "void ClientSession::OnStreamProfileChanged(", "void ClientSession::RestartVideoForProfile("]
    app = (REPO / "pc/streamer/src/app.cpp").read_text(encoding="utf-8")
    settings = (REPO / "pc/streamer/src/settings.h").read_text(encoding="utf-8")
    effects = block(app, "std::string App::ApplySettingSideEffects(")
    expression = re.search(r'if \((changed\.contains\("streamProfile"\).*?)\)\s*session->OnStreamProfileChanged\(\);', effects, re.S)[1]
    declared = settings.split("// Video", 1)[1].split("// Audio", 1)[0]
    keys = set(re.findall(r'^\s*(?:bool|int|std::string)\s+(\w+)\s*=', declared, re.M))
    assert set(re.findall(r'changed\.contains\("(\w+)"\)', expression)) == keys
    assert effects.count("session->OnStreamProfileChanged();") == 1
    assert not re.search(r'\b(?:StartVr|StartDesktop|RestartVr|RunAsync)\s*\(', effects)
    assert '"settings", SettingsForUi()' in block(app, "json App::StateJson()")
    assert '"settings", SettingsForUi()' in block(app, 'if (type == "setSettings")')
    connect = block(block(CPP, "void ClientSession::HandleMessage("), "if (type == rc::msg::kClientInfo)")
    assert connect.index("AdoptHeadsetOwnedSettings();") < connect.index("StartVr();")
    config_header = (REPO / "pc/streamer/src/stream/stream_config.h").read_text(encoding="utf-8")
    assert config_header.count("cfg.profile = rc::ProfileFromId(pc.streamProfile);") == 2
    for method, builder in (("void ClientSession::StartVr()", "BuildVrStreamConfigGuarded(eff,"),
                            ("void ClientSession::StartDesktop(", "BuildDesktopStreamConfig(eff,")):
        body = block(CPP, method)
        assert "const Settings eff = EffectiveSettings(pc);" in body
        assert builder in body
        assert "cfg.adaptive = slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate)" in config_header
        assert f"m_restart{'Vr' if 'StartVr' in method else 'Desktop'} = false;" in body
    auto_tune = block(CPP, "void ClientSession::RunAutoTune()")
    assert "sig.decodeMs = vs.clientDecodeMs;" in auto_tune
    assert "sig.lastFrameNs = vs.lastFrameNs;" in auto_tune
    assert "sig.lastFeedbackNs = vs.lastFeedbackNs;" in auto_tune
    assert "sig.streamActive = m_vr && m_vr->Running() && !m_vr->Paused();" in auto_tune
    assert "m_autoTune.UpdateWithEvidence(sig, in, now)" in auto_tune
    assert "if (!sig.streamActive || !rc::HasFreshSignals(sig, now)) return;" in auto_tune
    apply_info = block(CPP, "void ClientSession::ApplyClientInfo(")
    assert 'IntField(s, "streamProfile"' not in apply_info
    assert 'std::string(key) == rc::synced::kStreamProfile || !s.contains(key)' in apply_info
    own = block(CPP, "void ClientSession::OwnSyncedSettings(")
    assert 'std::string(key) == rc::synced::kStreamProfile || !changed.contains(key)' in own
    assert 'm_pcKeyRevisions.erase(rc::synced::kStreamProfile);' not in own
    update = block(CPP, "void ClientSession::HandleMessage(")
    assert "profileChanged" not in update
    sender = (REPO / "pc/streamer/src/stream/video_sender.cpp").read_text(encoding="utf-8")
    send = block(sender, "void VideoSender::Send(")
    assert send.index("m_packetizer.SendFrame(") < send.index("m_lastFrameNs.store(")
    assert "m_lastFrameNs.store(now, std::memory_order_release)" in send
    feedback = block(sender, "void VideoSender::OnFeedback(")
    assert feedback.index("const bool measured = fb.queueDelayMs >= 0;") < feedback.index("m_lastFeedbackNs.store(")
    assert feedback.index("m_lossAvg.store(") < feedback.index("m_lastFeedbackNs.store(")
    assert "m_lastFeedbackNs.store(now, std::memory_order_release)" in feedback
    stats = block(sender, "VideoSenderStats VideoSender::Stats(")
    assert "m_lastFrameNs.load(std::memory_order_acquire)" in stats
    assert "m_lastFeedbackNs.load(std::memory_order_acquire)" in stats
    vr_header = (REPO / "pc/streamer/src/stream/vr_stream.h").read_text(encoding="utf-8")
    apply_rung = block(vr_header, "void ApplyProfileRung(")
    assert apply_rung.index("m_liveProfile.store(") < apply_rung.index("m_pendingProfile.store(")
    assert "m_pendingMinQp" not in vr_header
    vr = (REPO / "pc/streamer/src/stream/vr_stream.cpp").read_text(encoding="utf-8")
    encoder_loop = block(vr, "void VrStream::RunEncoder(")
    assert "m_liveProfile(int(config.profile))" in vr
    assert "rc::ProfileFromId(m_liveProfile.load(std::memory_order_relaxed))" in encoder_loop
    assert "ec.profile = liveProfile;" in encoder_loop
    assert "ec.profile = m_config.profile" not in encoder_loop
    assert "encoder->SetProfile(liveProfile)" in encoder_loop
    sender_loop = block(vr, "void VrStream::RunSender(")
    assert "m_pendingProfile.exchange(-1" in sender_loop
    assert "m_sender.SetProfile(rc::ProfileFromId(pendingProfile))" in sender_loop
    encoder_header = (REPO / "pc/streamer/src/encode/encoder.h").read_text(encoding="utf-8")
    assert "virtual bool SetProfile(rc::StreamProfile /*profile*/) { return false; }" in encoder_header
    nvenc = (REPO / "pc/streamer/src/encode/nvenc_encoder.cpp").read_text(encoding="utf-8")
    set_profile = block(nvenc, "bool SetProfile(rc::StreamProfile profile) override")
    assert "const auto q = rc::ProfileParams(profile);" in set_profile
    assert "const auto plan = RatePlan(profile, now);" in set_profile
    assert "ApplyBudget(candidateConfig, plan);" in set_profile
    assert "candidateConfig.rcParams.minQP.qpInterP = qp;" in set_profile
    assert "nvEncReconfigureEncoder" in set_profile
    assert set_profile.index("if (!ok)") < set_profile.index("m_config.profile = profile;")
    assert set_profile.index("m_config.profile = profile;") < set_profile.index("m_encodeConfig = candidateConfig;")
    assert 'GetEnvironmentVariableA("RINCAST_CADENCE_FILL", text, sizeof(text)) == 1' in nvenc
    assert "text[0] == '1'" in nvenc
    rate_plan = block(nvenc, "rc::EncoderRatePlan RatePlan(rc::StreamProfile profile, int64_t now) const")
    assert "CadenceFillExperiment() && m_cappedVbr" in rate_plan
    assert "profile == rc::StreamProfile::Quality" in rate_plan
    assert "!m_config.desktop &&\n                                 !startup && m_config.fps > 0" in rate_plan
    assert "const bool pressure = !cadenceFill &&" in rate_plan
    assert "fillOn && !cadenceFill, cadenceFill);" in rate_plan
    reconfigure = block(nvenc, "void ReconfigureRate(int64_t now)")
    assert "ApplyBudget(candidateConfig, plan);" in reconfigure
    assert "rc::ClampVbvInitialDelay(candidateConfig.rcParams.vbvInitialDelay, plan.budget.vbvBufferSize)" in reconfigure
    assert "candidateInit.frameRateNum = plan.rateNum;" in reconfigure
    assert "const bool ok = rc::ApplyEncoderRateTransaction(" in reconfigure
    assert reconfigure.index("const bool ok = rc::ApplyEncoderRateTransaction(") < reconfigure.index("m_encodeConfig = candidateConfig;")
    assert "// resetEncoder/forceIDR 为零" in reconfigure
    assert "RINCAST_CADENCE_FILL" not in settings
    assert "RINCAST_CADENCE_FILL" not in config_header
    motion_header = (REPO / "pc/streamer/src/stream/motion_estimator.h").read_text(encoding="utf-8")
    motion_cpp = (REPO / "pc/streamer/src/stream/motion_estimator.cpp").read_text(encoding="utf-8")
    assert "static constexpr int kReadbackSlots = 4;" in motion_header
    assert "Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging[kReadbackSlots];" in motion_header
    assert "Microsoft::WRL::ComPtr<ID3D11Query> m_readQuery[kReadbackSlots];" in motion_header
    assert "bool m_readPending[kReadbackSlots]{};" in motion_header
    assert "bool HasPendingRead() const" in motion_header
    assert "void ResetPrevious()" in motion_header
    assert "bool QueueRead(ID3D11DeviceContext* ctx, const rc::MotionReadbackTiming& timing);" in motion_header
    assert "bool TryRead(ID3D11DeviceContext* ctx, std::vector<float>& out, rc::MotionReadbackTiming& timing, bool& slotConsumed);" in motion_header
    queue_read = block(motion_cpp, "bool MotionEstimator::QueueRead(")
    assert "ctx->CopyResource(m_staging[slot].Get(), m_field.tex.Get());" in queue_read
    assert "ctx->End(m_readQuery[slot].Get());" in queue_read
    assert "m_readPending[slot] = true;" in queue_read
    try_read = block(motion_cpp, "bool MotionEstimator::TryRead(")
    assert "D3D11_ASYNC_GETDATA_DONOTFLUSH" in try_read
    assert "if (!done) return false;" in try_read
    assert try_read.index("if (!done) return false;") < try_read.index("D3D11_MAP_READ, 0, &map")
    assert try_read.index("D3D11_MAP_READ, 0, &map") < try_read.rindex("m_readPending[slot] = false;")
    sync_read = block(motion_cpp, "bool MotionEstimator::Read(")
    assert "D3D11_MAP_READ, 0, &map" in sync_read
    assert "D3D11_MAP_FLAG_DO_NOT_WAIT" not in sync_read
    assert 'GetEnvironmentVariableA("RINCAST_ASYNC_MOTION", text, sizeof(text)) == 1' in vr
    assert "text[0] == '0'" in vr
    run = block(vr, "void VrStream::Run(")
    motion_policy = (REPO / "shared/include/rc/motion_field.h").read_text(encoding="utf-8")
    assert "const bool asyncMotion = AsyncMotionReadback();" in run
    assert "rc::ShouldResetMotionPrevious(fs.frameIndex, lastMotionFrameIndex, motionSkipPending)" in run
    assert "if (!previousFrameIndex) return false;" in motion_policy
    assert "currentFrameIndex - previousFrameIndex != 1 &&" in motion_policy
    assert "!(deliberateSkipPending && currentFrameIndex - previousFrameIndex == 2)" in motion_policy
    assert "motion.ResetPrevious();" in run
    assert "if (motionValid && asyncMotion)" in run
    assert "motion.QueueRead(ctx.Get(), timing)" in run
    assert "WaitForMotionField(" in vr
    flow = block(vr, "void VrStream::LogFrameFlow(")
    assert flow.index("overruns1s %u") < flow.index("motionPending %u")
    assert "m_diagMotionPending = 0;" in flow
    assert "RINCAST_ASYNC_MOTION" not in settings
    assert "RINCAST_ASYNC_MOTION" not in config_header
    assert 'Enqueue(json{{"type", "_profile"}});' in block(CPP, "void ClientSession::OnStreamProfileChanged()")
    handler = block(block(CPP, "void ClientSession::HandleMessage("), "if (type == rc::msg::kUpdateSettings)")
    dispatch = '''void ClientSession::HandleMessage(const json& m) {
        ++handled;
        const std::string type=m.value("type","");
        if(type=="_profile") return ScheduleVideoRestart(true,true);
        if(type=="_rebuild") return RestartVideoForProfile(m.value("vr",false),m.value("desktop",false));
    ''' + handler + "\n}\n"
    source = PRELUDE + block(HEADER, "struct ClientCaps") + ";\n" + block(HEADER, "struct ClientStreamSettings") + ";\n" + CLASS
    source += "\n".join(block(CPP, name) for name in methods) + "\n" + dispatch
    source += block(baseline, "void ClientSession::RestartVideoForProfile()").replace("ClientSession::RestartVideoForProfile()", "ClientSession::BaselineRestartVideoForProfile()")
    source += TESTS
    cpp = args.out / "settings-harness.cpp"
    cpp.write_text(source, encoding="utf-8")
    cache_candidates = [REPO / "build/pc/CMakeCache.txt", REPO / "build/CMakeCache.txt"]
    cache_path = next((p for p in cache_candidates if p.exists()), None)
    assert cache_path is not None, "CMakeCache.txt not found under build/pc or build"
    cache = cache_path.read_text(encoding="utf-8")
    compiler_bin = Path(re.search(r"^CMAKE_AR:FILEPATH=(.*)$", cache, re.M)[1]).parent
    msvc = compiler_bin.parents[2]
    sdk = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10"
    version = sorted(p.name for p in (sdk / "Include").iterdir() if (p / "um/Windows.h").exists())[-1]
    env = {k: os.environ[k] for k in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP") if k in os.environ}
    env["PATH"] = str(compiler_bin) + r";C:\Windows\System32"
    env["INCLUDE"] = ";".join(map(str, [msvc / "include", sdk / "Include" / version / "ucrt", sdk / "Include" / version / "shared", sdk / "Include" / version / "um", REPO / "third_party", REPO / "shared/include", REPO / "pc/streamer/src"]))
    env["LIB"] = ";".join(map(str, [msvc / "lib/x64", sdk / "Lib" / version / "ucrt/x64", sdk / "Lib" / version / "um/x64"]))
    exe = args.out / "settings-harness.exe"
    built = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/utf-8", "/std:c++20", "/EHsc", "/O2", "/DNDEBUG", str(cpp), "/Fe:" + str(exe)], cwd=args.out, env=env, capture_output=True)
    (args.out / "settings-harness-build.log").write_bytes(built.stdout + built.stderr)
    if built.returncode:
        print((built.stdout + built.stderr).decode("utf-8", errors="replace"))
        return built.returncode
    result = subprocess.run([str(exe)], capture_output=True, timeout=20)
    (args.out / "settings-harness-result.json").write_bytes(result.stdout)
    print((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
