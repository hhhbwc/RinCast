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
using namespace std::chrono_literals;
#define RC_LOGI(...) ((void)0)
#define RC_LOGW(...) (++errors)
namespace rc {
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
namespace msg { constexpr auto kUpdateSettings = "settings.update"; }
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
    void SendMonitors() {}
    void Enqueue(json);
    void ScheduleVideoRestart(bool,bool);
    void WorkerLoop();
    void ApplyClientInfo(const json&);
    bool AdoptHeadsetOwnedSettings();
    void OwnSyncedSettings(const json&,const std::function<void()>&);
    json SyncedSettingsForUi();
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
        check(SettingsStore::Get().Snapshot().streamProfile==2,"headset profile did not persist");
        check(partial.SyncedSettingsForUi()["adaptiveBitrate"]==true,"UI differs from latest headset setting");
    }
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
    batch.HandleMessage(report(batch,{{"codec","hevc"},{"streamProfile",2}}));
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
    args.out.mkdir(parents=True, exist_ok=True)
    baseline = subprocess.check_output(["git", "-C", str(REPO), "show", "636f9a4:pc/streamer/src/session/client_session.cpp"], encoding="utf-8")
    assert 'if (type == "_profile") return RestartVideoForProfile();' in baseline
    methods = ["void ClientSession::Enqueue(", "void ClientSession::ScheduleVideoRestart(",
               "void ClientSession::WorkerLoop(", "void ClientSession::ApplyClientInfo(",
               "bool ClientSession::AdoptHeadsetOwnedSettings(", "void ClientSession::OwnSyncedSettings(",
               "json ClientSession::SyncedSettingsForUi(", "void ClientSession::OnSettingsChanged(",
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
    for method, stream in (("void ClientSession::StartVr()", "Vr"), ("void ClientSession::StartDesktop(", "Desktop")):
        body = block(CPP, method)
        assert "cfg.adaptive = m_slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate)" in body
        assert "cfg.profile = rc::ProfileFromId(pc.streamProfile)" in body
        assert f"m_restart{stream} = false;" in body
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
    cache = (REPO / "build/pc/CMakeCache.txt").read_text(encoding="utf-8")
    compiler_bin = Path(re.search(r"^CMAKE_AR:FILEPATH=(.*)$", cache, re.M)[1]).parent
    msvc = compiler_bin.parents[2]
    sdk = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10"
    version = sorted(p.name for p in (sdk / "Include").iterdir() if (p / "um/Windows.h").exists())[-1]
    env = {k: os.environ[k] for k in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP") if k in os.environ}
    env["PATH"] = str(compiler_bin) + r";C:\Windows\System32"
    env["INCLUDE"] = ";".join(map(str, [msvc / "include", sdk / "Include" / version / "ucrt", sdk / "Include" / version / "shared", sdk / "Include" / version / "um", REPO / "third_party"]))
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
