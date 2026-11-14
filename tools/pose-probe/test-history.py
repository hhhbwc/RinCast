import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "pc/driver-openvr/src/driver.cpp").read_text(encoding="utf-8")


def section(start, end):
    if source.count(start) != 1:
        raise RuntimeError(f"Expected one source marker: {start}")
    first = source.index(start)
    return source[first:source.index(end, first)]


math = section("struct Quat {", "// Rigid transform:")
pose = section("vr::DriverPose_t MakePose(", "// ---------------------------------------------------------------------------\n// Shared state")
matcher = section("    rcvr::TrackingSnapshot RenderedWith(", "    rcvr::DisplayConfig m_cfg;")

harness = r'''
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include "openvr/openvr_driver.h"
#include "vr_bridge.h"

void Log(const char*, ...) {}
'''
harness += math + pose
harness += r'''
struct TestState {
    std::mutex mutex;
    std::deque<rcvr::TrackingSnapshot> history;
    rcvr::TrackingSnapshot frameTracking{};
};
TestState state;
TestState* g_shared = &state;
class Matcher {
public:
    int m_poseMismatchLogs = 0;
'''
harness += matcher
harness += r'''
};

vr::HmdMatrix34_t Matrix(const rcvr::BridgePose& pose) {
    const double x = pose.orientation[0], y = pose.orientation[1];
    const double z = pose.orientation[2], w = pose.orientation[3];
    vr::HmdMatrix34_t m{};
    m.m[0][0] = float(1 - 2 * (y*y + z*z));
    m.m[0][1] = float(2 * (x*y - z*w));
    m.m[0][2] = float(2 * (x*z + y*w));
    m.m[1][0] = float(2 * (x*y + z*w));
    m.m[1][1] = float(1 - 2 * (x*x + z*z));
    m.m[1][2] = float(2 * (y*z - x*w));
    m.m[2][0] = float(2 * (x*z - y*w));
    m.m[2][1] = float(2 * (y*z + x*w));
    m.m[2][2] = float(1 - 2 * (x*x + y*y));
    for (int i = 0; i < 3; ++i) m.m[i][3] = pose.position[i];
    return m;
}

void Fill(int hz, bool moving, bool rotated) {
    state.history.clear();
    for (int i = 0; i < 32; ++i) {
        rcvr::TrackingSnapshot t{};
        t.sequence = i + 1;
        t.receivedPcNs = 1'000'000'000LL + int64_t(i) * 1'000'000'000LL / hz;
        t.headsetTargetNs = t.receivedPcNs + 40'000'000LL;
        t.hmd.orientation[1] = rotated ? float(std::sin(0.3)) : 0.f;
        t.hmd.orientation[3] = rotated ? float(std::cos(0.3)) : 1.f;
        t.hmd.position[0] = moving ? i * 0.001f : 0.f;
        t.hmd.position[1] = 1.6f;
        state.history.push_back(t);
    }
    state.frameTracking = state.history.back();
}

int main() {
    Matcher matcher;
    int failed = 0, tests = 0;
    auto check = [&](const char* label, uint64_t got, uint64_t expected) {
        ++tests;
        if (got != expected) ++failed;
        std::printf("%s %s: selected=%llu expected=%llu\n", got == expected ? "PASS" : "FAIL",
                    label, (unsigned long long)got, (unsigned long long)expected);
    };
    for (int hz : {72, 90, 120}) {
        Fill(hz, false, false);
        const auto got = matcher.RenderedWith(Matrix(state.history.back().hmd));
        const double extraMs = double(state.history.back().receivedPcNs - got.receivedPcNs) / 1e6;
        std::printf("still %d Hz: timestamp overstatement %.3f ms\n", hz, extraMs);
        check("stationary identity", got.sequence, 32);
        Fill(hz, false, true);
        check("stationary rotated", matcher.RenderedWith(Matrix(state.history.back().hmd)).sequence, 32);
    }
    Fill(90, true, false);
    for (size_t index : {size_t(0), size_t(7), size_t(19), size_t(31)})
        check("unique moving pose", matcher.RenderedWith(Matrix(state.history[index].hmd)).sequence, index + 1);
    state.history[20].hmd = state.history[7].hmd;
    check("duplicate matching poses", matcher.RenderedWith(Matrix(state.history[7].hmd)).sequence, 21);
    Fill(90, true, true);
    check("unique rotated pose", matcher.RenderedWith(Matrix(state.history[8].hmd)).sequence, 9);
    state.history.clear();
    state.frameTracking.sequence = 900;
    check("empty history fallback", matcher.RenderedWith(Matrix(state.frameTracking.hmd)).sequence, 900);
    std::printf("%d tests, %d failures\n", tests, failed);
    return failed ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="rincast-pose-history-") as temp:
    directory = pathlib.Path(temp)
    (directory / "main.cpp").write_text(harness, encoding="utf-8")
    (directory / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.22)\n'
        'project(pose_history LANGUAGES CXX)\n'
        'add_executable(pose-history main.cpp)\n'
        'target_compile_features(pose-history PRIVATE cxx_std_17)\n'
        'target_compile_definitions(pose-history PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)\n'
        f'target_include_directories(pose-history PRIVATE "{ROOT.as_posix()}/third_party" '
        f'"{ROOT.as_posix()}/pc/common")\n', encoding="utf-8")
    subprocess.run(["cmake", "-S", temp, "-B", str(directory / "build"), "-G",
                    "Visual Studio 17 2022", "-A", "x64"], check=True)
    subprocess.run(["cmake", "--build", str(directory / "build"), "--config", "Release"], check=True)
    result = subprocess.run([str(directory / "build/Release/pose-history.exe")])
    raise SystemExit(result.returncode)
