// encode-bench: measures the streamer's encoder on this GPU with a moving picture
// (the first monitor's current image, panned like a turning head) and checks loss
// recovery by reference invalidation.
//
//   encode-bench [--codec hevc|av1|h264] [--size 3664x1920] [--fps 90] [--mbps 150]
//                [--preset P1..P7] [--tuning ull|ll|hq] [--pace-mode off|follow|burst]
//                [--frame-budget-ms <ms>] [--net-async]
//                [--frames 300] [--desktop] [--no-split] [--foveated] [--ten-bit]
//                [--no-aq] [--two-pass] [--one-pass]
//                [--loss <frame>] [--out <bitstream>] [--yuv <nv12 file>] [--compare-foveated]
//                [--still] [--flat] [--cbr] [--profile 0|1|2] [--minqp <qp>]
//
// --still freezes the picture (a still menu) and --flat makes the synthetic source a
// smooth gradient instead of noise. --cbr restores the old constant-bitrate law,
// --profile picks latency/balanced/quality, and --minqp adds a QP floor. Together
// they give an A/B for "does a simple picture actually cost fewer bits now?".
//
// --loss F drops frames F..F+3 from --out (as if lost on the network) and answers the
// loss the way the streamer does: InvalidateAfter(F-1), then the next frame is a
// recovery frame. Decode --out with ffmpeg to check that it decodes cleanly.
// --yuv writes the encoder input, for PSNR comparisons with ffmpeg.
// --compare-foveated also encodes every frame with foveation into <out>.fov, from the
// identical input, so centre and edge quality can be compared with and without it.
//
// --net sends every encoded frame through the real VideoSender (submit + worker thread +
// pacing) into a loopback connection and timestamps the arriving datagrams, so pacing,
// burst size and submit-to-sent latency are measured end to end. --csv writes the raw
// arrival timeline.
//
// --stall-every <N:ms> stops submitting for M ms every N frames (a load hitch).
//
// --fb-every <ms> --fb-queue "0:3,5:60,20:3" --fb-loss "0:0,5:0.02,20:0" script the
// headset feedback that --net would otherwise never receive: queueDelayMs and the lost
// fragment count, linearly interpolated between the listed seconds. Without feedback the
// queue guard only ever sees "not measured" and the loss brake never fires; this is how
// both are driven offline (PLAN-ROBUST R1).
//
// --fec <pct> sets the parity percentage the sender uses (0 = off, the production
// default for LAN). --drop <pct> loses that percentage of datagrams in the loopback
// receiver, the way Wi-Fi does, so the FEC on/off A/B measures what actually reaches
// the assembler instead of an ideal link. Both are seeded/reproducible.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>

#include "../../pc/streamer/src/capture/desktop_capture.h"
#include "../../pc/streamer/src/capture/monitors.h"
#include "../../pc/streamer/src/encode/color_convert.h"
#include "../../pc/streamer/src/encode/encoder.h"
#include "../../pc/streamer/src/gpu.h"
#include "../../pc/streamer/src/stream/video_sender.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/socket.h"
#include "rc/stream_profile.h"

using namespace rcs;

// 一个到达的数据报：时刻、字节数，以及（能解析出时）属于哪一帧的第几个分片。
struct NetArrival {
    int64_t ns = 0;
    uint32_t bytes = 0;
    uint32_t frame = 0;
    uint16_t frag = 0;
    uint16_t fragCount = 0;
    bool video = false;
};

// "0:3,5:60,20:3" -> {(0,3),(5,60),(20,3)}; empty string -> empty list.
static std::vector<std::pair<double, double>> ParseTimeline(const std::string& text) {
    std::vector<std::pair<double, double>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t comma = text.find(',', pos);
        const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            try {
                out.emplace_back(std::stod(item.substr(0, colon)), std::stod(item.substr(colon + 1)));
            } catch (const std::exception&) {
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

// Value of a timeline at t seconds: hold before the first point, linear between, hold after
// the last. An empty timeline returns fallback.
static double TimelineAt(const std::vector<std::pair<double, double>>& tl, double t, double fallback) {
    if (tl.empty()) return fallback;
    if (t <= tl.front().first) return tl.front().second;
    for (size_t i = 1; i < tl.size(); ++i) {
        if (t <= tl[i].first) {
            const double span = tl[i].first - tl[i - 1].first;
            if (span <= 0) return tl[i].second;
            const double k = (t - tl[i - 1].first) / span;
            return tl[i - 1].second + (tl[i].second - tl[i - 1].second) * k;
        }
    }
    return tl.back().second;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    EncoderConfig ec;
    ec.codec = rc::VideoCodec::HEVC;
    ec.width = 3664;
    ec.height = 1920;
    ec.fps = 90;
    ec.bitrateKbps = 150000;
    int frames = 300;
    int lossAt = -1;
    int lossCount = 4;
    int fecPercent = 5;        // --fec: parity on the wire; 5 = the LAN default, 0 = off
    double dropPercent = 0.0;  // --drop: datagram loss injected in the loopback receiver
    std::string outPath, yuvPath;
    bool moving = false;           // every pixel changes, like a turning head in a game
    std::string imagePath;         // pan over this picture instead of the desktop
    bool compareFoveated = false;  // also encode every frame with foveation, into <out>.fov
    bool onePass = false;          // --desktop without the streamer's second pass
    bool swing = false;
    bool synthetic = false;
    bool still = false;            // freeze the pan: every frame is identical (a still menu)
    bool flat = false;             // smooth gradient instead of noise (easy still content)
    bool netBench = false;         // 走真实 VideoSender + 回环，测 pacing 与提交延迟
    // Synthetic feedback for --net. Without it the sender never sees a queue delay or a
    // lost fragment, so QueueGuard and the loss brake run blind: the real link needed a
    // headset to exercise them. Timelines are "sec:value" pairs, linearly interpolated.
    int fbEveryMs = 0;             // 0 = no feedback at all (the old behaviour)
    std::vector<std::pair<double, double>> fbQueue, fbLoss;
    // Frame-rate stalls: the game freezes for M ms every N frames (a load hitch). The rate
    // tracker clamps a single long interval to 3x the smoothed one, so this is the
    // regression test for "one 800 ms hitch must not blow the VBV open".
    int stallEvery = 0, stallMs = 0;
    std::string csvPath;
    std::string presetArg, tuningArg;  // --preset/--tuning as typed, for the override line
    std::string paceModeArg;           // --pace-mode off|follow|burst (default: follow)
    int64_t frameBudgetMs = 0;         // --frame-budget-ms, 0 = the sender's measured fps
    bool netAsync = false;             // --net-async: a capacity-1 send slot like the VR stream
    double paceFps = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--codec") {
            const std::string c = next();
            ec.codec = c == "av1" ? rc::VideoCodec::AV1 : c == "h264" ? rc::VideoCodec::H264 : rc::VideoCodec::HEVC;
        } else if (a == "--size") {
            std::sscanf(next().c_str(), "%ux%u", &ec.width, &ec.height);
        } else if (a == "--fps") {
            ec.fps = std::stof(next());
        } else if (a == "--mbps") {
            ec.bitrateKbps = uint32_t(std::stoi(next()) * 1000);
        } else if (a == "--frames") {
            frames = std::stoi(next());
        } else if (a == "--desktop") {
            ec.desktop = true;
        } else if (a == "--no-split") {
            ec.splitFrame = false;
        } else if (a == "--slices") {
            ec.sliceCount = std::stoi(next());
        } else if (a == "--no-aq") {
            ec.adaptiveQuant = false;
        } else if (a == "--two-pass") {
            ec.twoPass = true;
        } else if (a == "--one-pass") {
            onePass = true;
        } else if (a == "--swing") {
            swing = true;
        } else if (a == "--synthetic") {
            synthetic = true;
            moving = true;
        } else if (a == "--still") {
            still = true;  // no pan, no colour drift: the same picture every frame
        } else if (a == "--flat") {
            flat = true;
        } else if (a == "--cbr") {
            ec.cappedVbr = false;  // the old behaviour, for A/B comparisons
        } else if (a == "--profile") {
            ec.profile = rc::ProfileFromId(std::stoi(next()));
        } else if (a == "--minqp") {
            ec.vbrMinQp = std::stoi(next());  // -1 = profile default, 0 = off
        } else if (a == "--avgpct") {
            ec.vbrAveragePercent = uint32_t(std::stoi(next()));
        } else if (a == "--preset") {
            presetArg = next();
            std::string p = presetArg;
            if (p.size() == 2 && p[0] >= 'a' && p[0] <= 'z') p[0] = char(p[0] - 'a' + 'A');
            if (p.size() != 2 || p[0] != 'P' || p[1] < '1' || p[1] > '7') {
                std::printf("--preset wants P1..P7\n");
                return 2;
            }
            ec.preset = p[1] - '0';
        } else if (a == "--tuning") {
            tuningArg = next();
            if (tuningArg == "ull") ec.tuning = 0;        // built-in = ultra-low-latency here
            else if (tuningArg == "ll") ec.tuning = 1;
            else if (tuningArg == "hq") ec.tuning = 2;
            else {
                std::printf("--tuning wants ull, ll or hq\n");
                return 2;
            }
        } else if (a == "--pace") {
            paceFps = std::stod(next());
            if (!std::isfinite(paceFps) || paceFps <= 0) return 2;
        } else if (a == "--foveated") {
            ec.foveated = true;
        } else if (a == "--moving") {
            moving = true;
        } else if (a == "--image") {
            imagePath = next();
        } else if (a == "--compare-foveated") {
            compareFoveated = true;
        } else if (a == "--ten-bit") {
            ec.tenBit = true;
        } else if (a == "--loss") {
            lossAt = std::stoi(next());
        } else if (a == "--lost") {
            lossCount = std::stoi(next());
        } else if (a == "--fec") {
            fecPercent = std::stoi(next());
            if (fecPercent < 0 || fecPercent > 60) {
                std::printf("--fec wants 0..60\n");
                return 2;
            }
        } else if (a == "--drop") {
            dropPercent = std::stod(next());
            if (!std::isfinite(dropPercent) || dropPercent < 0.0 || dropPercent > 50.0) {
                std::printf("--drop wants 0..50 (percent)\n");
                return 2;
            }
        } else if (a == "--out") {
            outPath = next();
        } else if (a == "--net") {
            netBench = true;
        } else if (a == "--pace-mode") {
            paceModeArg = next();
        } else if (a == "--frame-budget-ms") {
            frameBudgetMs = std::stoll(next());
        } else if (a == "--net-async") {
            netAsync = true;
        } else if (a == "--fb-every") {
            fbEveryMs = std::stoi(next());
        } else if (a == "--fb-queue") {
            fbQueue = ParseTimeline(next());
        } else if (a == "--fb-loss") {
            fbLoss = ParseTimeline(next());
        } else if (a == "--stall-every") {
            const std::string v = next();
            const size_t colon = v.find(':');
            if (colon != std::string::npos) {
                stallEvery = std::stoi(v.substr(0, colon));
                stallMs = std::stoi(v.substr(colon + 1));
            }
        } else if (a == "--csv") {
            csvPath = next();
        } else if (a == "--yuv") {
            yuvPath = next();
        } else {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        }
    }
    // As the streamer does: desktop streams always take two passes.
    if (ec.desktop && !onePass) ec.twoPass = true;
    if (!presetArg.empty() || !tuningArg.empty())
        std::printf("encoder override: preset %s, tuning %s\n",
                    presetArg.empty() ? "built-in" : presetArg.c_str(),
                    tuningArg.empty() ? "built-in (ultra-low-latency here)" : tuningArg.c_str());
    if (onePass) ec.twoPass = false;
    if (still) moving = false;
    // --net 要按真实帧率喂帧，否则发送端一直阻塞、测不出「帧间隔内的节奏」。
    if (netBench && paceFps <= 0) paceFps = ec.fps;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return 1;
    const auto& mon = monitors[0];
    auto adapter = AdapterByLuid(mon.adapterLuid);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateDevice(adapter.Get(), &ctx);

    DesktopCapture cap;
    D3D11_TEXTURE2D_DESC sd{};
    ID3D11Texture2D* panSource = nullptr;
    ComPtr<ID3D11Texture2D> image;
    if (synthetic || !imagePath.empty()) {
        int iw = 2048, ih = 2048, comp = 0;
        std::vector<uint32_t> pixels;
        stbi_uc* loaded = nullptr;
        if (synthetic) {
            pixels.resize(size_t(iw) * ih);
            for (int y = 0; y < ih; ++y) {
                for (int x = 0; x < iw; ++x) {
                    if (flat) {
                        // A menu-like smooth gradient: almost nothing to encode.
                        const uint32_t r = uint32_t(x * 255 / iw), g = uint32_t(y * 255 / ih);
                        pixels[size_t(y) * iw + x] = 0xff000000u | (g << 16) | (r << 8) | 0x40u;
                    } else {
                        uint32_t v = uint32_t(x / 4) * 0x9e3779b9u ^ uint32_t(y / 4) * 0x85ebca6bu;
                        v = (v ^ (v >> 16)) * 0x7feb352du;
                        pixels[size_t(y) * iw + x] = 0xff000000u | ((v ^ (v >> 15)) & 0x00ffffffu);
                    }
                }
            }
        } else {
            loaded = stbi_load(imagePath.c_str(), &iw, &ih, &comp, 4);
            if (!loaded) {
                std::printf("can't read %s\n", imagePath.c_str());
                return 1;
            }
        }
        sd.Width = UINT(iw);
        sd.Height = UINT(ih);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{synthetic ? static_cast<const void*>(pixels.data()) : loaded, UINT(iw) * 4, 0};
        const HRESULT hr = dev->CreateTexture2D(&sd, &init, &image);
        stbi_image_free(loaded);
        if (FAILED(hr)) {
            std::printf("can't create the image texture\n");
            return 1;
        }
        panSource = image.Get();
    } else {
        if (!cap.Init(dev.Get(), mon, mon.width, mon.height)) {
            std::printf("capture init failed\n");
            return 1;
        }
        for (int i = 0; i < 40; i++)
            if (cap.Capture(50) == DesktopCapture::Result::NewFrame) break;
        cap.Recompose();
        cap.Output()->GetDesc(&sd);
        panSource = cap.Output();
    }
    D3D11_TEXTURE2D_DESC cd = sd;
    cd.Width = sd.Width * 3 / 4;
    cd.Height = sd.Height * 3 / 4;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.MiscFlags = 0;
    if (moving) cd.BindFlags |= D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> crop;
    dev->CreateTexture2D(&cd, nullptr, &crop);
    // --moving: draws the picture into the crop, rotating and zooming it a little.
    ComPtr<ID3D11RenderTargetView> cropRtv;
    ComPtr<ID3D11ShaderResourceView> panSrv;
    ComPtr<ID3D11VertexShader> moveVs;
    ComPtr<ID3D11PixelShader> movePs;
    ComPtr<ID3D11Buffer> moveCb;
    ComPtr<ID3D11SamplerState> moveSampler;
    if (moving) {
        static const char* kMove = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float2 shift; float angle; float zoom; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;
}
float4 ps(V i) : SV_Target {
    float2 p = (i.uv - 0.5) * zoom;
    float s = sin(angle), c = cos(angle);
    return src.Sample(samp, 0.5 + shift + float2(c * p.x - s * p.y, s * p.x + c * p.y));
}
)";
        auto vsb = CompileShader(kMove, "vs", "vs_5_0");
        auto psb = CompileShader(kMove, "ps", "ps_5_0");
        if (!vsb || !psb) return 1;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &moveVs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &movePs);
        dev->CreateRenderTargetView(crop.Get(), nullptr, &cropRtv);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = sd.Format;
        dev->CreateShaderResourceView(panSource, &srv, &panSrv);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &moveCb);
        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        dev->CreateSamplerState(&samp, &moveSampler);
    }

    ColorConverter conv;
    if (!conv.Init(dev.Get(), ec.width, ec.height, ec.tenBit)) return 1;
    ec.inputFormat = conv.Format();
    auto encoder = CreateEncoder(dev.Get(), ad.VendorId, ec);
    if (!encoder) return 1;
    // --compare-foveated: a second, foveated encoder gets exactly the same frames, so the
    // two outputs can be compared region by region (the panned desktop isn't repeatable).
    std::unique_ptr<VideoEncoder> fovEncoder;
    std::ofstream fovOut;
    if (compareFoveated) {
        EncoderConfig fc = ec;
        fc.foveated = true;
        fovEncoder = CreateEncoder(dev.Get(), ad.VendorId, fc);
        if (!fovEncoder) return 1;
        if (!outPath.empty()) fovOut.open(outPath + ".fov", std::ios::binary);
    }
    // Quest 3 field of view (left, right, up, down tangents per eye).
    const float fov[2][4] = {{1.376f, 0.839f, 0.966f, 1.428f}, {0.839f, 1.376f, 0.966f, 1.428f}};
    encoder->SetFoveation(fov);
    if (fovEncoder) fovEncoder->SetFoveation(fov);

    std::ofstream out, yuv;
    if (!outPath.empty()) out.open(outPath, std::ios::binary);
    if (!yuvPath.empty()) yuv.open(yuvPath, std::ios::binary);
    ComPtr<ID3D11Texture2D> staging;
    if (yuv) {
        D3D11_TEXTURE2D_DESC yd;
        conv.Output()->GetDesc(&yd);
        yd.Usage = D3D11_USAGE_STAGING;
        yd.BindFlags = 0;
        yd.MiscFlags = 0;
        yd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&yd, nullptr, &staging);
    }

    std::vector<double> times;
    std::vector<size_t> sizes;
    size_t keyBytes = 0, recoveryBytes = 0;
    bool recoveryWasKey = false;
    uint64_t lastGoodTs = 0;
    EncodedFrame ef;

    // --net：编码结果交给真正的 VideoSender（1 帧缓冲 + 发送线程 + 连续 pacing），
    // 回环收包线程给每个数据报打时间戳。
    std::unique_ptr<rc::UdpSocket> txSock, rxSock;
    std::unique_ptr<rc::Connection> sendConn, recvConn;
    std::unique_ptr<VideoSender> sender;
    // --net-async state: the capacity-1 send slot behind the sender thread.
    EncodedFrame asyncFrame;
    rc::VrFrameMetadata asyncMeta{};
    double asyncEncodeMs = 0;
    bool asyncSlotBusy = false;
    bool asyncStop = false;
    std::mutex asyncMutex;
    std::condition_variable asyncFilledCv, asyncDoneCv;
    std::thread asyncThread;
    std::vector<NetArrival> arrivals;
    std::atomic<bool> rxStop{false};
    std::atomic<size_t> rxCount{0};
    std::atomic<int> dropped{0};
    std::thread rxThread;
    std::vector<int64_t> submitNs, encStartNs, encEndNs;  // 与提交顺序一一对应
    std::vector<size_t> netFrameBytes;
    rc::VrFrameMetadata netMeta{};
    if (netBench) {
        rc::NetInit();
        txSock = std::make_unique<rc::UdpSocket>();
        rxSock = std::make_unique<rc::UdpSocket>();
        if (!txSock->Open(0) || !rxSock->Open(0)) {
            std::printf("can't open the loopback sockets\n");
            return 1;
        }
        txSock->SetBuffers(16 << 20, 1 << 20);
        rxSock->SetBuffers(1 << 20, 16 << 20);
        auto client = rc::crypto::KeyPair::Generate();
        auto server = rc::crypto::KeyPair::Generate();
        const std::string ctx = rc::SessionContext("cn", "sn", "bench");
        auto ck = rc::crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey,
                                                server.publicKey, ctx);
        auto sk = rc::crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey,
                                                server.publicKey, ctx);
        const rc::NetAddress txAddr{0x7F000001, txSock->LocalPort()};
        const rc::NetAddress rxAddr{0x7F000001, rxSock->LocalPort()};
        sendConn = std::make_unique<rc::Connection>(txSock.get(), rxAddr, 7, sk, true, rc::kMaxDatagramLan);
        recvConn = std::make_unique<rc::Connection>(rxSock.get(), txAddr, 7, ck, false, rc::kMaxDatagramLan);
        arrivals.resize(400'000);
        recvConn->SetPacketHandler([&](rc::PacketType t, const uint8_t* p, size_t n) {
            const int64_t now = rc::NowNs();
            NetArrival a;
            a.ns = now;
            a.bytes = uint32_t(n);
            if (t == rc::PacketType::Video && n >= sizeof(rc::VideoFragmentHeader)) {
                rc::VideoFragmentHeader h;
                std::memcpy(&h, p, sizeof(h));
                a.video = true;
                a.frame = h.frameIndex;
                a.frag = h.fragmentIndex;
                a.fragCount = h.fragmentCount;
            } else if (t == rc::PacketType::VideoParity && n >= sizeof(rc::VideoParityHeader)) {
                rc::VideoParityHeader h;
                std::memcpy(&h, p, sizeof(h));
                a.video = true;
                a.frame = h.frameIndex;
                a.frag = 0xFFFF;  // 校验包，不属于某个分片
                a.fragCount = h.fragmentCount;
            }
            const size_t i = rxCount.fetch_add(1);
            if (i < arrivals.size()) arrivals[i] = a;
        });
        rxThread = std::thread([&] {
            std::vector<uint8_t> buf(2048);
            rc::NetAddress from;
            std::mt19937 rng(20261116u);  // seeded: the A/B below is reproducible
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            while (!rxStop.load()) {
                const int n = rxSock->RecvFrom(buf.data(), buf.size(), from, 20);
                if (n <= 0) {
                    recvConn->Tick(rc::NowNs());
                    continue;
                }
                // --drop: lose the datagram the way Wi-Fi does, BEFORE the receiver
                // assembles it. What survives is what the headset would have seen.
                if (dropPercent > 0.0 && unit(rng) < dropPercent / 100.0) {
                    dropped.fetch_add(1);
                    continue;
                }
                recvConn->HandleDatagram(buf.data(), size_t(n), from);
            }
        });
        sender = std::make_unique<VideoSender>(sendConn.get(), rc::kVrStreamId, ec.codec, ec.bitrateKbps,
                                               fecPercent /* --fec; 5 = the LAN default */, false);
        // Default: the legacy follow mode, so the existing 51-group matrix stays
        // reproducible; --pace-mode switches the behavior on explicitly.
        int mode = 2;
        if (paceModeArg == "off") mode = 1;
        else if (paceModeArg == "burst") mode = 3;
        else if (!paceModeArg.empty() && paceModeArg != "follow") {
            std::printf("--pace-mode wants off, follow or burst\n");
            return 2;
        }
        sender->SetPacingMode(mode, frameBudgetMs * 1'000'000LL);
        std::printf("net: %u kbps target, pace mode %s, frame budget %lld ms, %.0f fps frame pacing\n",
                    ec.bitrateKbps, mode == 1 ? "off" : mode == 3 ? "burst" : "follow",
                    (long long)frameBudgetMs, paceFps);
        if (netAsync) {
            // Same shape as VrStream::RunSender: take the frame out of the capacity-1
            // slot, free the slot so the encoder can overlap, then send.
            asyncThread = std::thread([&] {
                for (;;) {
                    EncodedFrame frame;
                    rc::VrFrameMetadata meta{};
                    double encodeMs = 0;
                    {
                        std::unique_lock<std::mutex> lock(asyncMutex);
                        asyncFilledCv.wait(lock, [&] { return asyncSlotBusy || asyncStop; });
                        if (!asyncSlotBusy) break;
                        frame = std::move(asyncFrame);
                        meta = asyncMeta;
                        encodeMs = asyncEncodeMs;
                        asyncSlotBusy = false;
                    }
                    asyncDoneCv.notify_one();
                    sender->Send(frame, &meta, sizeof(meta), 0, encodeMs);
                }
            });
        }
    }

    const UINT panW = sd.Width - cd.Width, panH = sd.Height - cd.Height;
    const bool paced = swing || paceFps > 0;
    HANDLE timer = paced ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS) : nullptr;
    if (paced && !timer) return 1;
    int64_t nextFrameNs = rc::NowNs(), swingStartNs = nextFrameNs, windowStartNs = nextFrameNs;
    bool swingHigh = false;
    size_t windowBytes = 0, windowFrames = 0;
    double maxWindowMbps = 0;
    const int64_t benchStartNs = rc::NowNs();
    int64_t nextFbNs = benchStartNs;
    uint32_t appliedKbps = ec.bitrateKbps;
    for (int i = 0; i < frames; i++) {
        if (stallEvery > 0 && i > 0 && i % stallEvery == 0 && stallMs > 0) {
            rc::SleepMs(stallMs);   // the game stopped submitting: a load hitch, not a frame
        }
        if (paced) {
            const int64_t nowNs = rc::NowNs();
            if (nowNs - swingStartNs > 400'000'000LL) {
                swingHigh = !swingHigh;
                swingStartNs = nowNs;
            }
            const double rate = swing ? (swingHigh ? 60.0 : 15.0) : paceFps;
            nextFrameNs += int64_t(1e9 / rate);
            const int64_t waitNs = nextFrameNs - nowNs;
            if (waitNs > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -std::max<int64_t>(1, waitNs / 100);
                if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return 1;
                if (WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) return 1;
            } else {
                nextFrameNs = nowNs;
            }
        }
        // Smooth pan: a few pixels per frame, like a slowly turning head.
        const double t = i / double(ec.fps);
        const UINT x = still ? 0u : UINT((0.5 + 0.5 * std::sin(t * 1.3)) * panW);
        const UINT y = still ? 0u : UINT((0.5 + 0.5 * std::sin(t * 0.7)) * panH);
        D3D11_BOX box{x, y, 0, x + cd.Width, y + cd.Height, 1};
        if (moving) {
            const float c[4] = {float(std::sin(t * 1.7) * 0.15), float(std::sin(t * 1.1) * 0.1), float(std::sin(t * 0.9) * 0.05),
                                1.0f + float(std::sin(t * 1.3)) * 0.04f};
            ctx->UpdateSubresource(moveCb.Get(), 0, nullptr, c, 0, 0);
            ctx->OMSetRenderTargets(1, cropRtv.GetAddressOf(), nullptr);
            D3D11_VIEWPORT mv{0, 0, float(cd.Width), float(cd.Height), 0, 1};
            ctx->RSSetViewports(1, &mv);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(moveVs.Get(), nullptr, 0);
            ctx->PSSetShader(movePs.Get(), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, panSrv.GetAddressOf());
            ctx->PSSetSamplers(0, 1, moveSampler.GetAddressOf());
            ctx->PSSetConstantBuffers(0, 1, moveCb.GetAddressOf());
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* noSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &noSrv);
            ID3D11RenderTargetView* noRtv = nullptr;
            ctx->OMSetRenderTargets(1, &noRtv, nullptr);
        } else {
            ctx->CopySubresourceRegion(crop.Get(), 0, 0, 0, 0, panSource, 0, &box);
        }
        conv.Convert(ctx.Get(), crop.Get());
        if (yuv) {
            ctx->CopyResource(staging.Get(), conv.Output());
            D3D11_MAPPED_SUBRESOURCE m{};
            ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
            const UINT bpp = ec.tenBit ? 2 : 1;
            for (UINT r = 0; r < ec.height * 3 / 2; r++)
                yuv.write(static_cast<const char*>(m.pData) + size_t(r) * m.RowPitch, ec.width * bpp);
            ctx->Unmap(staging.Get(), 0);
        }

        const bool lost = lossAt >= 0 && i >= lossAt && i < lossAt + lossCount;
        const bool recover = lossAt >= 0 && i == lossAt + lossCount;
        bool key = i == 0;
        if (recover && !encoder->InvalidateAfter(lastGoodTs)) {
            std::printf("InvalidateAfter refused: forcing a keyframe\n");
            key = true;
        }
        const int64_t t0 = rc::NowNs();
        if (!encoder->Encode(conv.Output(), key, ef)) {
            std::printf("encode failed at frame %d\n", i);
            return 1;
        }
        times.push_back(rc::NsToMs(rc::NowNs() - t0));
        sizes.push_back(ef.data.size());
        if (paced) {
            windowBytes += ef.data.size();
            windowFrames++;
            const int64_t nowNs = rc::NowNs();
            const double elapsed = double(nowNs - windowStartNs) / 1e9;
            if (elapsed >= 1.0) {
                const double mbps = double(windowBytes) * 8 / 1e6 / elapsed;
                maxWindowMbps = std::max(maxWindowMbps, mbps);
                std::printf("  window %.1f Mbps, %.0f fps\n", mbps, windowFrames / elapsed);
                windowBytes = 0; windowFrames = 0; windowStartNs = nowNs;
            }
        }
        if (ef.keyframe && i == 0) keyBytes = ef.data.size();
        if (recover) {
            recoveryBytes = ef.data.size();
            recoveryWasKey = ef.keyframe;
        }
        if (lossAt < 0 || i < lossAt) lastGoodTs = ef.timestamp;
        if (out && !lost) out.write(reinterpret_cast<const char*>(ef.data.data()), std::streamsize(ef.data.size()));
        if (fovEncoder) {
            EncodedFrame ff;
            if (!fovEncoder->Encode(conv.Output(), key, ff)) return 1;
            if (fovOut) fovOut.write(reinterpret_cast<const char*>(ff.data.data()), std::streamsize(ff.data.size()));
        }
        if (sender) {
            // Synthetic headset feedback: the queue guard and the loss brake only move on
            // what the headset reports, and a loopback has no headset. Feeding a scripted
            // timeline is the only way to drive them without a device (see PLAN-ROBUST R1).
            if (fbEveryMs > 0) {
                const int64_t fbNow = rc::NowNs();
                if (fbNow >= nextFbNs) {
                    const double sec = double(fbNow - benchStartNs) / 1e9;
                    rc::VideoFeedbackPayload fb{};
                    fb.streamId = rc::kVrStreamId;
                    fb.lastCompleteFrame = uint32_t(i);
                    fb.lastGoodFrame = uint32_t(i);
                    fb.queueDelayMs = float(TimelineAt(fbQueue, sec, -1.0));
                    const double loss = std::clamp(TimelineAt(fbLoss, sec, 0.0), 0.0, 0.99);
                    fb.fragmentsReceived = 1000;
                    fb.fragmentsLost = uint32_t(loss * 1000.0 / (1.0 - loss));
                    sender->OnFeedback(fb);
                    nextFbNs = fbNow + int64_t(fbEveryMs) * 1'000'000LL;
                }
            }
            // The async path mirrors production: the guard's cap feeds the encoder
            // target, so the user's ceiling remains an average-rate ceiling. The
            // synchronous --net path keeps its historical behavior unchanged.
            if (netAsync) {
                const uint32_t target = sender->TargetBitrateKbps();
                if (target && target != appliedKbps) {
                    encoder->SetBitrate(target);
                    appliedKbps = target;
                }
            }
            // --net-async: the VR stream's shape - the encode thread fills a capacity-1
            // slot and a sender thread drains it, so submit->sent and overlap carry the
            // production meaning. Without it: the synchronous Send.
            netFrameBytes.push_back(sizes.back());
            encStartNs.push_back(t0);
            encEndNs.push_back(rc::NowNs());
            submitNs.push_back(encEndNs.back());
            if (netAsync) {
                std::unique_lock<std::mutex> slotLock(asyncMutex);
                asyncDoneCv.wait(slotLock, [&] { return !asyncSlotBusy; });   // the sender frees the slot on this cv
                asyncFrame = std::move(ef);
                asyncMeta = netMeta;
                asyncEncodeMs = rc::NsToMs(rc::NowNs() - t0);
                asyncSlotBusy = true;
                asyncFilledCv.notify_one();   // wake the sender: nothing woke it before (deadlock)
            } else {
                sender->Send(ef, &netMeta, sizeof(netMeta), 0, rc::NsToMs(rc::NowNs() - t0));
            }
        }
    }

    const double benchSec = double(rc::NowNs() - benchStartNs) / 1e9;
    const double benchFps = benchSec > 0 ? double(sizes.size()) / benchSec : double(ec.fps);
    if (timer) CloseHandle(timer);

    // --net-async：让发送线程把最后一帧发完再汇总（先于 sender 析构）。
    if (netAsync) {
        {
            std::lock_guard<std::mutex> slotLock(asyncMutex);
            asyncStop = true;
        }
        asyncFilledCv.notify_all();
        asyncDoneCv.notify_all();
        if (asyncThread.joinable()) asyncThread.join();
    }
    // --net：把回环上量到的时间线汇总出来。
    if (sender) {
        // 析构函数里的 RequestStop 负责收尾；最后一批包靠下面的等待落地。
        rc::SleepMs(120);  // 让最后一批包落地
        rxStop = true;
        if (rxThread.joinable()) rxThread.join();

        const size_t n = std::min(rxCount.load(), arrivals.size());
        std::vector<const NetArrival*> v;
        for (size_t k = 0; k < n; k++)
            if (arrivals[k].video) v.push_back(&arrivals[k]);

        if (!csvPath.empty()) {
            std::ofstream csv(csvPath);
            csv << "arrival_ns,bytes,frame_index,fragment_index\n";
            for (auto* a : v) csv << a->ns << "," << a->bytes << "," << a->frame << "," << a->frag << "\n";
        }
        if (!v.empty()) {
            const int64_t spanNs = v.back()->ns - v.front()->ns;
            uint64_t totalBytes = 0;
            for (auto* a : v) totalBytes += a->bytes;
            std::vector<double> gaps;
            for (size_t k = 1; k < v.size(); k++) gaps.push_back(rc::NsToMs(v[k]->ns - v[k - 1]->ns));
            std::vector<double> sortedGaps = gaps;
            std::sort(sortedGaps.begin(), sortedGaps.end());
            // 单批突发：间隔 > 0.15 ms 算批边界。
            std::vector<size_t> batches;
            size_t cur = v[0]->bytes;
            for (size_t k = 1; k < v.size(); k++) {
                if (v[k]->ns - v[k - 1]->ns <= 150'000) {
                    cur += v[k]->bytes;
                } else {
                    batches.push_back(cur);
                    cur = v[k]->bytes;
                }
            }
            batches.push_back(cur);
            std::sort(batches.begin(), batches.end());
            // 每帧的发送完成时刻：最后一个分片到达的时间。
            std::vector<int64_t> frameEnd(netFrameBytes.size() + 2, 0);
            for (auto* a : v) {
                if (a->frag != 0xFFFF && a->frag + 1 == a->fragCount && a->frame < frameEnd.size())
                    frameEnd[a->frame] = std::max(frameEnd[a->frame], a->ns);
            }
            // The rate the sender actually paces at: the user's ceiling x 1.08 (WP2a),
            // raised inside the frame budget when burst mode asks for it.
            const double pacingKbps = double(ec.bitrateKbps) * 1.08;
            const double budgetNs = double(frameBudgetMs) * 1e6;
            std::vector<double> lat, theory;
            int overlapped = 0, pairs = 0;
            for (size_t i = 0; i < submitNs.size(); i++) {
                const uint32_t fi = uint32_t(i) + 1;
                if (fi < frameEnd.size() && frameEnd[fi] > 0) {
                    lat.push_back(rc::NsToMs(frameEnd[fi] - submitNs[i]));
                    double rate = pacingKbps;
                    if (paceModeArg == "burst" && budgetNs > 0) {
                        const double frameBits = double(netFrameBytes[i]) * 8.0;
                        const double budgetKbps = frameBits * 1e6 / budgetNs;
                        rate = std::max(rate, std::min(800000.0, budgetKbps));
                    }
                    theory.push_back(double(netFrameBytes[i]) * 8.0 / rate);
                }
                if (i > 0 && fi - 1 < frameEnd.size() && frameEnd[fi - 1] > 0) {
                    pairs++;
                    if (encStartNs[i] < frameEnd[fi - 1]) overlapped++;
                }
            }
            std::sort(lat.begin(), lat.end());
            double latSum = 0, theorySum = 0;
            for (double x : lat) latSum += x;
            for (double x : theory) theorySum += x;
            std::printf("net: %zu datagrams over %.2f s, %.1f Mbps, packet max %u B\n", v.size(),
                        double(spanNs) / 1e9, double(totalBytes) * 8 / 1e6 / (double(spanNs) / 1e9),
                        [&] { uint32_t m = 0; for (auto* a : v) m = std::max(m, a->bytes); return m; }());
            if (!sortedGaps.empty())
                std::printf("net: gap p50 %.2f ms p99 %.2f ms max %.2f ms; batch p50 %.1f KB p90 %.1f KB max %.1f KB\n",
                            sortedGaps[sortedGaps.size() / 2], sortedGaps[sortedGaps.size() * 99 / 100],
                            sortedGaps.back(), double(batches[batches.size() / 2]) / 1024,
                            double(batches[batches.size() * 9 / 10]) / 1024, double(batches.back()) / 1024);
            if (!lat.empty())
                std::printf("net: submit->sent avg %.2f ms p99 %.2f ms; theoretical avg %.2f ms (ratio %.2f)\n",
                            latSum / double(lat.size()), lat[lat.size() * 99 / 100],
                            theorySum / double(theory.size()), latSum / theorySum);
            if (pairs)
                std::printf("net: encode/send overlap %d of %d frames (%.0f%%)\n", overlapped, pairs,
                            100.0 * overlapped / pairs);
            // 丢包下的成帧率：一个帧少收一个分片，头显就只能等恢复帧（真机每次约 35-50 ms）。
            // 校验包数量决定这一帧能否被 FEC 救回；这条线就是「FEC 该不该开」的量化依据。
            if (dropPercent > 0.0) {
                std::map<uint32_t, std::pair<uint32_t, uint32_t>> frames;  // frame -> {fragCount, received}
                std::map<uint32_t, uint32_t> parity;                       // frame -> parity datagrams
                for (auto* a : v) {
                    if (a->frag == 0xFFFF) parity[a->frame] += 1;
                    else {
                        auto& f = frames[a->frame];
                        f.first = std::max(f.first, uint32_t(a->fragCount));
                        f.second += 1;
                    }
                }
                uint32_t complete = 0, damaged = 0, covered = 0;
                for (const auto& [fi, f] : frames) {
                    if (f.second >= f.first) complete++;
                    else {
                        damaged++;
                        if (f.second + parity[fi] >= f.first) covered++;
                    }
                }
                std::printf("net: drop %.1f%% (%d datagrams): %u/%u frames complete, %u damaged, "
                            "%u covered by FEC, %d%% parity\n",
                            dropPercent, dropped.load(), complete, uint32_t(frames.size()), damaged,
                            covered, fecPercent);
            }
        }
    }

    // Skip the first frames (session warm-up, keyframe) for the averages.
    const size_t skip = std::min<size_t>(10, times.size() / 2);
    std::vector<double> t(times.begin() + skip, times.end());
    std::sort(t.begin(), t.end());
    double sum = 0;
    for (double v : t) sum += v;
    size_t bytes = 0, maxBytes = 0;
    for (size_t i = skip; i < sizes.size(); i++) {
        bytes += sizes[i];
        maxBytes = std::max(maxBytes, sizes[i]);
    }
    const double avgBytes = double(bytes) / double(sizes.size() - skip);
    const auto pq = rc::ProfileParams(ec.profile);
    const auto budget = ec.cappedVbr
                            ? rc::ComputeCappedVbr(ec.bitrateKbps, uint32_t(ec.fps),
                                                   ec.vbrAveragePercent ? ec.vbrAveragePercent : pq.vbrAveragePercent,
                                                   pq.vbvFramesPercent)
                            : rc::ComputeCappedVbr(ec.bitrateKbps, uint32_t(ec.fps), 100, 150);
    const double wireMbps = avgBytes * 8 * benchFps / 1e6;
    std::printf("%s %ux%u @%.0f, %u Mbps%s%s%s\n", encoder->Name(), ec.width, ec.height, ec.fps,
                ec.bitrateKbps / 1000, ec.desktop ? ", desktop" : "", ec.foveated ? ", foveated" : "",
                ec.splitFrame ? "" : ", no split");
    const int effectiveMinQp = !ec.cappedVbr ? 0 : ec.vbrMinQp < 0 ? int(rc::ProfileParams(ec.profile).minQp)
                                                                    : ec.vbrMinQp;
    const std::string qpText = effectiveMinQp > 0 ? std::to_string(effectiveMinQp) : std::string("off");
    std::printf("rc: %s, profile %d, avg target %u Mbps, max %u Mbps, VBV %.2f Mbit, QP floor %s\n",
                ec.cappedVbr ? "capped VBR" : "CBR", int(ec.profile), budget.averageBitrate / 1000000,
                budget.maxBitrate / 1000000, double(budget.vbvBufferSize) / 1e6, qpText.c_str());
    std::printf("content: %s, %s\n", still ? "still" : "moving", flat ? "flat gradient" : synthetic ? "noise" : "capture");
    std::printf("wire: %.1f Mbps at %.0f fps (measured) = %.0f%% of the ceiling\n", wireMbps, benchFps,
                100.0 * wireMbps / double(ec.bitrateKbps / 1000.0));
    std::printf("encode ms: avg %.2f  p50 %.2f  p95 %.2f  max %.2f\n", sum / double(t.size()), t[t.size() / 2],
                t[t.size() * 95 / 100], t.back());
    std::printf("frame KB: avg %.1f  max %.1f  keyframe %.1f\n", avgBytes / 1024, double(maxBytes) / 1024,
                double(keyBytes) / 1024);
    if (lossAt >= 0)
        std::printf("recovery frame: %.1f KB (%s, %.1fx average)\n", double(recoveryBytes) / 1024,
                    recoveryWasKey ? "intra" : "inter", double(recoveryBytes) / avgBytes);
    if (paced)
        std::printf("paced: peak 1s throughput %.1f Mbps against %u Mbps setting\n", maxWindowMbps,
                    ec.bitrateKbps / 1000);
    return 0;
}
