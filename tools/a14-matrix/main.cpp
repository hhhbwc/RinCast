// a14-matrix: TASK-PERF-MATRIX-2026-11-21 (overnight A14) - a measurement stick,
// not an assertion stick. Runs exactly the grids the task card specifies; nothing
// here changes production behaviour and no parameter is tuned to look good.
//
// Part 1 prices the REAL encoder (nvEncEncodePicture + bitstream lock, i.e. the
// full VideoEncoder::Encode call) over the card's switch grid, on synthetic
// frames (gradient / moving square / noise texture, a three-frame cycle standing
// for light / medium / heavy encode load). Per combination: >=60 frames, p50/p95
// encode ms, output bytes -> actual Mbps, zero dropped frames, one encoder
// session opened and closed per combination.
//
// Part 2 drives the REAL VideoPacketizer (A10 token-bucket burst mode, exactly
// the production LAN path: pacing at the user's ceiling x 1.08, frame budget =
// frame period) over a REAL loopback UDP Connection pair built the way
// shared/tests/core_tests.cpp's SessionLink does it. Per combination: >=300
// frames submitted at the grid's fps, measuring frame completion latency (first
// fragment constructed -> last fragment received), the ceiling hold, completion
// interval jitter, the queue backlog peak (how long submits blocked at the frame
// boundary) and zero lost frames.
//
//   a14-matrix --part1 [--out1 csv] [--sizes-out json] [--frames1 63] [--shard i/n]
//   a14-matrix --part2 [--out2 csv] [--sizes-in json] [--light/--medium/--heavy KB] [--frames 300] [--shard i/n]
//   a14-matrix --leak-probe [--cycles 60]
//
// Grids (verbatim from the task card):
//   Part 1: codec {h264, hevc, av1} x scale {50, 67, 85, 100} (encodeScalePct)
//           x tenBit {off, on} (tenBit x h264 skipped - A12 F2 degraded semantics)
//           x AQ {off, on} x per-eye resolution {2144, 2688} (2688 = the PICO
//           display mode) x profile {0, 2} (latency-first / quality-first).
//   Part 2: bitrate cap {20, 100, 150, 200, 500} Mbps x fecPercent {0, 3, 10, 20}
//           x fps {36, 51, 72} (36 = half rate -> x2 per-frame payload, the VD
//           route semantics; 51/72 keep the measured tier sizes) x frame size
//           load {light, medium, heavy} (the three Part 1 content tiers).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include "../../pc/streamer/src/encode/encoder.h"
#include "../../pc/streamer/src/gpu.h"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/socket.h"
#include "rc/stream_profile.h"
#include "rc/video_transport.h"

using namespace rcs;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

struct Options {
    bool part1 = false, part2 = false, leakProbe = false;
    bool paced1 = true;  // Part 1 submits at the display rate (72 fps) like production; --unpaced for raw back-to-back
    int frames1 = 63;    // Part 1: >=60 measured frames per combination
    int frames2 = 300;   // Part 2: >=300 frames per combination
    int shardIndex = 0, shardCount = 1;
    std::string out1 = "a14-part1.csv", out2 = "a14-part2.csv", sizesOut = "a14-sizes.json", sizesIn = "a14-sizes.json";
    double lightKb = 4.0, mediumKb = 40.0, heavyKb = 160.0;  // Part 2 fallback sizes (KB)
    int leakCycles = 60;
    // The user's current settings (settings.json 2026-11-21): what Part 1 fixes
    // and what the matrix's baseline row mirrors. Not tuned - recorded.
    constexpr static uint32_t kFps = 72;              // PICO 4 locked at 72 Hz
    constexpr static uint32_t kBitrateKbps = 100000;  // vrBitrateMbps = 100
    constexpr static bool kFillMode = true;           // pc.fillMode
};

// nearest-rank percentile on a sorted copy (the same rule rc/percentile.h uses)
static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const size_t rank = size_t(std::ceil(p / 100.0 * double(v.size())));
    return v[std::min(rank, v.size()) - 1];
}

// Writes the CSV header only when the file is empty or new (shard-safe append).
static void EnsureHeader(const std::string& path, const std::string& header) {
    std::error_code ec;
    const bool haveData = std::filesystem::exists(path, ec) && !ec &&
                          std::filesystem::file_size(path, ec) > 0 && !ec;
    if (haveData) return;
    std::ofstream f(path, std::ios::app);
    f << header << "\n";
}

static bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out = std::move(s);
    return true;
}

// Extracts "key":number from the sizes JSON (no JSON dependency wanted here).
static bool JsonNumber(const std::string& text, const char* key, long long& value) {
    const std::string pat = std::string("\"") + key + "\"";
    const size_t k = text.find(pat);
    if (k == std::string::npos) return false;
    const size_t colon = text.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    value = std::strtoll(text.c_str() + colon + 1, nullptr, 10);
    return true;
}

// ---------------------------------------------------------------------------
// Synthetic content: gradient / moving square / fresh noise (light / medium /
// heavy), generated in the 8-bit domain; P010 expands at upload time.
// ---------------------------------------------------------------------------

struct SynthFrame {
    std::vector<uint8_t> y, uv;  // y: w*h, uv: w*h/2 (interleaved U,V pairs)
};

static void MakeGradient(SynthFrame& f, int w, int h) {
    f.y.resize(size_t(w) * h);
    f.uv.resize(size_t(w) * h / 2);
    // A genuinely smooth, menu-like picture (easy content): horizontal luma ramp
    // with a gentle vertical tilt, drifting chroma ramps. No high-frequency detail.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            f.y[size_t(y) * w + x] = uint8_t(x * 192 / w + y * 63 / h);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f.uv[size_t(y) * w + 2 * x] = uint8_t(120 + (x * 60 / (w / 2)));
            f.uv[size_t(y) * w + 2 * x + 1] = uint8_t(120 + (y * 60 / (h / 2)));
        }
}

static void DrawSquare(SynthFrame& f, int w, int h, int cx, int cy, int half, uint8_t yv) {
    for (int y = std::max(0, cy - half); y < std::min(h, cy + half); ++y)
        for (int x = std::max(0, cx - half); x < std::min(w, cx + half); ++x)
            f.y[size_t(y) * w + x] = yv;
}

static void FillNoise(SynthFrame& f, int w, int h, uint64_t seed) {
    uint64_t s = seed | 1;
    const auto next = [&s] {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    };
    for (auto& v : f.y) v = uint8_t(next() >> 33);
    for (auto& v : f.uv) v = uint8_t(next() >> 33);
}

// NV12 (8-bit) or P010 (10-bit samples in bits 15:6) upload buffer.
static void PackFrame(const SynthFrame& f, int w, int h, bool tenBit, std::vector<uint8_t>& upload) {
    const size_t lumaWords = size_t(w) * h;
    if (!tenBit) {
        upload.resize(lumaWords + lumaWords / 2);
        std::memcpy(upload.data(), f.y.data(), lumaWords);
        std::memcpy(upload.data() + lumaWords, f.uv.data(), lumaWords / 2);
        return;
    }
    upload.resize((lumaWords + lumaWords / 2) * 2);
    uint16_t* p = reinterpret_cast<uint16_t*>(upload.data());
    for (size_t i = 0; i < lumaWords; ++i) p[i] = uint16_t(f.y[i]) << 6;
    uint16_t* q = p + lumaWords;
    for (size_t i = 0; i < lumaWords / 2; ++i) q[i] = uint16_t(f.uv[i]) << 6;
}

// ---------------------------------------------------------------------------
// Part 1 - the real encoder over the card's grid
// ---------------------------------------------------------------------------

struct Part1Row {
    std::string status = "ok";  // ok | initfail | encodefail
    std::string codec;
    int eyeBase = 0, scalePct = 0, profileId = 0;
    bool tenBit = false, aq = false;
    uint32_t encW = 0, encH = 0;
    int frames = 0, failures = 0;
    double encP50 = 0, encP95 = 0, keyMs = 0;
    double tierP50[3] = {0, 0, 0};  // per-content-tier encode p50 (the mixed p50 above hides the heavy frame)
    double tierKb[3] = {0, 0, 0};
    double avgKb = 0, mbps72 = 0;
    long long initNs = 0;
    int slowFrames = 0;  // frames whose encode alone exceeded the 72 fps period
};

static const char* CodecTag(rc::VideoCodec c) {
    return c == rc::VideoCodec::H264 ? "h264" : c == rc::VideoCodec::AV1 ? "av1" : "hevc";
}

static ComPtr<IDXGIAdapter1> PickAdapter(DXGI_ADAPTER_DESC1& ad) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) == S_OK; ++i) {
        adapter->GetDesc1(&ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && ad.VendorId != 0x1414) return adapter;
        adapter = nullptr;
    }
    return nullptr;
}

static int RunPart1(const Options& opt) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;
    DXGI_ADAPTER_DESC1 ad{};
    auto adapter = PickAdapter(ad);
    if (!adapter) {
        std::printf("part1: no hardware adapter\n");
        return 1;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateDevice(adapter.Get(), &ctx);
    if (!dev) {
        std::printf("part1: device creation failed\n");
        return 1;
    }
    std::printf("part1: GPU %ls (vendor %04X)\n", ad.Description, ad.VendorId);

    const rc::VideoCodec codecs[] = {rc::VideoCodec::H264, rc::VideoCodec::HEVC, rc::VideoCodec::AV1};
    const int scales[] = {50, 67, 85, 100};
    const int eyeBases[] = {2144, 2688};

    // The grid, in the card's order; sharded deterministically.
    struct Cell { rc::VideoCodec codec; int scalePct, eyeBase, profileId; bool tenBit, aq; };
    std::vector<Cell> cells;
    for (auto codec : codecs)
        for (int scale : scales)
            for (int ten = 0; ten < 2; ++ten) {
                if (codec == rc::VideoCodec::H264 && ten) continue;  // A12 F2: tenBit x h264 degrades
                for (int aq = 0; aq < 2; ++aq)
                    for (int eye : eyeBases)
                        for (int prof : {0, 2}) cells.push_back({codec, scale, eye, prof, ten != 0, aq != 0});
            }
    const size_t total = cells.size();
    const size_t lo = opt.shardCount > 1 ? size_t(opt.shardIndex) * total / opt.shardCount : 0;
    const size_t hi = opt.shardCount > 1 ? size_t(opt.shardIndex + 1) * total / opt.shardCount : total;
    std::printf("part1: %zu combinations in the grid, this run: [%zu, %zu)\n", total, lo, hi);

    const std::string csvHeader =
        "codec,eyeBase,scalePct,tenBit,aq,profileId,encW,encH,frames,failures,encP50,encP95,keyMs,"
        "tierP50_0,tierP50_1,tierP50_2,tier0KB,tier1KB,tier2KB,avgKB,mbps72,initNs,slowFrames,paced,status";
    EnsureHeader(opt.out1, csvHeader);

    int exitCode = 0;
    bool sizesWritten = false;
    long long sessionsOpened = 0, sessionsClosed = 0;
    for (size_t ci = lo; ci < hi; ++ci) {
        const Cell& c = cells[ci];
        const float scale = c.scalePct / 100.0f;
        EncoderConfig ec;
        ec.codec = c.codec;
        // Production rule (stream_config.h + vr_stream.cpp): the side-by-side full
        // frame is 2 eyes wide, one eye high; encodeScale runs through the one
        // shared ScaledEncodeSize rule.
        ec.width = rc::ScaledEncodeSize(uint32_t(c.eyeBase) * 2, scale);
        ec.height = rc::ScaledEncodeSize(uint32_t(c.eyeBase), scale);
        ec.fps = float(opt.kFps);
        ec.bitrateKbps = opt.kBitrateKbps;
        ec.tenBit = c.tenBit;
        ec.adaptiveQuant = c.aq;
        ec.profile = rc::ProfileFromId(c.profileId);
        ec.cappedVbr = true;
        ec.fillMode = opt.kFillMode;
        ec.inputFormat = c.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

        Part1Row row;
        row.codec = CodecTag(c.codec);
        row.eyeBase = c.eyeBase;
        row.scalePct = c.scalePct;
        row.profileId = c.profileId;
        row.tenBit = c.tenBit;
        row.aq = c.aq;
        row.encW = ec.width;
        row.encH = ec.height;

        const int64_t initStart = rc::NowNs();
        auto encoder = CreateEncoder(dev.Get(), ad.VendorId, ec);
        row.initNs = rc::NowNs() - initStart;
        if (!encoder) {
            row.status = "initfail";
            std::printf("part1 [%zu/%zu] %s eye%d s%d ten%d aq%d p%d: init failed (unsupported on this GPU)\n", ci, total,
                        row.codec.c_str(), c.eyeBase, c.scalePct, int(c.tenBit), int(c.aq), c.profileId);
        } else {
            sessionsOpened++;
            // One NV12/P010 texture per combination, same bind flags as the
            // production converter's output (color_convert.cpp).
            D3D11_TEXTURE2D_DESC td{};
            td.Width = ec.width;
            td.Height = ec.height;
            td.Format = ec.inputFormat;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) {
                std::printf("part1: texture creation failed\n");
                return 1;
            }
            const UINT bpp = c.tenBit ? 2 : 1;
            const UINT pitch = ec.width * bpp;
            const UINT slice = pitch * ec.height + pitch * ec.height / 2;
            std::vector<uint8_t> upload;
            SynthFrame grad;
            MakeGradient(grad, int(ec.width), int(ec.height));
            SynthFrame work;

            std::vector<double> encMs;
            std::vector<double> tierBytes[3];
            std::vector<double> tierEncMs[3];
            int encodeFailures = 0;
            double keyMs = 0;
            // Production submits frames at the display rate; the encoder's rate
            // control behaves on the real frame interval, so the paced run is the
            // production-faithful measurement (--unpaced = raw back-to-back).
            const double periodNs = opt.paced1 ? 1e9 / double(opt.kFps) : 0.0;
            HANDLE paceTimer = periodNs > 0 ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                                     TIMER_ALL_ACCESS) : nullptr;
            int64_t nextFrameNs = rc::NowNs();
            int encodeSlow = 0;
            const int64_t tAll = rc::NowNs();
            for (int f = 0; f < opt.frames1; ++f) {
                if (f > 0 && periodNs > 0) {
                    nextFrameNs += int64_t(periodNs);
                    const int64_t wait = nextFrameNs - rc::NowNs();
                    if (wait > 0 && paceTimer) {
                        LARGE_INTEGER due;
                        due.QuadPart = -std::max<int64_t>(1, wait / 100);
                        SetWaitableTimer(paceTimer, &due, 0, nullptr, nullptr, FALSE);
                        WaitForSingleObject(paceTimer, INFINITE);
                    } else {
                        nextFrameNs = rc::NowNs();  // behind: the encode is slower than the period
                    }
                }
                const int tier = f == 0 ? 0 : f % 3;
                const int cycle = f == 0 ? 0 : f / 3;
                // light: the static gradient (identical every cycle -> the encoder
                // collapses it); medium: gradient + a square that moves each cycle;
                // heavy: fresh noise every frame. `grad` stays pristine.
                if (tier == 0) {
                    work = grad;
                } else if (tier == 1) {
                    work = grad;
                    const int cx = int(ec.width) / 2 + (cycle * 97) % (int(ec.width) / 4);
                    const int cy = int(ec.height) / 2 + (cycle * 61) % (int(ec.height) / 4);
                    DrawSquare(work, int(ec.width), int(ec.height), cx, cy, 60, 200);
                } else {
                    FillNoise(work, int(ec.width), int(ec.height),
                              0x9E3779B97F4A7C15ull ^ (uint64_t(f) * 0xBF58476D1CE4E5B9ull));
                }
                PackFrame(work, int(ec.width), int(ec.height), c.tenBit, upload);
                ctx->UpdateSubresource(tex.Get(), 0, nullptr, upload.data(), pitch, slice);

                EncodedFrame ef;
                const int64_t t0 = rc::NowNs();
                const bool ok = encoder->Encode(tex.Get(), f == 0, ef);
                const double ms = rc::NsToMs(rc::NowNs() - t0);
                if (periodNs > 0 && ms > periodNs / 1e6) encodeSlow++;
                if (!ok) {
                    encodeFailures++;
                    continue;
                }
                if (f == 0) keyMs = ms;
                else {
                    encMs.push_back(ms);
                    tierBytes[tier].push_back(double(ef.data.size()));
                    tierEncMs[tier].push_back(ms);
                }
            }
            if (paceTimer) CloseHandle(paceTimer);
            row.frames = opt.frames1;
            row.failures = encodeFailures;
            row.encP50 = Pct(encMs, 50);
            row.encP95 = Pct(encMs, 95);
            row.keyMs = keyMs;
            for (int t = 0; t < 3; ++t) row.tierP50[t] = Pct(tierEncMs[t], 50);
            row.slowFrames = encodeSlow;
            double sum = 0;
            size_t n = 0;
            for (int t = 0; t < 3; ++t) {
                double s = 0;
                for (double b : tierBytes[t]) s += b;
                row.tierKb[t] = tierBytes[t].empty() ? 0 : s / double(tierBytes[t].size()) / 1024.0;
                sum += s;
                n += tierBytes[t].size();
            }
            row.avgKb = n ? sum / double(n) / 1024.0 : 0;
            row.mbps72 = row.avgKb * 1024.0 * 8.0 * double(opt.kFps) / 1e6;
            const double wall = rc::NsToMs(rc::NowNs() - tAll);
            std::printf("part1 [%zu/%zu] %s eye%d s%d ten%d aq%d p%d %ux%u: enc %.2f/%.2f ms (key %.2f), tiers %.1f/%.1f/%.1f KB, %.1f Mbps, %d fails, %d slow, %.1f s\n",
                        ci, total, row.codec.c_str(), c.eyeBase, c.scalePct, int(c.tenBit), int(c.aq), c.profileId,
                        ec.width, ec.height, row.encP50, row.encP95, row.keyMs, row.tierKb[0], row.tierKb[1],
                        row.tierKb[2], row.mbps72, encodeFailures, row.slowFrames, wall / 1000.0);
            if (encodeFailures) {
                row.status = "encodefail";
                exitCode = 2;
            }

            // sizes.json: the baseline-provenance cell (the user's current
            // settings: HEVC, 2688 per eye, scale 100, ten-bit, AQ, profile 2).
            if (!sizesWritten && c.codec == rc::VideoCodec::HEVC && c.eyeBase == 2688 && c.scalePct == 100 &&
                c.tenBit && c.aq && c.profileId == 2 && !encodeFailures) {
                std::ofstream j(opt.sizesOut);
                j << "{\"provenance\":\"hevc,eye2688,scale100,tenBit,aqOn,profile2," << ec.width << "x" << ec.height
                  << "\",\"encW\":" << ec.width << ",\"encH\":" << ec.height
                  << ",\"lightBytes\":" << long long(row.tierKb[0] * 1024) << ",\"mediumBytes\":"
                  << long long(row.tierKb[1] * 1024) << ",\"heavyBytes\":" << long long(row.tierKb[2] * 1024) << "}\n";
                j.close();
                sizesWritten = true;
                std::printf("part1: sizes.json written from the baseline cell\n");
            }
            encoder.reset();  // session close (paired 1:1 with the open above)
            sessionsClosed++;
        }

        std::ofstream csv(opt.out1, std::ios::app);
        csv << row.codec << "," << row.eyeBase << "," << row.scalePct << "," << int(row.tenBit) << "," << int(row.aq) << ","
            << row.profileId << "," << row.encW << "," << row.encH << "," << row.frames << "," << row.failures << ","
            << row.encP50 << "," << row.encP95 << "," << row.keyMs << "," << row.tierP50[0] << "," << row.tierP50[1]
            << "," << row.tierP50[2] << "," << row.tierKb[0] << "," << row.tierKb[1] << "," << row.tierKb[2] << ","
            << row.avgKb << "," << row.mbps72 << "," << row.initNs << "," << row.slowFrames << "," << int(opt.paced1)
            << "," << row.status << "\n";
        csv.flush();
    }
    std::printf("part1: sessions opened %lld, closed %lld (1:1 pairing)\n", sessionsOpened, sessionsClosed);
    CoUninitialize();
    return exitCode;
}

// ---------------------------------------------------------------------------
// Part 2 - the real VideoPacketizer over a real loopback Connection pair
// ---------------------------------------------------------------------------

namespace {

// SessionLink shape (core_tests.cpp:3204) + the receiver bookkeeping the
// measurements need: every datagram's arrival time and size, every reassembled
// frame with its completion timestamp.
struct Loopback {
    rc::UdpSocket tx, rx;
    std::unique_ptr<rc::Connection> sendConn, recvConn;
    std::unique_ptr<rc::VideoReassembler> reasm;
    std::thread reader;
    std::atomic<bool> stop{false};

    std::mutex mutex;
    std::vector<std::pair<int64_t, size_t>> arrivals;  // (arrival ns, datagram bytes)
    std::vector<rc::ReceivedFrame> frames;

    bool Open() {
        if (!tx.Open(0) || !rx.Open(0)) return false;
        tx.SetBuffers(16 << 20, 1 << 20);
        rx.SetBuffers(1 << 20, 16 << 20);
        auto client = rc::crypto::KeyPair::Generate();
        auto server = rc::crypto::KeyPair::Generate();
        const std::string ctx = rc::SessionContext("cn", "sn", "a14");
        auto ck = rc::crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
        auto sk = rc::crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
        sendConn = std::make_unique<rc::Connection>(&tx, rc::NetAddress{0x7F000001, rx.LocalPort()}, 7, sk, true,
                                                    rc::kMaxDatagramLan);
        recvConn = std::make_unique<rc::Connection>(&rx, rc::NetAddress{0x7F000001, tx.LocalPort()}, 7, ck, false,
                                                    rc::kMaxDatagramLan);
        reasm = std::make_unique<rc::VideoReassembler>([this](rc::ReceivedFrame&& f) {
            std::lock_guard lock(mutex);
            frames.push_back(std::move(f));
        });
        recvConn->SetPacketHandler([this](rc::PacketType t, const uint8_t* d, size_t n) {
            if (t == rc::PacketType::Video) reasm->HandleFragment(d, n);
            else if (t == rc::PacketType::VideoParity) reasm->HandleParity(d, n);
        });
        reader = std::thread([this] {
            std::vector<uint8_t> buf(2048);
            rc::NetAddress from;
            while (!stop.load()) {
                const int n = rx.RecvFrom(buf.data(), buf.size(), from, 5);
                if (n <= 0) {
                    recvConn->Tick(rc::NowNs());
                    continue;
                }
                recvConn->HandleDatagram(buf.data(), size_t(n), from);
                std::lock_guard lock(mutex);
                arrivals.emplace_back(rc::NowNs(), size_t(n));
            }
        });
        return true;
    }
    void Close() {
        stop = true;
        if (reader.joinable()) reader.join();
        reasm.reset();
        recvConn.reset();
        sendConn.reset();
        tx.Close();
        rx.Close();
    }
};

}  // namespace

struct Part2Row {
    std::string status = "ok";  // ok | timeout
    double capMbps = 0;
    int fec = 0, fps = 0, tier = 0, mult = 1;
    int frames = 0;
    double avgMbps = 0, ceilLimit = 0;
    bool ceilOk = false;
    double latP50 = 0, latP95 = 0, latMax = 0;
    double blockP95 = 0, blockMax = 0;
    double jitterP50 = 0, jitterP95 = 0, intervalP50 = 0, intervalP95 = 0;
    uint32_t overruns = 0, sendFails = 0, framesLost = 0, framesComplete = 0;
    long long wireBytes = 0;
    double spanSec = 0;
};

static int RunPart2(const Options& opt) {
    rc::NetInit();

    double light = opt.lightKb * 1024.0, medium = opt.mediumKb * 1024.0, heavy = opt.heavyKb * 1024.0;
    std::string provenance = "fallback-arg";
    {
        std::string text;
        if (ReadFileText(opt.sizesIn, text)) {
            long long v = 0;
            if (JsonNumber(text, "lightBytes", v) && v > 0) {
                light = double(v);
                provenance = "part1";
            }
            if (JsonNumber(text, "mediumBytes", v) && v > 0) medium = double(v);
            if (JsonNumber(text, "heavyBytes", v) && v > 0) heavy = double(v);
            std::printf("part2: tier sizes from %s: light %.0f B, medium %.0f B, heavy %.0f B (%s)\n", opt.sizesIn.c_str(),
                        light, medium, heavy, provenance.c_str());
        } else {
            std::printf("part2: sizes file %s not found - fallback sizes in use\n", opt.sizesIn.c_str());
        }
    }

    const int caps[] = {20, 100, 150, 200, 500};
    const int fecs[] = {0, 3, 10, 20};
    const int fpss[] = {36, 51, 72};
    struct Cell { int capMbps, fec, fps, tier; };
    std::vector<Cell> cells;
    for (int cap : caps)
        for (int fec : fecs)
            for (int fps : fpss)
                for (int tier = 0; tier < 3; ++tier) cells.push_back({cap, fec, fps, tier});
    const size_t total = cells.size();
    const size_t lo = opt.shardCount > 1 ? size_t(opt.shardIndex) * total / opt.shardCount : 0;
    const size_t hi = opt.shardCount > 1 ? size_t(opt.shardIndex + 1) * total / opt.shardCount : total;
    std::printf("part2: %zu combinations in the grid, this run: [%zu, %zu)\n", total, lo, hi);

    const std::string csvHeader =
        "capMbps,fec,fps,tier,mult,frames,avgMbps,ceilLimit,ceilOk,latP50,latP95,latMax,blockP95,blockMax,"
        "jitterP50,jitterP95,intervalP50,intervalP95,overruns,sendFails,framesLost,framesComplete,wireBytes,spanSec,status";
    EnsureHeader(opt.out2, csvHeader);

    const double tierSizes[3] = {light, medium, heavy};
    int exitCode = 0;

    for (size_t ci = lo; ci < hi; ++ci) {
        const Cell& c = cells[ci];
        const int mult = c.fps == 36 ? 2 : 1;  // the card: 36 = half rate -> x2 per-frame payload
        const size_t bitBytes = size_t(tierSizes[c.tier]) * size_t(mult);
        const double periodNs = 1e9 / double(c.fps);
        const double ceilingMbps = double(c.capMbps) * 1.08;  // production: pacer = user ceiling x 1.08 (WP2a)

        Part2Row row;
        row.capMbps = c.capMbps;
        row.fec = c.fec;
        row.fps = c.fps;
        row.tier = c.tier;
        row.mult = mult;
        row.ceilLimit = ceilingMbps;

        Loopback net;
        if (!net.Open()) {
            std::printf("part2 [%zu/%zu]: loopback open failed\n", ci, total);
            return 1;
        }
        rc::VideoPacketizer pkt(net.sendConn.get());
        pkt.SetFecPercent(c.fec);
        pkt.SetPacingKbps(uint32_t(c.capMbps * 1000 * 1.08));
        pkt.SetPacingFrameBudgetNs(int64_t(periodNs));  // A10 burst window = the frame period

        // Real frame payload shape: [u16 metaSize][VrFrameMetadata][bitstream],
        // metadata = the production struct.
        rc::VrFrameMetadata meta{};
        meta.trackingTimestampNs = rc::NowNs();
        meta.ipd = 0.0686f;
        meta.eyeWidth = 2688;
        meta.eyeHeight = 2688;
        std::vector<uint8_t> bitstream(bitBytes);
        for (size_t i = 0; i < bitBytes; ++i) bitstream[i] = uint8_t(i * 131 + (i >> 8));

        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::vector<int64_t> submitNs(opt.frames2), completeNs(opt.frames2, 0);
        std::vector<double> blockMs(opt.frames2);
        const int64_t runStart = rc::NowNs();
        int64_t nextFrameNs = runStart;
        for (int f = 0; f < opt.frames2; ++f) {
            if (f > 0) {
                nextFrameNs += int64_t(periodNs);
                const int64_t wait = nextFrameNs - rc::NowNs();
                if (wait > 0) {
                    LARGE_INTEGER due;
                    due.QuadPart = -std::max<int64_t>(1, wait / 100);
                    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                        std::printf("part2: timer setup failed\n");
                        return 1;
                    }
                    WaitForSingleObject(timer, INFINITE);
                } else {
                    nextFrameNs = rc::NowNs();  // behind: the next frame starts now (no catch-up burst)
                }
            }
            submitNs[f] = rc::NowNs();
            const int64_t t0 = rc::NowNs();
            pkt.SendFrame(rc::kVrStreamId, rc::VideoCodec::HEVC, uint8_t(f == 0 ? rc::kVideoKeyframe : 0), uint32_t(f + 1),
                          &meta, sizeof(meta), bitstream.data(), bitBytes);
            blockMs[f] = rc::NsToMs(rc::NowNs() - t0);
        }
        CloseHandle(timer);
        // Drain: wait for every frame to complete (loopback: fast; saturated
        // combinations pace themselves, so the tail is short).
        const int64_t drainDeadline = rc::NowNs() + 30'000'000'000LL;
        while (rc::NowNs() < drainDeadline) {
            {
                std::lock_guard lock(net.mutex);
                row.framesComplete = uint32_t(net.frames.size());
            }
            if (row.framesComplete >= uint32_t(opt.frames2)) break;
            rc::SleepMs(5);
        }
        rc::ReassemblyStats reasmStats;
        {
            std::lock_guard lock(net.mutex);
            for (const auto& fr : net.frames)
                if (fr.frameIndex >= 1 && fr.frameIndex <= uint32_t(opt.frames2))
                    completeNs[fr.frameIndex - 1] = fr.completeNs;
            for (const auto& a : net.arrivals) row.wireBytes += int64_t(a.second);
            row.spanSec = net.arrivals.size() >= 2 ? double(net.arrivals.back().first - net.arrivals.front().first) / 1e9
                                                   : 0.0;
            reasmStats = net.reasm->TakeStats();
        }
        pkt.RequestStop();
        const uint32_t sendFails = pkt.SendFailures();
        const uint32_t overruns = pkt.BudgetOverruns();
        net.Close();

        // Metrics.
        std::vector<double> lat, jitter, interval, blocks;
        for (int f = 0; f < opt.frames2; ++f) {
            if (completeNs[f] == 0) continue;
            lat.push_back(rc::NsToMs(completeNs[f] - submitNs[f]));
            if (f > 0 && completeNs[f - 1] != 0) {
                interval.push_back(rc::NsToMs(completeNs[f] - completeNs[f - 1]));
                jitter.push_back(std::fabs(rc::NsToMs(completeNs[f] - completeNs[f - 1]) - periodNs / 1e6));
            }
        }
        for (double b : blockMs) blocks.push_back(b);
        row.frames = opt.frames2;
        row.latP50 = Pct(lat, 50);
        row.latP95 = Pct(lat, 95);
        row.latMax = Pct(lat, 100);
        row.jitterP50 = Pct(jitter, 50);
        row.jitterP95 = Pct(jitter, 95);
        row.intervalP50 = Pct(interval, 50);
        row.intervalP95 = Pct(interval, 95);
        row.blockP95 = Pct(blocks, 95);
        row.blockMax = Pct(blocks, 100);
        row.avgMbps = row.spanSec > 0 ? double(row.wireBytes) * 8.0 / row.spanSec / 1e6 : 0;
        row.ceilOk = row.avgMbps <= ceilingMbps * 1.05;  // the mean is billed at the rate; 5% = timer error
        row.overruns = overruns;
        row.sendFails = sendFails;
        row.framesLost = reasmStats.framesLost;
        if (row.framesComplete < uint32_t(opt.frames2) || reasmStats.framesLost > 0) {
            row.status = "timeout";
            exitCode = 2;
        }
        std::printf("part2 [%zu/%zu] cap%d fec%d fps%d t%d x%d: lat %.2f/%.2f/%.2f ms, block %.2f/%.2f, jitter %.2f/%.2f, %.1f Mbps (limit %.1f, %s), lost %u, complete %u/%u, overruns %u\n",
                    ci, total, c.capMbps, c.fec, c.fps, c.tier, mult, row.latP50, row.latP95, row.latMax, row.blockP95,
                    row.blockMax, row.jitterP50, row.jitterP95, row.avgMbps, ceilingMbps, row.ceilOk ? "ok" : "OVER",
                    reasmStats.framesLost, row.framesComplete, opt.frames2, overruns);

        std::ofstream csv(opt.out2, std::ios::app);
        csv << row.capMbps << "," << row.fec << "," << row.fps << "," << row.tier << "," << row.mult << "," << row.frames
            << "," << row.avgMbps << "," << row.ceilLimit << "," << int(row.ceilOk) << "," << row.latP50 << ","
            << row.latP95 << "," << row.latMax << "," << row.blockP95 << "," << row.blockMax << "," << row.jitterP50
            << "," << row.jitterP95 << "," << row.intervalP50 << "," << row.intervalP95 << "," << row.overruns << ","
            << row.sendFails << "," << row.framesLost << "," << row.framesComplete << "," << row.wireBytes << ","
            << row.spanSec << "," << row.status << "\n";
        csv.flush();
    }
    return exitCode;
}

// ---------------------------------------------------------------------------
// Leak probe - Part 1's gate: open/close the encoder session repeatedly and
// separate first-use driver initialization from growth across later sessions.
// ---------------------------------------------------------------------------

static int RunLeakProbe(const Options& opt) {
    if (opt.leakCycles < 8) {
        std::printf("leak-probe: --cycles must be at least 8\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    DXGI_ADAPTER_DESC1 ad{};
    auto adapter = PickAdapter(ad);
    if (!adapter) return 1;
    ComPtr<IDXGIAdapter3> adapter3;
    adapter.As(&adapter3);
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateDevice(adapter.Get(), &ctx);
    if (!dev) return 1;

    // The heaviest configuration in the grid.
    EncoderConfig ec;
    ec.codec = rc::VideoCodec::HEVC;
    ec.width = rc::ScaledEncodeSize(5376, 1.0f);
    ec.height = rc::ScaledEncodeSize(2688, 1.0f);
    ec.fps = float(opt.kFps);
    ec.bitrateKbps = opt.kBitrateKbps;
    ec.tenBit = true;
    ec.adaptiveQuant = true;
    ec.profile = rc::ProfileFromId(2);
    ec.cappedVbr = true;
    ec.fillMode = opt.kFillMode;
    ec.inputFormat = DXGI_FORMAT_P010;

    bool vramAvailable = true;
    const auto vramNow = [&] {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter3 || FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            vramAvailable = false;
            return 0.0;
        }
        return double(info.CurrentUsage) / (1024.0 * 1024.0);
    };
    const auto handlesNow = [&] {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return static_cast<long long>(n);
    };

    SynthFrame base;
    MakeGradient(base, int(ec.width), int(ec.height));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = ec.width;
    td.Height = ec.height;
    td.Format = DXGI_FORMAT_P010;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) return 1;
    const UINT pitch = ec.width * 2;
    const UINT slice = pitch * ec.height + pitch * ec.height / 2;
    std::vector<uint8_t> upload;
    PackFrame(base, int(ec.width), int(ec.height), true, upload);

    const int warmupCycles = std::min(8, opt.leakCycles / 2);
    const double vramStart = vramNow();
    const long long handlesStart = handlesNow();
    double vramWarm = vramStart;
    long long handlesWarm = handlesStart;
    int initFail = 0, encFail = 0;
    for (int i = 0; i < opt.leakCycles; ++i) {
        auto encoder = CreateEncoder(dev.Get(), ad.VendorId, ec);
        if (!encoder) {
            initFail++;
            continue;
        }
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, upload.data(), pitch, slice);
        EncodedFrame ef;
        if (!encoder->Encode(tex.Get(), i == 0, ef)) encFail++;
        encoder.reset();
        if (i + 1 == warmupCycles) {
            // DLL loading, the first upload and driver heaps are startup costs, not per-session leaks.
            vramWarm = vramNow();
            handlesWarm = handlesNow();
        }
    }
    const double vramAfter = vramNow();
    const long long handlesAfter = handlesNow();
    const double startupVram = vramWarm - vramStart;
    const double steadyVram = vramAfter - vramWarm;
    const long long startupHandles = handlesWarm - handlesStart;
    const long long steadyHandles = handlesAfter - handlesWarm;
    const bool ok = initFail == 0 && encFail == 0 && vramAvailable &&
                    steadyHandles <= 10 && std::fabs(steadyVram) <= 60.0;
    std::printf("leak-probe: cycles=%d warmup=%d measured=%d initFail=%d encFail=%d "
                "startup handles=%lld vram=%.1f MB, steady handles=%lld -> %lld (delta %lld), "
                "local vram %.1f -> %.1f MB (delta %.1f) verdict=%s\n",
                opt.leakCycles, warmupCycles, opt.leakCycles - warmupCycles, initFail, encFail,
                startupHandles, startupVram, handlesWarm, handlesAfter, steadyHandles,
                vramWarm, vramAfter, steadyVram, ok ? "OK" : "LEAK-SUSPECT");
    CoUninitialize();
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--part1") opt.part1 = true;
        else if (a == "--part2") opt.part2 = true;
        else if (a == "--leak-probe") opt.leakProbe = true;
        else if (a == "--frames") opt.frames2 = std::stoi(next());
        else if (a == "--frames1") opt.frames1 = std::stoi(next());
        else if (a == "--out1") opt.out1 = next();
        else if (a == "--out2") opt.out2 = next();
        else if (a == "--sizes-out") opt.sizesOut = next();
        else if (a == "--sizes-in") opt.sizesIn = next();
        else if (a == "--light") opt.lightKb = std::stod(next());
        else if (a == "--medium") opt.mediumKb = std::stod(next());
        else if (a == "--heavy") opt.heavyKb = std::stod(next());
        else if (a == "--shard") {
            const std::string v = next();
            const size_t slash = v.find('/');
            opt.shardIndex = std::stoi(v.substr(0, slash));
            opt.shardCount = std::stoi(v.substr(slash + 1));
        } else if (a == "--cycles") opt.leakCycles = std::stoi(next());
        else if (a == "--unpaced") opt.paced1 = false;
        else {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (!opt.part1 && !opt.part2 && !opt.leakProbe) {
        std::printf("a14-matrix: pass --part1, --part2 and/or --leak-probe\n");
        return 2;
    }
    int rc = 0;
    if (opt.leakProbe) rc |= RunLeakProbe(opt);
    if (opt.part1) rc |= RunPart1(opt);
    if (opt.part2) rc |= RunPart2(opt);
    std::printf("a14-matrix done, exit %d\n", rc);
    return rc;
}
