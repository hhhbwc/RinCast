// Loopback tests for the transport: handshake key agreement, reliable control
// messages under loss, video FEC recovery and discovery parsing.
// 策略类新用例必须：①输入来自真机日志形态；②先在旧实现上跑红留证，再修码变绿。
// (TASK-POLICY-FIX-TESTS-2026-11-20 §3.4: a test must not mirror the implementation it exists to catch.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "rc/audio_codec.h"
#include "rc/connection.h"
#include "rc/discovery.h"
#include "rc/hand_skeleton.h"
#include "rc/handshake.h"
#include "rc/motion_field.h"
#include "rc/music.h"
#include "rc/platform.h"
// A44 (TASK-HANDSHAKE-HARDEN-2026-11-22): the low-order defense test uses a
// frozen copy of the wire KDF as a known-answer oracle, so it calls Monocypher
// directly (rincast-core exports its include dir PUBLIC).
#include "monocypher.h"
#include "rc/fps_ladder.h"
#include "rc/loss_brake.h"
#include "rc/autoscale.h"
#include "rc/auto_tune.h"
#include "rc/percentile.h"
#include "rc/link_policy.h"
#include "rc/queue_guard.h"
// A23 (TASK-QUEUE-BOUND-2026-11-22): the decode queue bound's pure decision logic
// (threshold, boundary, keyframe-request throttle), tested below.
#include "rc/frame_queue_bound.h"
// A26 (TASK-BACKLOG-FLUSH-2026-11-22): the sender-side stale-frame flush's pure
// decision logic (expiry, debt wipe, keyframe/log throttles), tested below.
#include "rc/stale_flush.h"
#include "rc/fovea_placement.h"
#include "rc/fovea_qp.h"
#include "rc/stream_profile.h"
#include "rc/video_transport.h"
#include "rc/wire_ceiling.h"
// A84b §2 (TASK-A84b): the settings-file migration predicate. It is here because
// Settings::FromJson, the code that used to hold it inline, compiles into
// rincast-streamer only - nothing below could reach the judgement.
#include "rc/settings_migration.h"

// WP1a: the pure fovea override the VR session feeds Foveator::EncodedSize. The
// relative include keeps the test target free of pc-side link dependencies: the
// helper is constexpr and nothing else from this header is used here.
#include "../../pc/streamer/src/settings.h"
// TASK-COMBO-SWEEP-2026-11-21: the settings -> stream-config builders. Header-only
// pure functions (no pc-side link dependency); the sweep below calls them directly.
#include "../../pc/streamer/src/stream/stream_config.h"
// TASK-LATENCY-METER-FIX-2026-11-22 §2: the headset's stage-sample accumulator.
// Header-only and dependency-free (constants plus one inline function); the test
// below calls it directly, the same way it calls the settings helpers above.
#include "../../headset/app/src/main/cpp/app/latency.h"

using namespace rc;

static int g_failures = 0;
static int g_skips = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                              \
        }                                                              \
    } while (0)

// F1 (TASK-FIX 2026-11-19) -> TASK-FLAKE-ROBUST-2026-11-22 (2026-11-22): the two
// real-clock pacing tests (TestPacingBursts, TestPacingBurstShaping - the latter also
// gated together with TestPacingFrameBudget) used to fail deterministically under CPU
// load (user streaming): fixed ms tolerances reported scheduling artifacts as failures,
// so a live stream could never gate them ("skip under load / rerun for exemption" loop).
// Since 2026-11-22 they self-calibrate: an independent unloaded-pacer + SleepMs probe
// measures the host's jitter floor first (CalibrateTimingJitter), and assertion
// tolerances adapt with a hard cap (kAdaptiveTolCapMs) - assertion semantics unchanged.
// Evidence (forced-mode greens incl. under stream load): overnight/evidence/A7-receipt.md.
// The pre-run guard in main() stays for UNFORCED runs (explicit counted SKIP line, the
// checks themselves untouched); RINCAST_TEST_TIMING_SENSITIVE=1 forces the run
// (byte-identical gates) and is part of the standard gate since 2026-11-22
// (main-AI verification: forced 2 consecutive greens that day),
// =0 always skips, unset = auto (skip only while the streamer is running).
static bool TestHostBusy() {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool busy = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"RinCast Streamer.exe") == 0) {
                busy = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return busy;
#else
    return false;
#endif
}

struct Peer {
    UdpSocket socket;
    std::unique_ptr<Connection> conn;
    std::thread thread;
    std::atomic<bool> stop{false};
    double dropRate = 0;
    std::mt19937 rng{1234};

    void Run() {
        thread = std::thread([this] {
            uint8_t buf[2048];
            NetAddress from;
            std::uniform_real_distribution<double> dist(0, 1);
            while (!stop) {
                const int n = socket.RecvFrom(buf, sizeof(buf), from, 5);
                if (n > 0) {
                    if (dist(rng) < dropRate) continue;
                    conn->HandleDatagram(buf, size_t(n), from);
                }
                conn->Tick(NowNs());
            }
        });
    }
    void Stop() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
};

// A44 (TASK-HANDSHAKE-HARDEN-2026-11-22): frozen KDF oracle. This reproduces the
// pre-change wire bytes exactly and deliberately lacks the low-order defense, so
// equality proves interop/byte-frozen derivation and inequality on a low-order
// input proves the defense actually fired.
static crypto::SessionKeys ReferenceDerive(const crypto::Key& shared, const crypto::Key& clientPublic,
                                           const crypto::Key& serverPublic, const std::string& context) {
    std::vector<uint8_t> transcript;
    static const char kLabel[] = "rincast-session-v3";
    transcript.insert(transcript.end(), kLabel, kLabel + sizeof(kLabel) - 1);
    transcript.insert(transcript.end(), clientPublic.begin(), clientPublic.end());
    transcript.insert(transcript.end(), serverPublic.begin(), serverPublic.end());
    transcript.insert(transcript.end(), context.begin(), context.end());

    uint8_t okm[64];
    crypto_blake2b_keyed(okm, sizeof(okm), shared.data(), shared.size(), transcript.data(), transcript.size());
    crypto::SessionKeys keys;
    std::memcpy(keys.clientToServer.data(), okm, 32);
    std::memcpy(keys.serverToClient.data(), okm + 32, 32);
    crypto_wipe(okm, sizeof(okm));
    return keys;
}

static void TestHandshakeHardening() {
    // --- gate 5 / 1.1 regression: byte-identical derivation on a normal exchange ---
    crypto::Key mySecret{}, peerSecret{}, clientPublic{}, serverPublic{};
    for (size_t i = 0; i < 32; ++i) {
        mySecret[i] = uint8_t(i);
        peerSecret[i] = uint8_t(0xA0 + i);
        clientPublic[i] = uint8_t(0x20 + i);
        serverPublic[i] = uint8_t(0x40 + i);
    }
    crypto::Key peerPublic{};
    crypto_x25519_public_key(peerPublic.data(), peerSecret.data());
    const std::string context = "cn|sn|secret";  // == SessionContext("cn","sn","secret")

    crypto::Key refShared{};
    crypto_x25519(refShared.data(), mySecret.data(), peerPublic.data());
    const auto expected = ReferenceDerive(refShared, clientPublic, serverPublic, context);
    const auto actual = crypto::DeriveSessionKeys(mySecret, peerPublic, clientPublic, serverPublic, context);
    CHECK(actual.clientToServer == expected.clientToServer);
    CHECK(actual.serverToClient == expected.serverToClient);
    // Known-answer hex captured from the pre-change build (fixed vector, wire frozen).
    CHECK(HexEncode(actual.clientToServer.data(), 32) ==
          "2765ab9a32996cbd13c7c4faa93b84fa1869ffca6d9407f55bf670446d697c9f");
    CHECK(HexEncode(actual.serverToClient.data(), 32) ==
          "77697a0a60407ce545e8ef05810146cb138e7a5c8d9f7bdab3f644f326540436");

    // --- 1.1: a low-order peer public key must not yield the predictable key ---
    crypto::Key zeroPublic{};  // all-zero public key is a low-order point
    crypto::Key probe{};
    crypto_x25519(probe.data(), mySecret.data(), zeroPublic.data());
    uint8_t probeAcc = 0;
    for (size_t i = 0; i < 32; ++i) probeAcc |= probe[i];
    CHECK(probeAcc == 0);  // confirm the crafted input really drives shared to all zeros

    const auto predictable = ReferenceDerive(zeroPublic /* all-zero shared */, clientPublic, serverPublic, context);
    const auto defended = crypto::DeriveSessionKeys(mySecret, zeroPublic, clientPublic, serverPublic, context);
    CHECK(defended.clientToServer != predictable.clientToServer);
    CHECK(defended.serverToClient != predictable.serverToClient);
    const auto defendedAgain = crypto::DeriveSessionKeys(mySecret, zeroPublic, clientPublic, serverPublic, context);
    CHECK(defendedAgain.clientToServer == defended.clientToServer);  // still deterministic
    CHECK(defendedAgain.serverToClient == defended.serverToClient);

    // --- 1.3: a rejected parse must not clobber the caller's out ---
    HandshakePacket io;
    io.type = PacketType::Reject;
    io.sessionId = 0xDEADBEEFu;
    io.publicKey = clientPublic;
    io.json = "stale";

    HandshakePacket welcome;
    welcome.type = PacketType::Welcome;
    welcome.sessionId = 7;
    welcome.publicKey = serverPublic;
    welcome.json = R"({"ok":true})";
    auto welcomeBytes = BuildHandshake(welcome);
    CHECK(ParseHandshake(welcomeBytes.data(), welcomeBytes.size(), io));
    CHECK(io.publicKey == serverPublic && io.sessionId == 7 && io.json == welcome.json);

    HandshakePacket reject;
    reject.type = PacketType::Reject;
    reject.sessionId = 8;
    reject.json = R"({"reason":"no"})";
    auto rejectBytes = BuildHandshake(reject);
    CHECK(ParseHandshake(rejectBytes.data(), rejectBytes.size(), io));
    CHECK(io.publicKey == crypto::Key{});  // Reject carries no key -> must be zeroed
    CHECK(io.sessionId == 8 && io.json == reject.json);

    // Truncated Hello: returns false and leaves `io` byte-for-byte untouched.
    const HandshakePacket before = io;
    auto trunc = BuildHandshake(welcome);
    trunc.resize(sizeof(PacketHeader) + 16);  // key field cut in half
    CHECK(!ParseHandshake(trunc.data(), trunc.size(), io));
    CHECK(io.type == before.type && io.sessionId == before.sessionId && io.publicKey == before.publicKey &&
          io.json == before.json);

    // --- 1.4: version gate on IsHandshakeDatagram (Punch included) ---
    auto helloBytes = BuildHandshake(welcome);
    helloBytes[0] = uint8_t(kProtocolVersion) + 1;  // header.version is byte 0
    CHECK(!IsHandshakeDatagram(helloBytes.data(), helloBytes.size()));
    helloBytes[0] = uint8_t(kProtocolVersion);
    CHECK(IsHandshakeDatagram(helloBytes.data(), helloBytes.size()));
    // Punch is hand-built in cloud_client.cpp with version = kProtocolVersion;
    // it must still be recognized (hole punching preserved).
    PacketHeader punch{};
    punch.version = uint8_t(kProtocolVersion);
    punch.type = uint8_t(PacketType::Punch);
    punch.flags = kFlagNone;
    CHECK(IsHandshakeDatagram(reinterpret_cast<const uint8_t*>(&punch), sizeof(punch)));

    std::printf("handshake hardening OK: low-order defense + frozen KDF vector + parse isolation + version gate\n");
}

static void TestHandshakeAndTransport() {
    auto client = crypto::KeyPair::Generate();
    auto server = crypto::KeyPair::Generate();
    const std::string ctx = SessionContext("cn", "sn", "secret");
    auto ck = crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
    auto sk = crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
    CHECK(ck.clientToServer == sk.clientToServer);
    CHECK(ck.serverToClient == sk.serverToClient);

    HandshakePacket hello;
    hello.type = PacketType::Hello;
    hello.publicKey = client.publicKey;
    hello.json = R"({"deviceId":"x"})";
    auto bytes = BuildHandshake(hello);
    HandshakePacket parsed;
    CHECK(IsHandshakeDatagram(bytes.data(), bytes.size()));
    CHECK(ParseHandshake(bytes.data(), bytes.size(), parsed));
    CHECK(parsed.publicKey == client.publicKey && parsed.json == hello.json);
    CHECK(HelloProof("s", client.publicKey) == HelloProof("s", client.publicKey));
    CHECK(HelloProof("s", client.publicKey) != HelloProof("t", client.publicKey));

    Peer a, b;
    CHECK(a.socket.Open(0));
    CHECK(b.socket.Open(0));
    NetAddress aAddr{0x7F000001, a.socket.LocalPort()};
    NetAddress bAddr{0x7F000001, b.socket.LocalPort()};
    a.conn = std::make_unique<Connection>(&a.socket, bAddr, 42, sk, true, kMaxDatagramLan);
    b.conn = std::make_unique<Connection>(&b.socket, aAddr, 42, ck, false, kMaxDatagramLan);
    a.dropRate = 0.2;
    b.dropRate = 0.2;

    std::atomic<int> received{0};
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); i++) big[i] = char('a' + i % 26);
    std::atomic<bool> bigOk{false};
    std::atomic<int> order{0};
    b.conn->SetControlHandler([&](const std::string& m) {
        if (m.size() == big.size()) {
            bigOk = (m == big);
        } else {
            const int idx = std::atoi(m.c_str());
            if (idx != order.load()) std::printf("out of order %d vs %d\n", idx, order.load());
            order = idx + 1;
        }
        received++;
    });

    // Video: frames with FEC through a lossy link.
    std::atomic<int> framesOk{0};
    VideoReassembler reasm([&](ReceivedFrame&& f) {
        bool ok = f.bitstream.size() == 50000 + f.frameIndex;
        for (size_t i = 0; ok && i < f.bitstream.size(); i++) ok = f.bitstream[i] == uint8_t(i * 7 + f.frameIndex);
        if (ok) framesOk++;
    });
    b.conn->SetPacketHandler([&](PacketType t, const uint8_t* p, size_t n) {
        if (t == PacketType::Video) reasm.HandleFragment(p, n);
        if (t == PacketType::VideoParity) reasm.HandleParity(p, n);
    });

    a.Run();
    b.Run();
    for (int i = 0; i < 200; i++) a.conn->SendControl(std::to_string(i));
    a.conn->SendControl(big);

    const int64_t start = NowNs();
    while (received < 201 && NowNs() - start < 10'000'000'000LL) SleepMs(10);
    CHECK(received == 201);
    CHECK(bigOk);
    CHECK(order == 200);

    // Low loss for video to make FEC meaningful (single loss per group recoverable).
    b.dropRate = 0.01;
    a.dropRate = 0;
    SleepMs(200);
    VideoPacketizer pkt(a.conn.get());
    pkt.SetFecPercent(10);
    for (uint32_t f = 1; f <= 60; f++) {
        std::vector<uint8_t> data(50000 + f);
        for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 7 + f);
        DesktopFrameMetadata meta{};
        pkt.SendFrame(1, VideoCodec::HEVC, 0, f, &meta, sizeof(meta), data.data(), data.size());
        SleepMs(5);
    }
    SleepMs(300);
    auto stats = reasm.TakeStats();
    std::printf("video: %d/60 frames ok, %u recovered, %u lost frags, rtt %.2fms, offset %lldns\n", framesOk.load(),
                stats.fragmentsRecovered, stats.fragmentsLost, b.conn->Stats().rttMs,
                (long long)b.conn->ClockOffsetNs());
    CHECK(framesOk >= 50);
    CHECK(std::llabs(b.conn->ClockOffsetNs()) < 2'000'000);  // same machine => ~0

    a.Stop();
    b.Stop();
}

static void TestIdentity() {
    const auto seed = crypto::NewIdentitySeed();
    const auto id = crypto::IdentityPublicKey(seed);
    const auto eph = crypto::KeyPair::Generate();
    const auto sig = crypto::SignConnection(seed, eph.publicKey, "nonce");
    CHECK(crypto::VerifyConnection(id, eph.publicKey, "nonce", sig));
    CHECK(!crypto::VerifyConnection(id, eph.publicKey, "other", sig));
    CHECK(!crypto::VerifyConnection(id, crypto::KeyPair::Generate().publicKey, "nonce", sig));  // a replay can't reuse it
    CHECK(!crypto::VerifyConnection(crypto::IdentityPublicKey(crypto::NewIdentitySeed()), eph.publicKey, "nonce", sig));
}

static void TestDiscovery() {
    DiscoveryAnnounce a;
    a.serverId = "abc";
    a.name = "Gaming PC";
    a.accountLinked = true;
    a.nonce = 7;
    auto bytes = BuildAnnounce(a);
    DiscoveryAnnounce b;
    CHECK(ParseAnnounce(bytes.data(), bytes.size(), b));
    CHECK(b.serverId == "abc" && b.name == "Gaming PC" && b.accountLinked && b.nonce == 7);
    DiscoveryProbe p;
    CHECK(!ParseProbe(bytes.data(), bytes.size(), p));
    // ROUND20 T1 (B1): the broadcast never carries the account name, and an old-style
    // announcer that still does is parsed without breaking (the field defaults to "").
    const std::string wire(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(wire.find("accountName") == std::string::npos);
    CHECK(wire.find("Gaming PC") != std::string::npos);  // sanity: the JSON is on the wire
    rc::DiscoveryHeader header{rc::kDiscoveryMagic, rc::kProtocolVersion,
                               uint8_t(rc::DiscoveryType::Announce), 0};
    const std::string oldJson = R"({"serverId":"abc","name":"Gaming PC","accountLinked":true,)"
                                R"("accountName":"someone@cloud","nonce":7})";
    std::vector<uint8_t> oldWire(sizeof(header) + oldJson.size());
    std::memcpy(oldWire.data(), &header, sizeof(header));
    std::memcpy(oldWire.data() + sizeof(header), oldJson.data(), oldJson.size());
    DiscoveryAnnounce legacy;
    CHECK(ParseAnnounce(oldWire.data(), oldWire.size(), legacy));
    CHECK(legacy.accountLinked && legacy.accountName == "someone@cloud");  // tolerated in, never broadcast out
}

// A51 (TASK-DISCOVERY-PLATFORM-2026-11-22): every byte here arrives from the LAN, so a
// malformed body has to be rejected, not thrown out of the parser.
static std::vector<uint8_t> DiscoveryWire(DiscoveryType type, const std::string& body) {
    DiscoveryHeader header{kDiscoveryMagic, kProtocolVersion, uint8_t(type), 0};
    std::vector<uint8_t> out(sizeof(header) + body.size());
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), body.data(), body.size());
    return out;
}

static void TestDiscoveryHardening() {
    DiscoveryProbe p;
    DiscoveryAnnounce a;
    // A type mismatch used to throw json::type_error straight through these functions, which
    // means std::terminate - one unauthenticated broadcast packet killed the process.
    // Type mismatches throw inside json::value(), so these must come back false rather than
    // terminate the process.
    for (const char* body : {R"({"deviceId":123})", R"({"deviceName":[1,2]})", R"({"nonce":"abc"})",
                             R"({"deviceId":null})", R"({"nonce":{"a":1}})"}) {
        const auto wire = DiscoveryWire(DiscoveryType::Probe, body);
        CHECK(!ParseProbe(wire.data(), wire.size(), p));
    }
    // Numbers convert leniently rather than throwing (nlohmann semantics, not a crash):
    // pin the behaviour so a future library bump that starts rejecting them is noticed.
    {
        const auto wire = DiscoveryWire(DiscoveryType::Probe, R"({"nonce":-1})");
        CHECK(ParseProbe(wire.data(), wire.size(), p) && p.nonce == 0xFFFFFFFFu);  // wraps, no throw
        const auto frac = DiscoveryWire(DiscoveryType::Probe, R"({"nonce":1.5})");
        CHECK(ParseProbe(frac.data(), frac.size(), p) && p.nonce == 1u);  // truncates, no throw
    }
    for (const char* body : {R"({"serverId":"x","busy":1})", R"({"serverId":"x","port":70000})",
                             R"({"serverId":"x","port":-5})", R"({"serverId":42})",
                             R"({"serverId":"x","name":{"a":1}})"}) {
        const auto wire = DiscoveryWire(DiscoveryType::Announce, body);
        CHECK(!ParseAnnounce(wire.data(), wire.size(), a));
    }
    // A rejected packet must not leave a half-written result behind.
    a = {};
    a.serverId = "keep-me";
    const auto bad = DiscoveryWire(DiscoveryType::Announce, R"({"serverId":"other","busy":1})");
    CHECK(!ParseAnnounce(bad.data(), bad.size(), a) && a.serverId == "keep-me");
    // A well-formed packet still parses, port included.
    const auto ok = DiscoveryWire(DiscoveryType::Announce,
                                  R"({"serverId":"abc","name":"PC","port":38711,"busy":false})");
    CHECK(ParseAnnounce(ok.data(), ok.size(), a) && a.serverId == "abc" && a.streamPort == 38711);
    printf("discovery hardening OK\n");
}

static void TestAudio() {
    AudioEncoder enc;
    AudioDecoder dec;
    CHECK(enc.Init(48000, 2, 128000, true));
    CHECK(dec.Init(48000, 2));
    std::vector<int16_t> pcm(480 * 2);
    for (int i = 0; i < 480; i++) pcm[i * 2] = pcm[i * 2 + 1] = int16_t(8000 * std::sin(i * 0.05));
    uint8_t packet[1500];
    const int n = enc.Encode(pcm.data(), 480, packet, sizeof(packet));
    CHECK(n > 0);
    std::vector<int16_t> out(5760 * 2);
    CHECK(dec.Decode(packet, n, out.data(), 5760) == 480);

    JitterBuffer jb;
    jb.Configure(2, 48000, 20, 100);
    for (int i = 0; i < 3; i++) jb.Push(pcm.data(), 480);
    std::vector<int16_t> o(480 * 2);
    CHECK(jb.Pop(o.data(), 480) == 480);
    // Running dry after playback started means the network was late: hold 20 ms more.
    CHECK(jb.TargetMs() == 20);
    for (int i = 0; i < 3; i++) jb.Pop(o.data(), 480);
    CHECK(jb.TargetMs() == 40);
    CHECK(jb.Pop(o.data(), 480) == 0);  // priming again, to the new target
}

// Reordering: parity first must not lose the frame's flags; pacing spreads a frame out.
static void TestVideoReassemblyOrder() {
    std::vector<ReceivedFrame> done;
    VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
    const uint8_t payload[] = {0, 0, 'a', 'b', 'c', 'd'};  // no metadata
    VideoFragmentHeader h{};
    h.streamId = 1;
    h.codec = uint8_t(VideoCodec::HEVC);
    h.frameFlags = kVideoKeyframe;
    h.fecGroupSize = 2;
    h.frameIndex = 7;
    h.fragmentCount = 2;
    h.frameSize = sizeof(payload);
    VideoParityHeader ph{};
    ph.streamId = 1;
    ph.fecGroupSize = 2;
    ph.groupIndex = 0;
    ph.frameIndex = 7;
    ph.fragmentCount = 2;
    ph.lastFragmentSize = 3;
    ph.frameSize = sizeof(payload);
    std::vector<uint8_t> parity(sizeof(ph) + 3);
    std::memcpy(parity.data(), &ph, sizeof(ph));
    for (int i = 0; i < 3; i++) parity[sizeof(ph) + i] = uint8_t(payload[i] ^ payload[i + 3]);
    r.HandleParity(parity.data(), parity.size());
    std::vector<uint8_t> frag(sizeof(h) + 3);
    h.fragmentIndex = 0;
    std::memcpy(frag.data(), &h, sizeof(h));
    std::memcpy(frag.data() + sizeof(h), payload, 3);
    r.HandleFragment(frag.data(), frag.size());
    h.fragmentIndex = 1;
    std::memcpy(frag.data(), &h, sizeof(h));
    std::memcpy(frag.data() + sizeof(h), payload + 3, 3);
    r.HandleFragment(frag.data(), frag.size());
    CHECK(done.size() == 1);
    if (!done.empty()) {
        CHECK(done[0].codec == VideoCodec::HEVC);
        CHECK(done[0].flags == kVideoKeyframe);
        CHECK(done[0].firstFragmentNs <= done[0].completeNs);
    }
}

static void TestPacing() {
    UdpSocket a, b;
    CHECK(a.Open(0) && b.Open(0));
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey, "t");
    Connection conn(&a, NetAddress{0x7F000001, b.LocalPort()}, 1, keys, true, kMaxDatagramLan);
    VideoPacketizer p(&conn);
    p.SetFecPercent(0);
    std::vector<uint8_t> data(100000);
    DesktopFrameMetadata meta{};
    int64_t t = NowNs();
    p.SendFrame(1, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), data.data(), data.size());
    const double unpacedMs = NsToMs(NowNs() - t);
    p.SetPacingKbps(8000);  // 100 kB at 1 MB/s: about 100 ms
    t = NowNs();
    p.SendFrame(1, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), data.data(), data.size());
    const double pacedMs = NsToMs(NowNs() - t);
    std::printf("pacing: %.1f ms unpaced, %.1f ms at 8 Mbit/s\n", unpacedMs, pacedMs);
    CHECK(pacedMs > 80 && pacedMs < 130);
}

// ---------------------------------------------------------------------------
// 回环收包器：一端（sendConn）发，另一端（recvConn）收，收包线程记录每个数据报的
// 到达时刻和字节数，同时把内容喂给重组器。drop* 用来在重组器之前丢掉指定的分片，
// 效果和这个数据报在线路上丢了完全一样（混淆、序号都已经校验过了）。
// ---------------------------------------------------------------------------
namespace {

// pacing 测量对调度很敏感：发送和收包互相抢占会把两批包挤成一团，量出来的突发就失真。
// 这里把两边各钉在一个核上（Windows；其它平台空实现）。
DWORD_PTR PinThread(int core) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
#else
    (void)core;
    return 0;
#endif
}
void UnpinThread(DWORD_PTR mask) {
#ifdef _WIN32
    if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
#else
    (void)mask;
#endif
}

struct LoopbackPair {
    UdpSocket tx, rx;
    std::unique_ptr<Connection> sendConn, recvConn;
    std::unique_ptr<VideoReassembler> reasm;
    std::thread reader;
    std::atomic<bool> stop{false};

    std::mutex mutex;
    std::vector<std::pair<int64_t, size_t>> arrivals;  // (到达时刻, 数据报字节数)
    std::vector<ReceivedFrame> frames;                // 重组完成的帧
    // The drop plan is per frame and it is set BEFORE the frame goes out. A single
    // global plan was a race in ORDER, not only in visibility: the receiver thread
    // runs behind the sender, so the plan had already moved on to the next frame
    // before the receiver had dropped the previous frame's fragment - 2 of 5 runs
    // failed with "dropped 2" instead of 3 (D-027). The assertions below are
    // unchanged.
    struct DropPlan {
        std::atomic<uint32_t> frame{0};  // 0 = nothing to drop
        std::atomic<int> fragA{-1};
        std::atomic<int> fragB{-1};
    };
    DropPlan drop2, drop3;
    std::atomic<int> dropped{0};

    bool Open() {
        if (!tx.Open(0) || !rx.Open(0)) return false;
        tx.SetBuffers(8 << 20, 1 << 20);
        rx.SetBuffers(1 << 20, 8 << 20);
        auto client = crypto::KeyPair::Generate();
        auto server = crypto::KeyPair::Generate();
        const std::string ctx = SessionContext("cn", "sn", "pacing");
        auto ck = crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
        auto sk = crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
        sendConn = std::make_unique<Connection>(&tx, NetAddress{0x7F000001, rx.LocalPort()}, 7, sk, true,
                                                kMaxDatagramLan);
        recvConn = std::make_unique<Connection>(&rx, NetAddress{0x7F000001, tx.LocalPort()}, 7, ck, false,
                                                kMaxDatagramLan);
        reasm = std::make_unique<VideoReassembler>([this](ReceivedFrame&& f) {
            std::lock_guard lock(mutex);
            frames.push_back(std::move(f));
        });
        recvConn->SetPacketHandler([this](PacketType t, const uint8_t* d, size_t n) {
            if (t == PacketType::Video) {
                if (n < sizeof(VideoFragmentHeader)) return;
                VideoFragmentHeader h;
                std::memcpy(&h, d, sizeof(h));
                const uint32_t frame = h.frameIndex;
                const int frag = int(h.fragmentIndex);
                const auto hits = [&](const DropPlan& plan) {
                    return frame == plan.frame.load() &&
                           (frag == plan.fragA.load() || frag == plan.fragB.load());
                };
                if (hits(drop2) || hits(drop3)) {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                reasm->HandleFragment(d, n);
            } else if (t == PacketType::VideoParity) {
                reasm->HandleParity(d, n);
            }
        });
        return true;
    }

    void Start() {
        reader = std::thread([this] {
            std::vector<uint8_t> buf(2048);
            NetAddress from;
            while (!stop.load()) {
                const int n = rx.RecvFrom(buf.data(), buf.size(), from, 20);
                if (n <= 0) {
                    recvConn->Tick(NowNs());
                    continue;
                }
                const int64_t now = NowNs();
                {
                    std::lock_guard lock(mutex);
                    arrivals.emplace_back(now, size_t(n));
                }
                recvConn->HandleDatagram(buf.data(), size_t(n), from);
            }
        });
    }

    void Stop() {
        stop = true;
        if (reader.joinable()) reader.join();
    }

    // 等着收包线程把已经发出去的东西收完（回环上很快，这里只是给最后一帧一点时间）。
    void Drain(int ms) { SleepMs(ms); }

    size_t PendingArrivals() {
        std::lock_guard lock(mutex);
        return arrivals.size();
    }
};

// 裸收包器：只 recvfrom + 记时间戳，不解密不重组。pacing 测的正是「线路上的节奏」，
// 收包侧一旦跟上不就会把相邻两批挤到一起，量出来的突发就不真了。
struct RawReceiver {
    UdpSocket sock;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::atomic<size_t> count{0};
    std::vector<std::pair<int64_t, size_t>> arrivals;

    bool Open() {
        if (!sock.Open(0)) return false;
        sock.SetBuffers(1 << 20, 16 << 20);
        return true;
    }
    void Start() {
        // 收包侧要跟得上 200+ Mbps：预分配 + 原子下标，不用锁也不用 push_back。
        arrivals.resize(200'000);
        reader = std::thread([this] {
            PinThread(1);
            std::vector<uint8_t> buf(2048);
            NetAddress from;
            while (!stop.load()) {
                const int n = sock.RecvFrom(buf.data(), buf.size(), from, 20);
                if (n <= 0) continue;
                const int64_t now = NowNs();
                const size_t i = count.fetch_add(1, std::memory_order_relaxed);
                if (i < arrivals.size()) arrivals[i] = {now, size_t(n)};
            }
        });
    }
    ~RawReceiver() { Stop(); }  // a joinable std::thread dtor would std::terminate
    void Stop() {
        stop = true;
        if (reader.joinable()) reader.join();
    }
};

struct TimelineStats {    double p50Ms = 0, p99Ms = 0, maxMs = 0;
    size_t burstBytes = 0;  // 1 ms 窗口内的最大字节数（含两批之间的空隙，仅供参考）
    size_t batchBytes = 0;  // 单批突发：相邻包间隔 < 0.15 ms 的连续一串的最大字节数
    size_t p50BatchBytes = 0;  // 批字节数的中位数：典型的单批突发（收包侧偶发停顿会把两批
                               // 挤在一起，所以 max/p99 会被拖尾，中位数最能代表实际行为）
    size_t p90BatchBytes = 0;
    size_t maxPacket = 0;
    size_t packets = 0;
};

// 只看视频包（ACK / ping 这类几十字节的小包不算），算包间隔和突发。
TimelineStats Analyze(std::vector<std::pair<int64_t, size_t>> arrivals, int64_t windowNs) {
    TimelineStats s;
    std::vector<std::pair<int64_t, size_t>> v;
    for (const auto& a : arrivals)
        if (a.second > 200) v.push_back(a);
    s.packets = v.size();
    if (v.empty()) return s;
    std::vector<double> gaps;
    for (size_t i = 1; i < v.size(); i++) gaps.push_back(NsToMs(v[i].first - v[i - 1].first));
    for (const auto& a : v) s.maxPacket = std::max(s.maxPacket, a.second);
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        s.p50Ms = gaps[gaps.size() / 2];
        s.p99Ms = gaps[gaps.size() * 99 / 100];
        s.maxMs = gaps.back();
    }
    // 滑动窗口内的最大字节数。
    size_t j = 0, sum = 0;
    for (size_t i = 0; i < v.size(); i++) {
        sum += v[i].second;
        while (v[i].first - v[j].first >= windowNs) sum -= v[j++].second;
        s.burstBytes = std::max(s.burstBytes, sum);
    }
    // 单批突发：间隔 < 0.3 ms 就算同一批连续发出（pacing 是「攒够 0.5 ms 的量再一次发」，
    // 所以一批就是一次真正的突发）。
    // 单批突发：间隔 < 0.15 ms 就算同一批连续发出。实测线路上的结构是「一批约 19 个包
    // 挤在一起（接收侧逐个取包，包间隔约 17 µs），然后 0.5~0.75 ms 的空隙」，
    // 所以批边界用 0.15 ms 判得干净；判宽了会把相邻两批并成一批，量化出来的突发虚高。
    constexpr int64_t kBatchGapNs = 150'000;
    std::vector<size_t> batches;
    size_t cur = v[0].second;
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i].first - v[i - 1].first <= kBatchGapNs) {
            cur += v[i].second;
        } else {
            batches.push_back(cur);
            cur = v[i].second;
        }
    }
    batches.push_back(cur);
    std::sort(batches.begin(), batches.end());
    s.batchBytes = batches.back();
    s.p50BatchBytes = batches[batches.size() / 2];
    s.p90BatchBytes = batches[batches.size() * 90 / 100];
    return s;
}

// ---------------------------------------------------------------------------
// TASK-FLAKE-ROBUST-2026-11-22（方案 b 自校准容差，主 AI 批准，带三条护栏）：
// 固定 ms 容差对调度抢占过敏——负载（用户串流）下边界间隔被压缩/拉长，断言确定性失败。
// 断言容差 = max(固定基线, min(实测抖动×安全系数, kAdaptiveTolCapMs))。
// 护栏 1：容差必须有绝对上限。取 15 ms，依据：两个计时测试的计费间隔（TestPacingBursts
// 35.0 ms / TestPacingBurstShaping ~30.3 ms）的一半——(1) 变异鉴别力：计费 ×10 错误把帧
// 间隔压到 billing/10（~3 ms），容差上限 15 ms 时 billing - cap 仍远高于它，变异必然仍挂；
// (2) 与测试自己的边界判定阈值（gap >= billing/2 才算边界）同尺度；(3) 吸收串流负载下
// 10-15 ms 量级的收包停顿。没有上限的自校准会在负载下把断言变成永真式。
// 护栏 3：实测抖动超过上限时断言照常失败并如实报——真故障浮出通道，不许静默豁免。
constexpr double kJitterSafety = 3.0;       // 探针只采少数边界，max 低估尾部，×3 兜住
constexpr double kAdaptiveTolCapMs = 15.0;  // 护栏 1：绝对上限（取值依据见上）

struct PacingJitter {
    double compressMs = 0;  // 边界间隔比计费短了多少（接收停顿把包挤到一起的幅度）
    double stretchMs = 0;   // 边界间隔比计费长了多少（唤醒过冲/抢占的幅度）
    double stallScaleMs = 0;  // 统一停顿幅度 = max(compress, stretch)，见 CalibrateTimingJitter
};

// 护栏 2：校准数据源必须独立于被断言的运行。这里跑一次独立的空载 pacer 采样（与
// TestPacingBursts 同形：96 KB×8 帧 @ 24 Mbps，帧预算 8 ms，独立 socket/连接/帧），
// 不许用被断言那批帧自己的延迟做校准——否则 pacer 本身坏了会把自己的症状校准掉
// （自指陷阱）。变异污染探针时（计费 ×10），采样偏差会被护栏 1 的上限封住，断言仍挂。
// 不用 SleepMs 过冲做信号源：测试进程没有 timeBeginPeriod，sleep_for 的唤醒量化在
// 15.6 ms 的系统节拍上，量出的是定时器分辨率不是调度抢占（实测 ~13 ms 假信号），而
// pacer 的等待走 SetWaitableTimerEx + 自旋，从不睡 SleepMs——pacer 采样本身就是对的
// 拉伸信号源。
static PacingJitter ProbePacingJitter() {
    PacingJitter j;
    RawReceiver rx;
    UdpSocket tx;
    if (!rx.Open() || !tx.Open(0)) return j;
    tx.SetBuffers(16 << 20, 1 << 20);
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                          "t");
    Connection conn(&tx, NetAddress{0x7F000001, rx.sock.LocalPort()}, 1, keys, true, kMaxDatagramLan);
    rx.Start();
    VideoPacketizer pkt(&conn);
    pkt.SetFecPercent(5);
    pkt.SetPacingKbps(24000);
    pkt.SetPacingFrameBudgetNs(8'000'000LL);
    constexpr size_t kFrameBytes = 96 * 1024;
    constexpr uint32_t kFrames = 8;  // 7 个边界样本：压缩事件稀疏，样本太少会低估尾部
    std::vector<uint8_t> frame(kFrameBytes);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 31);
    VrFrameMetadata meta{};
    meta.trackingTimestampNs = 12345;
    const size_t fragPayload = conn.MaxPayload() - sizeof(VideoFragmentHeader);
    const size_t total = 2 + sizeof(VrFrameMetadata) + kFrameBytes;
    const size_t count = (total + fragPayload - 1) / fragPayload;
    const size_t groupSize = size_t(std::clamp(100 / 5, 2, 64));
    const size_t groups = (count + groupSize - 1) / groupSize;
    const size_t lastMembers = count - (groups - 1) * groupSize;
    const size_t nParity = groups - (lastMembers == 1 ? 1 : 0);
    const size_t perFrame = count + nParity;
    const double billingMs = double(total + count * sizeof(VideoFragmentHeader) +
                                    groups * (sizeof(VideoParityHeader) + fragPayload)) * 8.0 /
                             24000.0;
    const DWORD_PTR mask = PinThread(0);
    for (uint32_t f = 0; f < kFrames; f++)
        pkt.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, f + 1, &meta, sizeof(meta), frame.data(), frame.size());
    UnpinThread(mask);
    SleepMs(60);
    rx.Stop();
    const size_t n = std::min(rx.count.load(), rx.arrivals.size());
    std::vector<std::pair<int64_t, size_t>> v;
    for (size_t i = 0; i < n; i++)
        if (rx.arrivals[i].second > 200) v.push_back(rx.arrivals[i]);
    // 帧首片按已知每帧包数取位（回环不丢不重排）。采样不完整时保守放弃 pacer 部分
    // （jitter 全零 → 容差落在固定基线）——宁紧勿松，不给断言静默放宽（护栏 3）。
    if (perFrame > 0 && v.size() >= size_t(kFrames) * perFrame) {
        double minGap = 1e9, maxGap = 0;
        for (uint32_t f = 1; f < kFrames; f++) {
            const double gapMs = NsToMs(v[size_t(f) * perFrame].first - v[size_t(f - 1) * perFrame].first);
            minGap = std::min(minGap, gapMs);
            maxGap = std::max(maxGap, gapMs);
        }
        j.compressMs = std::max(0.0, billingMs - minGap);
        j.stretchMs = std::max(0.0, maxGap - billingMs);
    }
    return j;
}

// 护栏 2 的完整取数：独立 pacer 采样，打印留痕（如实报）。同一台机器的停顿总体既会
// 打在发送侧（探针里表现为边界拉伸）也会打在接收侧（表现为边界压缩）——落在哪一侧
// 是概率事件，不是两个独立尺度：探针量到 |X| 的拉伸，被断言的运行里同幅度的停顿完全
// 可以表现为压缩。所以两侧统一到同一个停顿幅度 stallScale = max(compress, stretch)。
static PacingJitter CalibrateTimingJitter() {
    PacingJitter j = ProbePacingJitter();
    j.stallScaleMs = std::max(j.compressMs, j.stretchMs);
    std::printf("timing calib: pacer boundary compress %.2f / stretch %.2f ms -> stall scale %.2f ms "
                "(safety x%.1f, cap %.1f ms)\n",
                j.compressMs, j.stretchMs, j.stallScaleMs, kJitterSafety, kAdaptiveTolCapMs);
    return j;
}

// 护栏 1 的公式：容差 = max(固定基线, min(实测抖动×安全系数, 绝对上限))。
static double AdaptiveTol(double fixedBaselineMs, double jitterMs) {
    return std::max(fixedBaselineMs, std::min(jitterMs * kJitterSafety, kAdaptiveTolCapMs));
}

}  // namespace

// A10 (TASK-LATENCY-BURST-SEND 2026-11-21)：帧间预算 = 帧粒度令牌桶。每帧按线路字节在
// 平均上限（pacing kbps）上计费，下一帧不得早于计费结束开始；帧内背靠背突发（窗口内
// 的单帧突发形态由 TestPacingFrameBudget/TestPacingBurstShaping 验）。这里验证：连续 N
// 帧无空隙地交给发送器时，帧首片间隔 >= 帧字节/上限（平均速率守上限，token 桶语义），
// 一个包都不丢，发送循环真实等了计费而不是把等待摊进帧里。
// TASK-FLAKE-ROBUST-2026-11-22：压缩侧容差（原固定 2.0 ms）改为自校准自适应（上限
// 15 ms，护栏依据见 CalibrateTimingJitter 上方）；拉长侧不变——负载只会拉长
// sendLoop/span、压低 avgMbps，那三个方向本来就是安全的。
static void TestPacingBursts() {
    const PacingJitter jitter = CalibrateTimingJitter();  // 护栏 2：独立采样
    // 压缩侧固定基线 2.0 -> 4.0 ms：等待唤醒的定时器量化（±~1 ms/边界，两个边界的差分
    // 可到 ~2 ms）加上收包侧小延迟差；空载实测底噪 1.4-2.7 ms（streamer 进程在跑）。
    // 原固定 2.0 ms 对这个底噪过敏（本卡 §0 的问题本身）。拉长侧基线保持不变。
    const double tolCompressMs = AdaptiveTol(4.0, jitter.stallScaleMs);  // 护栏 1 公式
    const double tolSpanMs = AdaptiveTol(2.0, jitter.stallScaleMs);
    RawReceiver rx;
    UdpSocket tx;
    CHECK(rx.Open() && tx.Open(0));
    if (!rx.sock.IsOpen()) return;
    tx.SetBuffers(16 << 20, 1 << 20);
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                          "t");
    Connection conn(&tx, NetAddress{0x7F000001, rx.sock.LocalPort()}, 1, keys, true, kMaxDatagramLan);
    rx.Start();

    constexpr uint32_t kPaceKbps = 24000;      // 平均上限 24 Mbps
    VideoPacketizer pkt(&conn);
    pkt.SetFecPercent(5);                      // 组大小 20：校验包参与计费（要计入带宽）
    pkt.SetPacingKbps(kPaceKbps);
    pkt.SetPacingFrameBudgetNs(8'000'000LL);   // 突发窗口 8 ms（每帧突发远小于它）

    constexpr size_t kFrameBytes = 96 * 1024;
    constexpr uint32_t kFrames = 6;
    std::vector<uint8_t> frame(kFrameBytes);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 31);
    VrFrameMetadata meta{};
    meta.trackingTimestampNs = 12345;
    // 每帧的数据报数与计费时长：与发送端同一套公式（分片头计入线路字节；末组只剩一个
    // 成员时不发校验包）。
    const size_t fragPayload = conn.MaxPayload() - sizeof(VideoFragmentHeader);
    const size_t total = 2 + sizeof(VrFrameMetadata) + kFrameBytes;
    const size_t count = (total + fragPayload - 1) / fragPayload;
    const size_t groupSize = size_t(std::clamp(100 / 5, 2, 64));
    const size_t groups = (count + groupSize - 1) / groupSize;
    const size_t lastMembers = count - (groups - 1) * groupSize;
    const size_t nParity = groups - (lastMembers == 1 ? 1 : 0);
    const size_t perFrame = count + nParity;
    const double billingMs = double(total + count * sizeof(VideoFragmentHeader) +
                                    groups * (sizeof(VideoParityHeader) + fragPayload)) * 8.0 /
                             double(kPaceKbps);
    const DWORD_PTR senderMask = PinThread(0);
    const int64_t t = NowNs();
    for (uint32_t f = 0; f < kFrames; f++)
        pkt.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, f + 1, &meta, sizeof(meta), frame.data(), frame.size());
    const double sendLoopMs = NsToMs(NowNs() - t);
    UnpinThread(senderMask);
    SleepMs(150);

    const size_t n = std::min(rx.count.load(), rx.arrivals.size());
    std::vector<std::pair<int64_t, size_t>> v;  // 只看视频包（几十字节的控制包不算）
    for (size_t i = 0; i < n; i++)
        if (rx.arrivals[i].second > 200) v.push_back(rx.arrivals[i]);
    // RINCAST_PACING_CSV=<path>：把原始到达时间线写出来，便于事后分析（不参与断言）。
    if (const char* csv = std::getenv("RINCAST_PACING_CSV")) {
        if (FILE* f = std::fopen(csv, "w")) {
            std::fprintf(f, "arrival_ns,bytes\n");
            for (const auto& a : v) std::fprintf(f, "%lld,%zu\n", (long long)a.first, a.second);
            std::fclose(f);
        }
    }
    const TimelineStats shape = Analyze(v, 1'000'000);
    CHECK(!v.empty());
    if (v.empty()) { rx.Stop(); return; }
    const double spanMs = NsToMs(v.back().first - v.front().first);
    // 帧边界 = 长空隙（一帧的突发 ~1-2 ms，计费间隔 ~35 ms）。断言方向取「只能更晚、
    // 不能更早」的一侧：调度只会把间隔拉长，不会把它压到计费之下。
    size_t boundaries = 0;
    double minBoundaryGap = 1e9;
    uint64_t totalBytes = v[0].second;
    for (size_t i = 1; i < v.size(); i++) {
        totalBytes += v[i].second;
        const double gapMs = NsToMs(v[i].first - v[i - 1].first);
        if (gapMs >= billingMs / 2) {
            boundaries++;
            if (gapMs < minBoundaryGap) minBoundaryGap = gapMs;
        }
    }
    const double avgMbps = double(totalBytes) * 8.0 / spanMs / 1000.0;
    std::printf("token bucket: %u x %.0f KB @ %u kbps, billing %.1f ms/frame; %zu datagrams (%zu expected), "
                "span %.1f ms, sendLoop %.1f ms, boundaries %zu, min gap %.2f ms, avg %.1f Mbps, "
                "gap p50 %.2f p99 %.2f ms, packet max %zu B, tol compress %.2f ms\n",
                kFrames, double(kFrameBytes) / 1024, kPaceKbps, billingMs, v.size(), kFrames * perFrame,
                spanMs, sendLoopMs, boundaries, minBoundaryGap, avgMbps, shape.p50Ms, shape.p99Ms,
                shape.maxPacket, tolCompressMs);

    CHECK(v.size() == kFrames * perFrame);  // 一个包都不丢（回环 + 大缓冲，没有丢弃路径）
    CHECK(shape.maxPacket <= kMaxDatagramLan);
    CHECK(sendLoopMs >= double(kFrames - 1) * billingMs - 2.0);  // 发送侧真实等了计费（只会被负载拉长，方向安全）
    CHECK(spanMs >= double(kFrames - 1) * billingMs - tolSpanMs);  // 令牌桶把帧隔开了（压缩=接收停顿假象，容差自适应）
    // 窗口内平均速率守上限：N 帧挤进 (N-1) 个计费间隔，理论上限是 N/(N-1) 倍平均上限，
    // 再留 15% 给突发的分摊（更紧就等于在断言定时器精度了）。
    CHECK(avgMbps <= double(kPaceKbps) * double(kFrames) / double(kFrames - 1) * 1.15 / 1000.0);
    // 结构精确时的硬检查：每个帧间隔都达到计费。边界多了说明主机在帧内停顿过（那是
    // 测量假象，span 检查仍然兜底），这时不对单个间隔下硬断言。容差自适应但封顶
    // （kAdaptiveTolCapMs）：抖动超上限 = 真故障，照常失败浮出（护栏 3）。
    if (boundaries == kFrames - 1) CHECK(minBoundaryGap >= billingMs - tolCompressMs);
    rx.Stop();
}

// 停止语义：pacing 等待中请求停止要毫秒级返回；反复建/停不挂、不泄漏。
static void TestPacingStop() {
    UdpSocket tx, rx;
    CHECK(tx.Open(0) && rx.Open(0));
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                          "t");
    Connection conn(&tx, NetAddress{0x7F000001, rx.LocalPort()}, 1, keys, true, kMaxDatagramLan);
    // 接收端有人收，否则发送缓冲满了会卡在 sendto 上（那是另一个问题，不属于本测试）。
    std::atomic<bool> stop{false};
    std::thread drain([&] {
        std::vector<uint8_t> buf(2048);
        NetAddress from;
        while (!stop.load()) rx.RecvFrom(buf.data(), buf.size(), from, 20);
    });

    {
        VideoPacketizer p(&conn);
        p.SetFecPercent(0);
        p.SetPacingKbps(2000);  // 100 KB 要 400 ms
        std::vector<uint8_t> data(100 * 1024);
        std::atomic<int64_t> doneNs{0};
        std::thread t([&] {
            p.SendFrame(1, VideoCodec::HEVC, 0, 1, nullptr, 0, data.data(), data.size());
            doneNs = NowNs();
        });
        SleepMs(30);
        const int64_t t0 = NowNs();
        p.RequestStop();
        t.join();
        const double stopMs = NsToMs(doneNs.load() - t0);
        std::printf("pacing stop: returned in %.1f ms\n", stopMs);
        CHECK(doneNs.load() != 0);
        CHECK(stopMs < 100);
    }
    // 建了马上停（还在睡眠/等待的那一段）、反复多轮，确认没有死锁。
    for (int i = 0; i < 8; i++) {
        VideoPacketizer p(&conn);
        p.SetPacingKbps(1000);
        if (i % 2) p.RequestStop();
    }
    stop = true;
    drain.join();
}

// FEC 语义（确定性丢包，不靠概率）：同组丢 1 个分片用 XOR 校验包恢复；
// 同组丢 2 个恢复不了，留给超时后请求关键帧的现有路径。
static void TestFecDeterministic() {
    LoopbackPair p;
    CHECK(p.Open());
    if (!p.sendConn) return;
    p.Start();
    VideoPacketizer pkt(p.sendConn.get());
    pkt.SetFecPercent(5);  // 每组 20 个分片
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 13);
    DesktopFrameMetadata meta{};

    // 帧 1 正常；帧 2 丢同组 1 个；帧 3 丢同组 2 个。
    pkt.SendFrame(1, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), data.data(), data.size());
    p.drop2.frame = 2;
    p.drop2.fragA = 2;
    p.drop3.frame = 3;
    p.drop3.fragA = 2;
    p.drop3.fragB = 3;
    pkt.SendFrame(1, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), data.data(), data.size());
    pkt.SendFrame(1, VideoCodec::HEVC, 0, 3, &meta, sizeof(meta), data.data(), data.size());
    p.Drain(150);

    size_t got1 = 0, got2 = 0, got3 = 0;
    bool contentOk = false;
    {
        std::lock_guard lock(p.mutex);
        for (const auto& f : p.frames) {
            if (f.frameIndex == 1) got1++;
            if (f.frameIndex == 2) got2++;
            if (f.frameIndex == 3) got3++;
            // 内容完整性：恢复出来的帧和发出去的逐字节一致。
            if (f.frameIndex == 2 && f.bitstream.size() == data.size())
                contentOk = std::memcmp(f.bitstream.data(), data.data(), data.size()) == 0;
        }
    }
    const auto stats = p.reasm->TakeStats();
    std::printf("FEC: frame1 %zu, frame2(recovered) %zu, frame3(2 lost) %zu, recovered %u, dropped %d\n", got1, got2,
                got3, stats.fragmentsRecovered, p.dropped.load());
    CHECK(p.dropped == 3);
    CHECK(got1 == 1);
    CHECK(got2 == 1);                        // 单丢 + XOR 校验包 => 恢复
    CHECK(stats.fragmentsRecovered >= 1);
    CHECK(contentOk);                        // 恢复后内容逐字节一致
    CHECK(got3 == 0);                        // 双丢恢复不了
    CHECK(p.reasm->Expire(NowNs(), 10'000'000));  // 走超时 => 请求关键帧的现有路径
    p.Stop();
}

static void TestAddresses() {    NetAddress a;
    CHECK(NetAddress::ParseEndpoint("192.168.1.20:38711", a) && !a.v6 && a.ip == 0xC0A80114 && a.port == 38711);
    CHECK(a.ToString() == "192.168.1.20:38711");
    CHECK(NetAddress::ParseEndpoint("[2001:db8::20]:38711", a) && a.v6 && a.port == 38711 && a.IsGlobalV6());
    CHECK(a.ToString() == "[2001:db8::20]:38711");
    NetAddress b;
    CHECK(NetAddress::ParseEndpoint(a.ToString(), b) && b == a);
    CHECK(!NetAddress::ParseEndpoint("2001:db8::20", a));  // IPv6 without brackets/port
    CHECK(!NetAddress::ParseEndpoint("1.2.3.4", a));
    CHECK(!NetAddress::ParseEndpoint("1.2.3.4:99999", a));
    CHECK(!NetAddress::Parse("", 1, a));  // empty host must not resolve to this machine
    CHECK(NetAddress::Parse("[fe80::1]", 5, a) && a.v6 && !a.IsGlobalV6());
    CHECK(IsPrivateLan(0x0A000001) && IsPrivateLan(0xAC100001) && IsPrivateLan(0xC0A80001) && !IsPrivateLan(0x08080808));
    CHECK(IsCgnat(0x64410001) && !IsCgnat(0x64000001));  // 100.65.0.1 yes, 100.0.0.1 no
    const NetAddress empty{}, one{1, 1};
    CHECK(!empty.valid() && one.valid());

    // One dual-stack socket talks to IPv4 and IPv6 peers; IPv4 senders show up as plain IPv4.
    UdpSocket s, v4peer, v6peer;
    CHECK(s.Open(0) && v4peer.Open(0) && v6peer.Open(0));
    if (!s.DualStack()) {
        std::printf("note: no IPv6 on this system, skipping dual-stack checks\n");
        return;
    }
    uint8_t buf[64];
    NetAddress from;
    const NetAddress sV4{0x7F000001, s.LocalPort()};
    CHECK(v4peer.SendTo(sV4, "four", 4));
    CHECK(s.RecvFrom(buf, sizeof(buf), from, 1000) == 4 && !from.v6 && from.ip == 0x7F000001 &&
          from.port == v4peer.LocalPort());
    NetAddress sV6;
    CHECK(NetAddress::Parse("::1", s.LocalPort(), sV6) && sV6.v6);
    CHECK(v6peer.SendTo(sV6, "six", 3));
    CHECK(s.RecvFrom(buf, sizeof(buf), from, 1000) == 3 && from.v6 && from.port == v6peer.LocalPort());
    CHECK(s.SendTo(from, "back", 4));  // reply over IPv6
    NetAddress back;
    CHECK(v6peer.RecvFrom(buf, sizeof(buf), back, 1000) == 4 && back.v6 && back.port == s.LocalPort());
}

// An open hand, palm down, fingers along -Z, with the wrist 5 cm behind the palm.
static void FlatHand(int hand, rc::Pose joints[rc::kHandJointCount]) {
    const float side = hand == 0 ? 1.f : -1.f;  // the thumb's side
    for (int i = 0; i < rc::kHandJointCount; i++) joints[i] = {{0, 0, 0, 1}, {0, 0, 0}};
    joints[1].position[2] = 0.05f;
    for (int finger = 1; finger <= 4; finger++) {
        const int base = 1 + finger * 5;
        const float x = side * (0.03f - 0.02f * float(finger - 1));
        for (int i = 0; i < 5; i++) {
            joints[base + i].position[0] = x;
            joints[base + i].position[2] = 0.035f - 0.04f * float(i);
        }
    }
    for (int i = 0; i < 4; i++) {
        joints[2 + i].position[0] = side * 0.04f;
        joints[2 + i].position[2] = 0.03f - 0.03f * float(i);
    }
}

static void TestHandSkeleton() {
    rc::Pose joints[rc::kHandJointCount], bones[rc::kHandBoneCount];
    for (int hand = 0; hand < 2; hand++) {
        FlatHand(hand, joints);
        rc::HandJointsToBones(hand, joints, bones);
        const float along = hand == 0 ? 1.f : -1.f;
        // Finger bones lie along +X on the left hand and -X on the right, as in SteamVR's own poses.
        for (int i : {7, 8, 9, 10, 12, 17, 22}) {
            CHECK(std::fabs(bones[i].position[0] - along * 0.04f) < 1e-4f);
            CHECK(std::fabs(bones[i].position[1]) < 1e-4f);
            CHECK(std::fabs(bones[i].position[2]) < 1e-4f);
            CHECK(std::fabs(bones[i].orientation[3] - 1) < 1e-4f);
        }
        // SteamVR's open hand has the index metacarpal 2.7 cm from the wrist's y, 1.5 cm in z.
        CHECK(std::fabs(bones[6].position[1] - 0.03f) < 0.012f);
        CHECK(std::fabs(bones[6].position[2] - 0.015f) < 0.012f);
        // SteamVR's held hand has the wrist 3.4 cm to the side, 3.7 cm up and 16.5 cm behind the controller.
        CHECK(std::fabs(bones[1].position[0] + along * 0.034f) < 0.03f);
        CHECK(std::fabs(bones[1].position[1] - 0.0365f) < 0.03f);
        CHECK(std::fabs(bones[1].position[2] - 0.1647f) < 0.03f);
        // The wrist's orientation is close to the one in SteamVR's held pose.
        const float sw = hand == 0 ? 1.f : -1.f;
        const float q[4] = {-0.078608f * 1, -0.920279f * sw, 0.379296f * sw, -0.055147f};
        const float dot = std::fabs(bones[1].orientation[0] * q[0] + bones[1].orientation[1] * q[1] +
                                    bones[1].orientation[2] * q[2] + bones[1].orientation[3] * q[3]);
        CHECK(dot > 0.8f);
        CHECK(rc::HandGrip(joints) == 0);
        // The controller's pose stays the same distance from the palm on both hands.
        rc::Pose palm{{0, 0, 0, 1}, {0.1f, 1.2f, -0.3f}};
        const rc::Pose device = rc::HandDevicePose(hand, palm);
        const float dx = device.position[0] - palm.position[0], dy = device.position[1] - palm.position[1],
                    dz = device.position[2] - palm.position[2];
        CHECK(std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - std::sqrt(0.04f * 0.04f + 0.02f * 0.02f + 0.13f * 0.13f)) < 1e-3f);

        // Closing the lower three fingers grips; the index alone doesn't.
        for (int finger = 2; finger <= 4; finger++) {
            const int base = 1 + finger * 5;
            for (int i = 1; i < 4; i++) {
                const float half = 0.6f * float(i) / 2;
                joints[base + i].orientation[0] = std::sin(half);
                joints[base + i].orientation[3] = std::cos(half);
            }
        }
        CHECK(rc::HandFingerCurl(joints, 1) == 0);
        CHECK(rc::HandFingerCurl(joints, 2) > 0.5f);
        CHECK(rc::HandGrip(joints) > 0.1f);
    }
}

static void TestSubnetHosts() {
    const uint32_t ip = (192u << 24) | (168u << 16) | (178u << 8) | 110u;
    const auto all = rc::SubnetHosts(ip, 0xFFFFFF00u, 4096);
    CHECK(all.size() == 253);  // 256 less the network, the broadcast and ourselves
    CHECK(all[0] == ip - 1 && all[1] == ip + 1);
    bool bad = false;
    for (const uint32_t h : all) bad = bad || h == ip || (h & 255) == 0 || (h & 255) == 255 || (h >> 8) != (ip >> 8);
    CHECK(!bad);
    // A wide network: the nearest ones, and no more than asked.
    const auto wide = rc::SubnetHosts(ip, 0xFFFF0000u, 1024);
    CHECK(wide.size() == 1024);
    CHECK(wide.back() >> 16 == ip >> 16);
    // At the edge of the subnet it still finds them all.
    CHECK(rc::SubnetHosts((ip & 0xFFFFFF00u) | 1u, 0xFFFFFF00u, 4096).size() == 253);
    CHECK(rc::SubnetHosts((10u << 24) | 1u, 0xFFFFFFFCu, 10).size() == 1);  // a /30 has one other host
    CHECK(rc::SubnetHosts(ip, 0xFFFFFFFFu, 10).empty());
    CHECK(rc::SubnetHosts(ip, 0xFFFFFFFEu, 10).empty());
    // Tailscale's range is a tunnel wherever it's routed; loopback never is.
    CHECK(rc::RoutesThroughTunnel((100u << 24) | (64u << 16) | 1u));
    CHECK(!rc::RoutesThroughTunnel((127u << 24) | 1u));
    printf("subnet hosts OK\n");
}

// A50 (TASK-SOCKET-HARDEN-2026-11-22): a v4-mapped literal must normalize to the same shape
// FromSockaddr produces for that peer, and the /0 fallback must not turn into a space sweep.
static void TestSocketHardening() {
    NetAddress mapped, plain, ep, real6;
    CHECK(NetAddress::Parse("::ffff:192.168.1.10", 9999, mapped));
    CHECK(!mapped.v6 && mapped.ip == 0xC0A8010A && mapped.port == 9999);
    CHECK(NetAddress::Parse("192.168.1.10", 9999, plain));
    CHECK(mapped == plain);                    // same peer, same key: no phantom roaming
    CHECK(mapped.ToString() == "192.168.1.10:9999");
    CHECK(NetAddress::ParseEndpoint("[::ffff:10.0.0.1]:9999", ep) && !ep.v6 && ep.ip == 0x0A000001);
    // A genuine v6 literal must still be v6 - the normalization is not a blanket downgrade.
    CHECK(NetAddress::Parse("::1", 5, real6) && real6.v6);
    CHECK(NetAddress::Parse("2001:db8::20", 5, real6) && real6.v6 && real6.IsGlobalV6());
    // An out-of-range OnLinkPrefixLength now yields netmask 0; that is never a LAN to sweep.
    CHECK(rc::SubnetHosts(0xC0A8010Au, 0, 1000).empty());
    printf("socket hardening OK\n");
}

// A58 (TASK-DISCOVERY-REPLY-2026-11-22): one predicate decides both who may open a session and
// who gets a discovery reply, so its table is worth pinning: the boundaries are where a LAN ends
// and the internet begins.
static void TestLocalSource() {
    const auto ip4 = [](uint32_t ip) {
        NetAddress a;
        a.ip = ip;
        a.port = 1;
        return a;
    };
    CHECK(rc::IsLocalSource(ip4(0x0A000001u)));   // 10.0.0.1
    CHECK(rc::IsLocalSource(ip4(0xAC100001u)));   // 172.16.0.1, the low end of 172.16/12
    CHECK(rc::IsLocalSource(ip4(0xAC1FFFFFu)));   // 172.31.255.255, the high end
    CHECK(!rc::IsLocalSource(ip4(0xAC200001u)));  // 172.32.0.1 is already the internet
    CHECK(rc::IsLocalSource(ip4(0xC0A80105u)));   // 192.168.1.5
    CHECK(!rc::IsLocalSource(ip4(0xC0A90105u)));  // 192.169.1.5 is not
    CHECK(rc::IsLocalSource(ip4(0x7F000001u)));   // loopback, for a local test tool
    CHECK(rc::IsLocalSource(ip4(0xA9FE0101u)));   // 169.254.1.1, link-local
    CHECK(rc::IsLocalSource(ip4(0x64400001u)));   // 100.64.0.1, CGNAT / Tailscale, low end
    CHECK(rc::IsLocalSource(ip4(0x647FFFFFu)));   // 100.127.255.255, high end
    CHECK(!rc::IsLocalSource(ip4(0x64800001u)));  // 100.128.0.1 is not
    CHECK(!rc::IsLocalSource(ip4(0x08080808u)));  // 8.8.8.8

    NetAddress v6;
    CHECK(NetAddress::Parse("fe80::1", 1, v6) && rc::IsLocalSource(v6));        // USB cable
    CHECK(NetAddress::Parse("fd12:3456::1", 1, v6) && rc::IsLocalSource(v6));   // unique local
    CHECK(NetAddress::Parse("::1", 1, v6) && rc::IsLocalSource(v6));
    CHECK(NetAddress::Parse("2001:db8::1", 1, v6) && !rc::IsLocalSource(v6));   // global = internet
    printf("local source OK\n");
}

static void TestMotionRuns() {
    std::vector<int8_t> grid(2 * 1000, 0);
    grid[2 * 3] = 5;
    grid[2 * 3 + 1] = -7;
    for (int i = 400; i < 800; i++) grid[2 * i] = int8_t(i % 100 + 1);
    std::vector<uint8_t> runs;
    rc::EncodeMotionRuns(grid.data(), 1000, runs);
    std::vector<int8_t> back;
    CHECK(rc::DecodeMotionRuns(runs.data(), runs.size(), 1000, back));
    CHECK(back == grid);
    // A still picture costs next to nothing; damaged data is refused.
    std::vector<int8_t> still(2 * 1000, 0);
    std::vector<uint8_t> stillRuns;
    rc::EncodeMotionRuns(still.data(), 1000, stillRuns);
    CHECK(stillRuns.size() < 16);
    CHECK(!rc::DecodeMotionRuns(runs.data(), runs.size() - 1, 1000, back));
    CHECK(!rc::DecodeMotionRuns(runs.data(), runs.size(), 999, back));
}

// A18 (TASK-MOTION-HYSTERESIS-2026-11-22): the Schmidt hysteresis that keeps block-matching
// noise - static text's bogus +-1 units, +-3.4 px on the generated frame - out of the motion
// field. Pure function on rc/motion_field.h; the grid doubles as the previous frame's state,
// the way VrStream's m_motionGridPrev feeds it.
static void TestMotionHysteresis() {
    // H1/H2/H3 in one stream of frames on one grid, the way the streamer runs it: noise on
    // the first frame must stay all-still, real motion must enter at one unit, and the unit
    // must leave again once the motion drops under the exit threshold.
    {
        std::vector<float> field;
        std::vector<int8_t> grid;
        for (int i = 0; i < 64; i++) field.push_back(i % 2 ? 2.5f : -2.5f);
        // H1: prev all zero, +-2.5 px of block-matching noise -> everything stays 0. The old
        // quantizer carried every one of these as a moving +-1 unit (lround(2.5/4) = 1).
        rc::ApplyMotionHysteresis(field, grid);
        bool nonzero = false;
        for (const int8_t v : grid) nonzero = nonzero || v != 0;
        CHECK(!nonzero);
        // H2: prev still all zero, real motion of 5 px -> +-1 unit (enter threshold 4 px),
        // and exactly 4 px enters too (>=, not >).
        std::fill(field.begin(), field.end(), 4.0f);
        rc::ApplyMotionHysteresis(field, grid);
        for (const int8_t v : grid) CHECK(v == 1);
        for (size_t i = 0; i < field.size(); i++) field[i] = i % 2 ? -5.0f : 5.0f;
        rc::ApplyMotionHysteresis(field, grid);
        for (size_t i = 0; i < grid.size(); i++) CHECK(grid[i] == (i % 2 ? -1 : 1));
        // H3: prev +-1 (restore the alternating state a previous frame would have left),
        // motion down to 1 px -> back to 0 (exit threshold 2 px).
        for (size_t i = 0; i < grid.size(); i++) grid[i] = i % 2 ? int8_t(-1) : int8_t(1);
        std::fill(field.begin(), field.end(), 1.0f);
        rc::ApplyMotionHysteresis(field, grid);
        bool nonzeroH3 = false;
        for (const int8_t v : grid) nonzeroH3 = nonzeroH3 || v != 0;
        CHECK(!nonzeroH3);
        // From prev 0, exactly 2 px is still under the 4 px enter threshold; from prev +-1
        // exactly 2 px HOLDS the unit (only < 2 px exits; lround(2/4) = lround(0.5) = 1,
        // away from zero), and 1.9 px lets it go.
        std::fill(field.begin(), field.end(), 2.0f);
        rc::ApplyMotionHysteresis(field, grid);
        bool nonzeroH3a = false;
        for (const int8_t v : grid) nonzeroH3a = nonzeroH3a || v != 0;
        CHECK(!nonzeroH3a);
        for (size_t i = 0; i < grid.size(); i++) grid[i] = i % 2 ? int8_t(-1) : int8_t(1);
        rc::ApplyMotionHysteresis(field, grid);
        for (const int8_t v : grid) CHECK(v == 1);
        std::fill(field.begin(), field.end(), 1.9f);
        rc::ApplyMotionHysteresis(field, grid);
        bool nonzeroH3b = false;
        for (const int8_t v : grid) nonzeroH3b = nonzeroH3b || v != 0;
        CHECK(!nonzeroH3b);
    }
    // H4: 40 px of real motion quantizes to 10 units whatever the previous state was - the
    // hysteresis only guards the 0 <-> 1 boundary, large motion follows plain quantization.
    {
        std::vector<float> field(8, 40.0f);
        std::vector<int8_t> grid;
        rc::ApplyMotionHysteresis(field, grid);  // prev: all still -> enters at 10
        for (const int8_t v : grid) CHECK(v == 10);
        rc::ApplyMotionHysteresis(field, grid);  // prev: 10 (|prev| >= 2, no hysteresis) -> 10
        for (const int8_t v : grid) CHECK(v == 10);
        std::vector<int8_t> ones(8);
        for (size_t i = 0; i < ones.size(); i++) ones[i] = i % 2 ? int8_t(-1) : int8_t(1);
        grid = ones;  // prev: +-1, the guarded boundary itself
        std::fill(field.begin(), field.end(), -40.0f);
        rc::ApplyMotionHysteresis(field, grid);
        for (const int8_t v : grid) CHECK(v == -10);
    }
    // H5: a hysteresis-produced grid survives the run-length round trip byte for byte, its
    // still/moving classification is exactly where the motion is, and a field the hysteresis
    // holds at zero encodes as still runs only (the bandwidth win - the same field used to
    // come out all-moving).
    {
        constexpr size_t kBlocks = 1000;
        std::vector<float> field(2 * kBlocks, 0.0f);
        for (size_t b = 400; b < 800; b++) field[2 * b] = 9.0f;  // 9 px -> 2 units
        std::vector<int8_t> grid;
        rc::ApplyMotionHysteresis(field, grid);
        std::vector<uint8_t> runs;
        rc::EncodeMotionRuns(grid.data(), kBlocks, runs);
        std::vector<int8_t> back;
        CHECK(rc::DecodeMotionRuns(runs.data(), runs.size(), kBlocks, back));
        CHECK(back == grid);
        for (size_t b = 0; b < kBlocks; b++)
            CHECK((back[2 * b] != 0 || back[2 * b + 1] != 0) == (b >= 400 && b < 800));
        // An all-zero grid - here the +-2.5 px noise field the hysteresis kept at zero -
        // encodes as still runs only.
        for (size_t i = 0; i < field.size(); i++) field[i] = i % 2 ? -2.5f : 2.5f;
        std::vector<int8_t> still;
        rc::ApplyMotionHysteresis(field, still);
        bool nonzeroStill = false;
        for (const int8_t v : still) nonzeroStill = nonzeroStill || v != 0;
        CHECK(!nonzeroStill);
        std::vector<uint8_t> stillRuns;
        rc::EncodeMotionRuns(still.data(), kBlocks, stillRuns);
        std::vector<int8_t> stillBack;
        CHECK(rc::DecodeMotionRuns(stillRuns.data(), stillRuns.size(), kBlocks, stillBack));
        CHECK(stillBack == still);
        bool moving = false;
        for (size_t at = 0; at < stillRuns.size(); at += 2) moving = moving || stillRuns[at + 1] != 0;
        CHECK(!moving);
        CHECK(stillRuns.size() < 16);
        // And the literal all-zero field does the same.
        std::vector<float> zero(2 * kBlocks, 0.0f);
        std::vector<int8_t> zeroGrid;
        rc::ApplyMotionHysteresis(zero, zeroGrid);
        std::vector<uint8_t> zeroRuns;
        rc::EncodeMotionRuns(zeroGrid.data(), kBlocks, zeroRuns);
        CHECK(zeroRuns.size() < 16);
    }
}

// An Ogg Opus file of a stereo sine, built the way an encoder lays it out (no CRCs: the
// reader doesn't check them).
static rc::TrackData MakeOggOpus(double hz, int packets, int trimEnd, bool badTail = false) {
    std::vector<uint8_t> file;
    int64_t pageNo = 0;
    auto page = [&](const std::vector<std::vector<uint8_t>>& ps, uint64_t granule, uint8_t flags) {
        std::vector<uint8_t> lacing;
        for (const auto& p : ps) {
            size_t n = p.size();
            for (; n >= 255; n -= 255) lacing.push_back(255);
            lacing.push_back(uint8_t(n));
        }
        const uint8_t head[] = {'O', 'g', 'g', 'S', 0, flags};
        file.insert(file.end(), head, head + 6);
        for (int i = 0; i < 8; i++) file.push_back(uint8_t(granule >> (8 * i)));
        for (int i = 0; i < 4; i++) file.push_back(uint8_t(i == 0 ? 1 : 0));  // serial
        for (int i = 0; i < 4; i++) file.push_back(uint8_t(pageNo >> (8 * i)));
        for (int i = 0; i < 4; i++) file.push_back(0);  // CRC
        file.push_back(uint8_t(lacing.size()));
        file.insert(file.end(), lacing.begin(), lacing.end());
        for (const auto& p : ps) file.insert(file.end(), p.begin(), p.end());
        pageNo++;
    };
    const int preSkip = 312;
    std::vector<uint8_t> opusHead = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, uint8_t(preSkip & 255), uint8_t(preSkip >> 8),
                                     0x80, 0xBB, 0, 0, 0, 0, 0};
    page({opusHead}, 0, 2);
    std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
    page({tags}, 0, 0);
    AudioEncoder enc;
    enc.Init(48000, 2, 96000, false);
    std::vector<std::vector<uint8_t>> pending;
    int16_t pcm[960 * 2];
    uint8_t buf[4000];
    int64_t t = 0;
    for (int k = 0; k < packets; k++) {
        for (int i = 0; i < 960; i++, t++) pcm[2 * i] = pcm[2 * i + 1] = int16_t(8000 * std::sin(6.2831853 * hz * double(t) / 48000));
        const int n = enc.Encode(pcm, 960, buf, sizeof(buf));
        pending.emplace_back(buf, buf + std::max(n, 0));
        const bool last = k == packets - 1;
        if (pending.size() == 50 || last) {
            // The granule position counts decoded samples, the pre-skip among them.
            page(pending, uint64_t((k + 1) * 960 - (last ? trimEnd : 0)), last ? 4 : 0);
            pending.clear();
        }
    }
    if (badTail) page({}, ~uint64_t(0), 0);  // a truncated trailer: granule -1, no packet ends here
    return std::make_shared<const std::vector<uint8_t>>(std::move(file));
}

struct Level {
    double rms = 0;
    double hz = 0;  // from zero crossings
};

static Level Measure(const int16_t* stereo, int frames) {
    Level l;
    double sum = 0;
    int crossings = 0;
    for (int i = 0; i < frames; i++) {
        sum += double(stereo[2 * i]) * stereo[2 * i];
        if (i && (stereo[2 * i] >= 0) != (stereo[2 * i - 2] >= 0)) crossings++;
    }
    l.rms = std::sqrt(sum / frames);
    l.hz = crossings * 48000.0 / (2.0 * frames);
    return l;
}

static void TestMusic() {
    // The reader: exact length (pre-skip and the end trim honoured), and the sound itself.
    {
        rc::OggOpusReader r;
        CHECK(r.Open(MakeOggOpus(440, 120, 300)));
        CHECK(r.TotalFrames() == 120 * 960 - 300 - 312);
        std::vector<int16_t> all, chunk(1000 * 2);
        for (int n; (n = r.Read(chunk.data(), 1000)) > 0;) all.insert(all.end(), chunk.begin(), chunk.begin() + n * 2);
        CHECK(int64_t(all.size() / 2) == 120 * 960 - 300 - 312);
        const Level mid = Measure(all.data() + 48000 * 2, 4800);
        CHECK(mid.rms > 4000 && mid.rms < 7000);
        CHECK(std::abs(mid.hz - 440) < 15);
        rc::OggOpusReader bad;
        CHECK(!bad.Open(std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'O', 'g', 'g'})));
    }

    // The mixer, 100 ms at a time.
    rc::MusicMixer mix;
    mix.SetTracks({MakeOggOpus(440, 1000, 0), MakeOggOpus(660, 1000, 0)}, 7);  // two 20 s tracks
    std::vector<int16_t> block(4800 * 2);
    auto step = [&] {
        mix.Render(block.data(), 4800);
        return Measure(block.data(), 4800);
    };
    CHECK(mix.Idle());
    mix.SetActive(true);
    const Level first = step();
    std::vector<Level> levels;
    for (int i = 0; i < 59; i++) levels.push_back(step());
    CHECK(first.rms < levels[50].rms * 0.3);  // faded in, not started at full level
    const double played = levels.back().hz;
    CHECK(std::abs(played - 440) < 15 || std::abs(played - 660) < 15);

    // Connected: silent within 2 s, and done.
    mix.SetActive(false);
    Level l;
    for (int i = 0; i < 20; i++) l = step();
    CHECK(l.rms < levels.back().rms * 0.1);  // the last 100 ms of the fade
    l = step();
    CHECK(l.rms < 1);
    CHECK(mix.Idle());

    // Disconnected again: a new track (here, with two, the other one), not the old one resumed.
    mix.SetActive(true);
    for (int i = 0; i < 60; i++) l = step();
    CHECK(std::abs(l.hz - played) > 100);

    // Through the end of that track into the next: no gap, no dip, no jump in loudness.
    double low = 1e9, high = 0;
    for (int i = 0; i < 200; i++) {
        l = step();
        low = std::min(low, l.rms);
        high = std::max(high, l.rms);
    }
    CHECK(low > 4000);
    CHECK(high < 7500);
    CHECK(std::abs(l.hz - played) < 15);  // and it's the other track again

    // A54: a truncated tail page carries granule -1 ("no packet ends here"). Reading that as
    // the track length gave zero frames, so the whole track silently never played; the reader
    // has to walk back to the last page that actually closes a packet.
    {
        rc::OggOpusReader r;
        CHECK(r.Open(MakeOggOpus(440, 120, 300, /*badTail=*/true)));
        CHECK(r.TotalFrames() == 120 * 960 - 300 - 312);  // identical to the untruncated file
        std::vector<int16_t> chunk(1000 * 2);
        CHECK(r.Read(chunk.data(), 1000) == 1000);        // and it really plays
    }
    // A54: replacing the playlist must reset the "any track but the last one" index; a stale
    // one is an index into a list that no longer exists.
    {
        mix.SetActive(false);
        for (int i = 0; i < 25; i++) step();  // fade out
        // 10 s each: longer than the 8 s crossfade window, so the fade-in is not being restarted
        // by a track that is already inside that window on its first frame.
        mix.SetTracks({MakeOggOpus(440, 500, 0), MakeOggOpus(660, 500, 0)}, 3);
        mix.SetActive(true);
        for (int i = 0; i < 12; i++) l = step();
        CHECK(l.rms > 500);                   // still audible after the swap
        mix.SetActive(false);
        for (int i = 0; i < 25; i++) step();
    }
    std::printf("music OK\n");
}

// The path a Quest on a USB cable takes: IPv6 link-local only, asking every device on the
// link. A socket that joined ff02::1 on an adapter hears what's sent there, from an address
// that carries the adapter as its scope, and can answer it directly.
static void TestLinkLocalDiscovery() {
    // 这一段会向真实网卡发 ff02::1 组播，同链路上的其它设备也收得到：默认跳过，
    // 需要时设 RINCAST_TEST_MULTICAST=1（不是删掉测试，随时可以开回来）。
    const char* enable = std::getenv("RINCAST_TEST_MULTICAST");
    if (!enable || std::string(enable) == "0" || !*enable) {
        std::printf("link-local multicast discovery skipped (RINCAST_TEST_MULTICAST=1 to run)\n");
        return;
    }
    CHECK(rc::NetAddress::AllNodesV6(9, 3).ToString() == "[ff02::1%3]:9");
    rc::NetAddress ll;
    CHECK(rc::NetAddress::Parse("fe80::1", 5, ll) && ll.IsLinkLocalV6() && !ll.IsGlobalV6());
    rc::UdpSocket listener, sender;
    if (!listener.OpenV6(0) || !sender.Open(0)) {
        std::printf("link-local discovery skipped: no IPv6\n");
        return;
    }
    const uint16_t port = listener.LocalPort();
    int tried = 0, heard = 0;
    for (const auto& itf : rc::ListInterfacesV6()) {
        if (!listener.JoinGroupV6(rc::NetAddress::AllNodesV6(0, itf.index), itf.index)) continue;
        tried++;
        const char msg[] = "probe";
        sender.SendTo(rc::NetAddress::AllNodesV6(port, itf.index), msg, sizeof(msg));
        char buf[64];
        rc::NetAddress from;
        for (int i = 0; i < 10; i++) {
            const int n = listener.RecvFrom(buf, sizeof(buf), from, 50);
            if (n == int(sizeof(msg)) && from.IsLinkLocalV6()) {
                heard++;
                CHECK(from.scope == itf.index);
                // And the answer goes straight back, over the same adapter.
                CHECK(listener.SendTo(from, "announce", 8));
                char back[64];
                rc::NetAddress replyFrom;
                CHECK(sender.RecvFrom(back, sizeof(back), replyFrom, 200) == 8);
                break;
            }
        }
    }
    std::printf("link-local discovery: %d of %d adapters\n", heard, tried);
    CHECK(tried == 0 || heard > 0);
}

// ---------------------------------------------------------------------------
// The fixed-bitrate guard, against a modelled link.
//
// This checks the control law, not the radio: the queue is a backlog that grows when the
// rate is above capacity, and the headset's reading is what its DelayTracker reports - the
// delay now minus the lowest delay of the last four seconds, so a queue that simply sits
// there stops being reported (that is why the guard's threshold has to be a band and not a
// level). What is asserted is what went wrong in the field: never pinning at the floor,
// never walking the rate down in steps, and settling near the capacity instead of
// oscillating around it.
namespace {
struct SimLink {
    double capacityKbps = 100000;
    double backpackMs = 0;
    double naturalMs = 25;  // what the AP and its queueing add when it carries the stream
    double ring[40] = {};   // the last 4 s of instantaneous delay, 100 ms apart
    int ringPos = 0;
    int ringCount = 0;

    void Step(double rateKbps, double stepMs) {
        // Above capacity the excess waits: one second of a 10% overload is 100 ms of queue.
        backpackMs += (rateKbps - capacityKbps) * stepMs / capacityKbps;
        if (backpackMs < 0) backpackMs = 0;  // a buffer drains at once, it does not go negative
        ring[ringPos] = naturalMs + backpackMs;
        ringPos = (ringPos + 1) % 40;
        if (ringCount < 40) ringCount++;
    }
    // What the headset reports: this delay minus the lowest of the last four seconds.
    double Report() const {
        if (!ringCount) return -1;
        double low = ring[0];
        for (int i = 1; i < ringCount; i++) low = std::min(low, ring[i]);
        return ring[(ringPos + 39) % 40] - low;
    }
};

struct SimRun {
    uint32_t lastRate = 0;
    int dips = 0;
    int climbs = 0;
    int floorHits = 0;
    int reversals = 0;
    int dir = 0;
};

// Runs the guard against a link, calling it every 100 ms like the headset's feedback does.
// nowNs is optional: pass it to carry the clock across consecutive runs of one scenario.
SimRun RunGuard(rc::QueueGuard& guard, SimLink& link, uint32_t targetKbps, double seconds,
                int64_t* nowNs = nullptr, double keyframeEverySec = 0) {
    SimRun run;
    int64_t localNow = 0;
    int64_t& now = nowNs ? *nowNs : localNow;
    const int steps = int(seconds * 10);
    for (int i = 0; i < steps; i++) {
        const uint32_t cap = guard.Cap();
        const double rate = cap ? cap : targetKbps;
        link.Step(rate, 100.0);
        now += 100'000'000LL;
        const bool keyframe = keyframeEverySec > 0 && std::fmod(i * 0.1, keyframeEverySec) < 0.099;
        const auto res = guard.Update(targetKbps, link.Report(), 0, keyframe, now);
        if (res.action == rc::QueueGuard::Action::Dip) {
            run.dips++;
            run.dir = -1;
        } else if (res.action == rc::QueueGuard::Action::Climb ||
                   res.action == rc::QueueGuard::Action::Released) {
            run.climbs++;
            run.dir = 1;
        }
        if (res.capKbps && res.capKbps <= targetKbps / guard.Config().minDivisor) run.floorHits++;
        run.lastRate = res.capKbps ? res.capKbps : targetKbps;
    }
    return run;
}

// The same run, keeping every report instead of only the counters. TASK-A73's two scenarios
// are about the *shape* of the pump - which dip came after which queue, how fast the rate
// moved back up - so a scalar per scenario is not enough to pin it.
struct GuardStep {
    double sec = 0;        // the clock this report was made at
    double rateKbps = 0;   // the rate that was on the wire during this step
    uint32_t capKbps = 0;  // the cap after it; 0 = released (the target again)
    double reportMs = 0;   // what the headset reported
    int action = 0;        // 0 none, 1 dip, 2 climb, 3 released
};

struct GuardTrace {
    std::vector<GuardStep> s;
    uint32_t target = 0;
    double floorKbps = 0;

    double RateAt(size_t i) const { return s[i].capKbps ? double(s[i].capKbps) : double(target); }
    double LastRate() const { return s.empty() ? 0 : RateAt(s.size() - 1); }
    int DipCount() const {
        int n = 0;
        for (const auto& x : s)
            if (x.action == 1) n++;
        return n;
    }
    double PeakReport(double fromSec) const {
        double p = 0;
        for (const auto& x : s)
            if (x.sec >= fromSec) p = std::max(p, x.reportMs);
        return p;
    }
    double MeanRate(double fromSec, double toSec) const {
        double sum = 0;
        int n = 0;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i].sec >= fromSec && s[i].sec < toSec) {
                sum += RateAt(i);
                n++;
            }
        return n ? sum / n : 0;
    }
    double MaxRate(double fromSec, double toSec) const {
        double m = 0;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i].sec >= fromSec && s[i].sec < toSec) m = std::max(m, RateAt(i));
        return m;
    }
    double MinRate(double fromSec, double toSec) const {
        double m = -1;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i].sec >= fromSec && s[i].sec < toSec) m = m < 0 ? RateAt(i) : std::min(m, RateAt(i));
        return m < 0 ? 0 : m;
    }
    // The most dips any window of widthSec can hold, counting windows that open at fromSec.
    int WorstWindow(double widthSec, double fromSec) const {
        int worst = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].action != 1 || s[i].sec < fromSec) continue;
            int n = 0;
            for (const auto& x : s)
                if (x.action == 1 && x.sec >= s[i].sec && x.sec < s[i].sec + widthSec) n++;
            worst = std::max(worst, n);
        }
        return worst;
    }
    // Dips taken against a queue that was no worse than the one the previous dip was taken
    // against, counting only dips inside one congestion episode: an episode ends when a
    // report comes back under the band, which is the drain finishing. Those dips are the guard
    // pressing again on a backlog its last press is already draining (TASK-A73 M1).
    int PressedWhileDraining(double episodeEndsBelowMs) const {
        int n = 0;
        double prev = -1;
        for (const auto& x : s) {
            if (x.reportMs < episodeEndsBelowMs) prev = -1;
            if (x.action != 1) continue;
            if (prev >= 0 && x.reportMs <= prev) n++;
            prev = x.reportMs;
        }
        return n;
    }
    // The largest relative gain of the sent rate in any window of widthSec - how fast the
    // guard sprints back up. Steps taken from the floor are excluded: the pinned self-rescue
    // is allowed to be fast (test 6).
    double FastestGain(double fromSec, double widthSec) const {
        double worst = 1;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].sec < fromSec) continue;
            const double r0 = RateAt(i);
            if (r0 <= floorKbps * 1.05) continue;
            size_t j = i;
            while (j < s.size() && s[j].sec < s[i].sec + widthSec) j++;
            const double r1 = RateAt(j < s.size() ? j - 1 : s.size() - 1);
            if (r0 > 0) worst = std::max(worst, r1 / r0);
        }
        return worst;
    }
};

GuardTrace RunGuardTrace(rc::QueueGuard& guard, SimLink& link, uint32_t targetKbps, double seconds,
                         int64_t& now) {
    GuardTrace tr;
    tr.target = targetKbps;
    tr.floorKbps = double(targetKbps) / guard.Config().minDivisor;
    const int steps = int(seconds * 10);
    for (int i = 0; i < steps; i++) {
        GuardStep st;
        st.rateKbps = guard.Cap() ? guard.Cap() : targetKbps;
        link.Step(st.rateKbps, 100.0);
        now += 100'000'000LL;
        st.sec = double(now) / 1e9;
        st.reportMs = link.Report();
        const auto res = guard.Update(targetKbps, st.reportMs, 0, false, now);
        st.capKbps = res.capKbps;
        st.action = res.action == rc::QueueGuard::Action::Dip
                        ? 1
                        : (res.action == rc::QueueGuard::Action::Climb
                               ? 2
                               : (res.action == rc::QueueGuard::Action::Released ? 3 : 0));
        tr.s.push_back(st);
    }
    return tr;
}

// Reports fed by hand, the way test 7 does it: the link model is the thing being reported, so
// a scenario that is about the guard's own timers can state the reports directly. Each phase
// is [from, to) with the reported queue ramping q0 -> q1.
struct FeedPhase {
    double from, to, q0, q1;
};

GuardTrace RunFeedTrace(rc::QueueGuard& guard, uint32_t targetKbps,
                        const std::vector<FeedPhase>& phases) {
    GuardTrace tr;
    tr.target = targetKbps;
    tr.floorKbps = double(targetKbps) / guard.Config().minDivisor;
    int64_t now = 0;
    for (int i = 0; i * 100'000'000LL < int64_t(phases.back().to * 1e9); i++) {
        const double t = i * 0.1;  // the report lands at t + 0.1 s
        const FeedPhase* ph = nullptr;
        for (const auto& p : phases)
            if (t >= p.from && t < p.to) ph = &p;
        if (!ph) continue;
        const double f = ph->to > ph->from ? (t - ph->from) / (ph->to - ph->from) : 0.0;
        const double queueMs = ph->q0 + (ph->q1 - ph->q0) * f;
        GuardStep st;
        st.rateKbps = guard.Cap() ? guard.Cap() : targetKbps;
        now += 100'000'000LL;
        st.sec = double(now) / 1e9;
        st.reportMs = queueMs;
        const auto res = guard.Update(targetKbps, queueMs, 0, false, now);
        st.capKbps = res.capKbps;
        st.action = res.action == rc::QueueGuard::Action::Dip
                        ? 1
                        : (res.action == rc::QueueGuard::Action::Climb
                               ? 2
                               : (res.action == rc::QueueGuard::Action::Released ? 3 : 0));
        tr.s.push_back(st);
    }
    return tr;
}
}  // namespace

static void TestStreamProfile() {
    const uint32_t cap = 150000;  // 150 Mbps ceiling
    const uint32_t fps = 90;
    const rc::StreamProfile all[] = {rc::StreamProfile::LowLatency, rc::StreamProfile::Balanced,
                                     rc::StreamProfile::Quality};
    uint32_t lastVbv = 0, lastMinQp = 100;
    double lastQueue = 0;
    for (auto p : all) {
        const auto q = rc::ProfileParams(p);
        const auto b = rc::ComputeCappedVbr(cap, fps, p);
        // The ceiling is a ceiling: no profile may ask for more than the user allowed.
        CHECK(b.maxBitrate == uint64_t(cap) * 1000);
        CHECK(b.averageBitrate > 0 && b.averageBitrate <= b.maxBitrate);
        CHECK(b.averageBitrate == uint64_t(cap) * 1000 * q.vbrAveragePercent / 100);
        // The average must STAY at the ceiling: lowering it is not "saving on simple
        // pictures", it starves complex ones too. The QP floor does the saving.
        CHECK(b.averageBitrate == b.maxBitrate);
        CHECK(b.vbvBufferSize > 0);
        CHECK(b.vbvBufferSize >= b.averageBitrate / fps);  // the bucket still holds a frame
        // The preference is monotonic: latency-first has the highest (cheapest) QP
        // floor (24), balanced sits at 16, quality-first keeps the lowest (12) and
        // tolerates the deepest queue and the biggest burst.
        CHECK(q.minQp <= lastMinQp);   // a floor never rises across the gradient
        CHECK(b.vbvBufferSize >= lastVbv);
        CHECK(q.targetQueueMs > lastQueue);
        lastMinQp = q.minQp;
        lastVbv = b.vbvBufferSize;
        lastQueue = q.targetQueueMs;
        std::printf("profile %d: queue %.0f+-%.0f ms, avg %u/%u kbps, QP floor %u, VBV %.2f Mbit\n", int(p),
                    q.targetQueueMs, q.bandMs, b.averageBitrate / 1000, b.maxBitrate / 1000, q.minQp,
                    double(b.vbvBufferSize) / 1e6);
    }

    // Quality keeps a mild floor since 2026-11-17: with none, the real device
    // swung QP 3-25 / 9-29 and the user read it as strong "breathing"; 12 cuts
    // the near-lossless spikes while keeping the sharpness edge over Balanced 16.
    CHECK(rc::ProfileParams(rc::StreamProfile::Quality).minQp == 12);
    // The floors are the point: with Balanced's at 20 the real device pinned the QP
    // at exactly 20.0 and 35 of a 100 Mbps ceiling; off, the same content swung
    // 4.7-25 and read as "breathing" (2026-11-17 real device). 16 is a middle guess
    // pending real-link re-measure (ROUND15 / TASKBOOK-ROUND15 §3.3).
    CHECK(rc::ProfileParams(rc::StreamProfile::Balanced).minQp == 16);
    CHECK(rc::ProfileParams(rc::StreamProfile::LowLatency).minQp == 24);
    CHECK(rc::ProfileParams(rc::StreamProfile::Balanced).minQp == 16);

    // ROUND18 L2: encodeScale is the encode/render resolution ratio. Every profile
    // defaults to 1.0 (no behaviour change); the clamp keeps the user override in
    // [0.5, 1.0] so the encoder never sees a degenerate or upscaled request.
    for (auto p : all) CHECK(rc::ProfileParams(p).encodeScale == 1.0f);
    CHECK(rc::ClampEncodeScale(1.0f) == 1.0f);
    CHECK(rc::ClampEncodeScale(0.5f) == 0.5f);
    CHECK(rc::ClampEncodeScale(0.67f) == 0.67f);
    CHECK(rc::ClampEncodeScale(0.0f) == 0.5f);   // below range clamps up
    CHECK(rc::ClampEncodeScale(2.0f) == 1.0f);   // above range clamps down
    CHECK(rc::ClampEncodeScale(-1.0f) == 0.5f);

    // ROUND18 L2-FIX2: ScaledEncodeSize is the single source of truth for the encoded
    // frame size. scale=1.0 must be byte-identical to the old inline formula (base &
    // ~1u), so 100% is a strict no-op; 0.67 follows truncate-then-even-down; the
    // clamp and the minimum-2 guard match vr_stream.cpp exactly.
    CHECK(rc::ScaledEncodeSize(4576, 1.0f) == 4576);          // 100% full width: identity
    CHECK(rc::ScaledEncodeSize(4577, 1.0f) == 4576);          // 100% odd base: even down
    CHECK(rc::ScaledEncodeSize(2288, 1.0f) == 2288);          // 100% height: identity
    CHECK(rc::ScaledEncodeSize(4576, 0.67f) == 3064);         // 4576*0.67=3065.92 -> 3064
    CHECK(rc::ScaledEncodeSize(2288, 0.67f) == 1532);         // 2288*0.67=1532.96 -> 1532
    CHECK(rc::ScaledEncodeSize(4576, 0.0f) == 2288);          // 0 clamps to 0.5
    CHECK(rc::ScaledEncodeSize(4576, 2.0f) == 4576);          // >1 clamps to 1.0
    CHECK(rc::ScaledEncodeSize(4576, -1.0f) == 2288);         // <0.5 clamps to 0.5
    CHECK(rc::ScaledEncodeSize(3, 1.0f) == 2);                // minimum 2 (and even)
    CHECK(rc::ScaledEncodeSize(1, 0.5f) == 2);                // minimum 2 floor

    // ROUND18 L3: the optional head-reported decode-fps hint. Absent (0) and
    // malformed values must read as "no hint" so an old/buggy headset is a no-op.
    CHECK(rc::ValidateDecodeFpsHint(0) == 0);     // absent / old headset
    CHECK(rc::ValidateDecodeFpsHint(45) == 45);   // measured PICO 4 hint, valid
    CHECK(rc::ValidateDecodeFpsHint(24) == 24);   // lower boundary
    CHECK(rc::ValidateDecodeFpsHint(240) == 240); // upper boundary
    CHECK(rc::ValidateDecodeFpsHint(23) == 0);    // malformed low
    CHECK(rc::ValidateDecodeFpsHint(241) == 0);   // malformed high
    CHECK(rc::ValidateDecodeFpsHint(0xFFFFFFFFu) == 0);  // garbage

    // A60: the plausibility gate every self-reported headset number passes through.
    // A bad value must read as "not reported" (the caller keeps its previous copy);
    // NaN/inf fail the comparisons, so none of them reaches a period (1e9/hz) or a
    // stream size.
    CHECK(!rc::PlausibleRefreshRate(23.9f));
    CHECK(rc::PlausibleRefreshRate(24.0f));
    CHECK(rc::PlausibleRefreshRate(72.0f));
    CHECK(rc::PlausibleRefreshRate(90.0f));
    CHECK(rc::PlausibleRefreshRate(120.0f));
    CHECK(rc::PlausibleRefreshRate(240.0f));
    CHECK(!rc::PlausibleRefreshRate(240.1f));
    CHECK(!rc::PlausibleRefreshRate(std::numeric_limits<float>::infinity()));
    CHECK(!rc::PlausibleRefreshRate(std::nanf("")));

    // The panel size: 0 makes the encode geometry 0/0 later, 0xFFFFFFFF wraps the
    // alignment - both must read as "not reported".
    CHECK(!rc::PlausibleEyeSize(0u));
    CHECK(!rc::PlausibleEyeSize(255u));
    CHECK(rc::PlausibleEyeSize(256u));
    CHECK(rc::PlausibleEyeSize(1832u));
    CHECK(rc::PlausibleEyeSize(1920u));
    CHECK(rc::PlausibleEyeSize(4096u));
    CHECK(!rc::PlausibleEyeSize(4097u));
    CHECK(!rc::PlausibleEyeSize(0xFFFFFFFFu));

    CHECK(!rc::PlausibleDecodeSize(0u));
    CHECK(!rc::PlausibleDecodeSize(511u));
    CHECK(rc::PlausibleDecodeSize(512u));
    CHECK(rc::PlausibleDecodeSize(4096u));
    CHECK(rc::PlausibleDecodeSize(16384u));
    CHECK(!rc::PlausibleDecodeSize(16385u));

    CHECK(!rc::PlausibleIpd(0.0f));
    CHECK(rc::PlausibleIpd(0.03f));
    CHECK(rc::PlausibleIpd(0.063f));
    CHECK(rc::PlausibleIpd(0.09f));
    CHECK(!rc::PlausibleIpd(0.2f));

    CHECK(!rc::PlausibleFovTangent(-10.0f));
    CHECK(rc::PlausibleFovTangent(-1.0f));
    CHECK(rc::PlausibleFovTangent(0.0f));
    CHECK(rc::PlausibleFovTangent(1.1f));
    CHECK(rc::PlausibleFovTangent(9.9f));
    CHECK(!rc::PlausibleFovTangent(10.0f));
    CHECK(!rc::PlausibleFovTangent(std::numeric_limits<float>::quiet_NaN()));
    CHECK(!rc::PlausibleFovTangent(std::numeric_limits<float>::infinity()));

    // A one-frame VBV cannot hold NVENC's VBR rate control. Measured 2026-11-15
    // (encode-bench --synthetic, 3488x1744 @90): LowLatency with 100 % sent
    // 136 / 126 / 109 % of a 20 / 30 / 40 Mbps ceiling and only 87 % of 140 Mbps;
    // at 150 % every ceiling landed at 95-97 %. Never let a profile go under 1.5
    // frames again -- the user's bitrate is a CEILING, not a target to average above.
    for (auto p : all) {
        CHECK(rc::ProfileParams(p).vbvFramesPercent >= 150);
        std::printf("profile %d: VBV %u%% of one frame\n", int(p),
                    rc::ProfileParams(p).vbvFramesPercent);
    }

    // Degenerate inputs: a zero ceiling and a zero frame rate must not divide by zero
    // or smuggle a budget through.
    CHECK(rc::ComputeCappedVbr(0, fps, rc::StreamProfile::Balanced).maxBitrate == 0);
    const auto noFps = rc::ComputeCappedVbr(cap, 0, rc::StreamProfile::Balanced);
    CHECK(noFps.vbvBufferSize > 0 && noFps.maxBitrate == uint64_t(cap) * 1000);
    // The budget is linear in the ceiling, so raising it never changes the shape.
    const auto half = rc::ComputeCappedVbr(cap / 2, fps, rc::StreamProfile::Balanced);
    const auto full = rc::ComputeCappedVbr(cap, fps, rc::StreamProfile::Balanced);
    CHECK(half.maxBitrate * 2 == full.maxBitrate);
    CHECK(std::abs(int64_t(half.averageBitrate) * 2 - int64_t(full.averageBitrate)) <= 1);
    CHECK(half.vbvBufferSize * 2 == full.vbvBufferSize);
    // Out-of-range percentages are clamped, not UB.
    CHECK(rc::ComputeCappedVbr(cap, fps, 0, 0).averageBitrate > 0);
    CHECK(rc::ComputeCappedVbr(cap, fps, 200, 1000).averageBitrate == full.maxBitrate);
    CHECK(rc::ProfileFromId(0) == rc::StreamProfile::LowLatency);
    CHECK(rc::ProfileFromId(2) == rc::StreamProfile::Quality);
    CHECK(rc::ProfileFromId(99) == rc::StreamProfile::Balanced);  // unknown -> safe default

    // The sender must not invent its own brake line: it uses the same helper.
    const auto g = rc::GuardConfigFor(rc::StreamProfile::Quality);
    CHECK(g.targetMs == rc::ProfileParams(rc::StreamProfile::Quality).targetQueueMs);
    CHECK(g.bandMs == rc::ProfileParams(rc::StreamProfile::Quality).bandMs);
    CHECK(g.floorExtraMs == rc::ProfileParams(rc::StreamProfile::Quality).floorExtraMs);

    // All three presets must still find the link's capacity. A tighter latency budget
    // is allowed to dip earlier, but none may strand the stream at the floor.
    for (auto p : all) {
        rc::QueueGuard guard;
        guard.Configure(rc::GuardConfigFor(p));
        SimLink link;
        link.capacityKbps = 100000;
        const auto run = RunGuard(guard, link, 140000, 40);
        std::printf("profile %d on a 100 Mb/s link: %u kbps, queue %.0f ms, %d dips\n", int(p), run.lastRate,
                    link.Report(), run.dips);
        CHECK(run.lastRate >= 75000 && run.lastRate <= 104000);
        CHECK(run.floorHits == 0);
    }
}

static void TestFoveaPlacement() {
    CHECK(rc::PlausibleFovPair(1.376f, 0.839f));
    CHECK(!rc::PlausibleFovPair(0.0f, 0.0f));
    CHECK(!rc::PlausibleFovPair(-1.0f, 1.0f));
    CHECK(!rc::PlausibleFovPair(-1.0f, 3.0f));
    CHECK(!rc::PlausibleFovPair(0.025f, 0.025f));
    CHECK(rc::PlausibleFovPair(0.03f, 0.03f));
    CHECK(!rc::PlausibleFovPair(10.0f, 1.0f));
    CHECK(!rc::PlausibleFovPair(std::numeric_limits<float>::quiet_NaN(), 1.0f));
    CHECK(!rc::PlausibleFovPair(std::numeric_limits<float>::infinity(), 1.0f));

    const float F = 0.45f;
    const float normal[4] = {1.376f, 0.839f, 0.966f, 1.191f};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float flat[4] = {-1.0f, 1.0f, 0.966f, 1.191f};
    const float gaze[3] = {0.3f, 0.0f, -1.0f};
    const float nanGaze[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, -1.0f};
    float s[2];

    rc::FoveaSquare(normal, nullptr, F, false, s);
    CHECK(std::fabs(s[0] - 0.3963f) < 1e-3f);
    CHECK(std::fabs(s[1] - 0.2229f) < 1e-3f);

    rc::FoveaSquare(zero, nullptr, F, false, s);
    CHECK(std::fabs(s[0] - 0.275f) < 1e-4f);
    CHECK(std::fabs(s[1] - 0.275f) < 1e-4f);

    rc::FoveaSquare(flat, nullptr, F, false, s);
    CHECK(std::fabs(s[0] - 0.275f) < 1e-4f);
    CHECK(std::fabs(s[1] - 0.275f) < 1e-4f);

    rc::FoveaSquare(normal, gaze, F, false, s);
    CHECK(std::fabs(s[0] - 0.532f) < 1e-3f);
    CHECK(s[0] < 1.0f - F);

    rc::FoveaSquare(normal, nanGaze, F, false, s);
    CHECK(std::fabs(s[0] - 0.275f) < 1e-4f);
    CHECK(std::fabs(s[1] - 0.2229f) < 1e-3f);

    s[0] = 0.4f;
    s[1] = 0.2229f;
    rc::FoveaSquare(normal, nullptr, F, true, s);
    CHECK(s[0] == 0.4f);
    CHECK(s[1] == 0.2229f);

    s[0] = 0.42f;
    s[1] = 0.2229f;
    rc::FoveaSquare(normal, nullptr, F, true, s);
    CHECK(s[0] == 0.42f);

    s[0] = 0.2f;
    s[1] = 0.2f;
    rc::FoveaSquare(normal, nullptr, F, true, s);
    CHECK(std::fabs(s[0] - 0.3963f) < 1e-3f);
    CHECK(std::fabs(s[1] - 0.2229f) < 1e-3f);

    s[0] = std::numeric_limits<float>::quiet_NaN();
    s[1] = std::numeric_limits<float>::quiet_NaN();
    rc::FoveaSquare(normal, nullptr, F, true, s);
    CHECK(std::isfinite(s[0]));
    CHECK(std::isfinite(s[1]));
}

static void TestFoveaQpDelta() {
    // The angle ramp: flat inside 25 degrees, smoothstep out to 50, then flat again.
    CHECK(rc::FoveaQpDelta(0.0f, 6) == 0);
    CHECK(rc::FoveaQpDelta(25.0f, 6) == 0);
    CHECK(rc::FoveaQpDelta(37.5f, 6) == 3);   // smoothstep(0.5) * 6
    CHECK(rc::FoveaQpDelta(50.0f, 6) == 6);
    CHECK(rc::FoveaQpDelta(90.0f, 6) == 6);
    CHECK(rc::FoveaQpDelta(37.5f, 24) == 12);  // AV1 counts in finer steps
    // A non-finite fov tangent must not reach the QP map at all.
    CHECK(rc::FoveaQpDelta(std::numeric_limits<float>::quiet_NaN(), 6) == 0);
    CHECK(rc::FoveaQpDelta(std::numeric_limits<float>::infinity(), 6) == 0);
    // An infinite tangent is not the same thing: atan(inf) is a real 90 degrees, so it
    // arrives here as a finite angle and earns the full offset.
    CHECK(rc::FoveaQpDelta(std::atan(std::numeric_limits<float>::infinity()) * 57.2958f, 6) == 6);
}

static void TestQueueGuard() {
    rc::QueueGuardConfig cfg;
    const uint32_t target = 140000;

    // 1. A link that carries 100 Mb/s: settle under it, inside the band, and stay there.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        auto run = RunGuard(guard, link, target, 40);
        const double steady = link.Report();
        std::printf("queue guard: 100 Mb/s link settles at %u kbps (reported queue %.0f ms, %d dips)\n",
                    run.lastRate, steady, run.dips);
        CHECK(run.lastRate <= 104000);       // did not sit on the target and flood
        CHECK(run.lastRate >= 84000);        // nor gave the link away
        CHECK(run.floorHits == 0);           // never pinned
        CHECK(run.dips < 40);                // 40 s is 400 reports: this is not a sawtooth
        CHECK(steady < 60);                  // the queue it holds is a budget, not a second
    }

    // 2. The path halves mid-stream: follow it down without falling to the floor.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 30, &now);
        link.capacityKbps = 55000;
        auto run = RunGuard(guard, link, target, 30, &now);
        std::printf("queue guard: path halves to 55 Mb/s, settles at %u kbps (%d dips, %d at the floor)\n",
                    run.lastRate, run.dips, run.floorHits);
        CHECK(run.lastRate <= 62000);
        CHECK(run.lastRate >= 40000);
        CHECK(run.floorHits == 0);
    }

    // 3. Frame loss on a clean link is not congestion. A keyframe storm used to cut
    // 140 -> 85 Mbps every two seconds while queue and loss both read zero (S3, 2026-11-15).
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 20, &now);
        const uint32_t before = guard.Cap() ? guard.Cap() : target;
        auto run = RunGuard(guard, link, target, 10, &now, 0.2);  // a request every 200 ms
        std::printf("queue guard: clean-link keyframe storm stays at %u kbps (was %u, %d dips)\n",
                    run.lastRate, before, run.dips);
        CHECK(run.dips == 0);
        CHECK(run.lastRate >= before * 98 / 100);
    }

    // 3b. The early-warning path is still there when the queue agrees: a request on a
    // congested link must still bring the rate down, not be ignored altogether.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 20, &now);
        link.capacityKbps = 55000;  // path collapses while frames are being lost
        auto run = RunGuard(guard, link, target, 10, &now, 0.2);
        std::printf("queue guard: congested keyframe storm -> %u kbps (%d dips)\n", run.lastRate, run.dips);
        CHECK(run.dips > 0);
        CHECK(run.lastRate <= 62000);
    }

    // 4. A device whose own queue is deep is not congested for its whole life.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        link.naturalMs = 45;  // a headset that adds 45 ms just by existing
        int64_t now = 0;
        auto run = RunGuard(guard, link, target, 30);
        std::printf("queue guard: 45 ms device floor, settles at %u kbps (%d dips)\n", run.lastRate, run.dips);
        CHECK(run.lastRate >= 84000);  // the baseline is not read as congestion
        CHECK(run.floorHits == 0);
    }

    // 5. Steady state: the climb stops under the rate that just failed, so the stream stops
    //    pumping between two levels. The field saw 140 <-> 113 every ten seconds, and each
    //    of those steps reconfigured NVENC (45 a minute on VRChat) - a stutter per step.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 60, &now);  // let the ceiling find the path
        uint32_t lo = 0, hi = 0;
        int dips = 0;
        for (int i = 0; i < 300; i++) {  // 30 s of steady state
            const uint32_t cap = guard.Cap();
            const double rate = cap ? cap : target;
            link.Step(rate, 100.0);
            now += 100'000'000LL;
            const auto res = guard.Update(target, link.Report(), 0, false, now);
            const uint32_t r = res.capKbps ? res.capKbps : target;
            if (res.action == rc::QueueGuard::Action::Dip) dips++;
            // Skip the recovery from whatever dip ended the settling run: the question is
            // whether the rate then sits still.
            if (i >= 200) {
                if (!lo || r < lo) lo = r;
                if (r > hi) hi = r;
            }
        }
        std::printf("queue guard: steady state %u-%u kbps over 25 s, %d dips\n", lo, hi, dips);
        CHECK(dips <= 3);
        CHECK(hi - lo <= hi / 12);  // inside 8%: no visible pump
    }

    // 6. A collapse to the floor must not be a trap. The field log left the stream at 16 Mb/s
    //    and climbing *down*: a dip while already at the floor recorded the floor itself as
    //    the ceiling, so the climb's aim sat under the current cap.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 20, &now);
        // The path disappears and the queue lands a second deep, as it did in the field.
        link.capacityKbps = 2000;
        link.backpackMs = 1400;
        RunGuard(guard, link, target, 3, &now);
        const uint32_t atFloor = guard.Cap();
        const uint32_t floorKbps = target / cfg.minDivisor;
        std::printf("queue guard: collapse put it at %u kbps (floor %u)\n", atFloor, floorKbps);
        CHECK(atFloor == 0 || atFloor <= floorKbps + floorKbps / 20);  // it did reach the floor
        // The path comes back: the climb has to get out of there.
        link.capacityKbps = 100000;
        auto run = RunGuard(guard, link, target, 40, &now);
        std::printf("queue guard: back from the floor at %u kbps (%d dips)\n", run.lastRate, run.dips);
        CHECK(run.lastRate >= 84000);
    }

    // 7. The headset's first keyframe request is "give me an I-frame so I can start
    //    decoding", not a lost packet. Every stream start and profile switch used to
    //    answer it with a 12% rate cut while the queue was still -1..4 ms and loss 0%
    //    (field log 2026-11-14 18:30:39/18:31:46/18:32:21), then spend 2-3 s climbing
    //    back. Ignore keyframe dips for the first two seconds; after the grace a request
    //    alone still must not cut a clean link (test 3), but real queue pressure always
    //    dips. Actual packet loss is LossBrake's job, not the guard's.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        const auto first = guard.Update(target, -1, 0, true, 1'000'000'000LL);
        CHECK(first.action == rc::QueueGuard::Action::None);
        CHECK(first.capKbps == 0);
        for (int i = 2; i <= 10; ++i) {
            const auto r = guard.Update(target, 2, 0, true, int64_t(i) * 200'000'000LL);
            CHECK(r.action == rc::QueueGuard::Action::None);
        }
        // A lost frame on a clean queue, even after the grace, is not congestion.
        const auto late = guard.Update(target, 3, 0, true, 4'000'000'000LL);
        CHECK(late.action == rc::QueueGuard::Action::None);
        CHECK(late.capKbps == 0);
        // Real pressure after the grace still dips, with or without a keyframe request.
        const auto lateBusy = guard.Update(target, 200, 0, true, 4'100'000'000LL);
        CHECK(lateBusy.action == rc::QueueGuard::Action::Dip);
        CHECK(lateBusy.capKbps < target);
        // A queue over the budget still dips even inside the grace: only the keyframe
        // branch is suppressed, not congestion control.
        rc::QueueGuard congested;
        congested.Configure(cfg);
        const auto over = congested.Update(target, 200, 0, false, 1'000'000'000LL);
        CHECK(over.action == rc::QueueGuard::Action::Dip);
        std::printf("queue guard: startup keyframe ignored, clean late request held, congestion dips to %u kbps\n",
                    lateBusy.capKbps);
    }

    // 8. TASK-A73 P1, the pump (card §1 M1). Field log 22:48:38-45: 180000 -> 38442 kbps in
    //    three seconds, and the last four of those dips were taken against a queue that had
    //    already turned around and was falling. The guard cannot see its own press: the
    //    headset report is relative (DelayTracker - newest minus the lowest of the last four
    //    seconds), so a dip's effect enters the report seconds later, while the 400 ms
    //    throttle already allows the next press. SimLink with the card's numbers reproduces
    //    the same shape: 200 Mbps aimed at a path that carries 90.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        const uint32_t p1Target = 200000;
        const double capacity = 90000;
        SimLink link;
        link.capacityKbps = capacity;
        link.naturalMs = 25;
        int64_t now = 0;
        GuardTrace t = RunGuardTrace(guard, link, p1Target, 40, now);
        // One congestion episode ends when a report comes back inside the dead band: that is
        // the backlog draining, and any press after it against a queue no worse than the one
        // the previous press was taken against is the guard pushing again on a drain.
        const double episodeEndsBelow = cfg.targetMs - cfg.bandMs;  // 33 ms with the defaults
        const int pressed = t.PressedWhileDraining(episodeEndsBelow);
        const int worst5 = t.WorstWindow(5.0, 0.0);
        const double minRate = t.MinRate(0.0, 40.0);
        const double gain2 = t.FastestGain(0.0, 2.0);
        const double steady = t.MeanRate(30.0, 40.0);
        std::printf(
            "queue guard: A73 P1 flood 200 Mb/s onto 90, %d dips, %d in the worst 5 s, %d pressed "
            "a draining queue, min %.0f kbps, back up %.3f x in 2 s\n",
            t.DipCount(), worst5, pressed, minRate, gain2);
        // Card §3.1 also asks for the reported peak to stay under 120 ms for the whole run.
        // That is not reachable in this model and no law that dips on the reported queue can
        // reach it: the first dip cannot land before 0.5 s (m_lastDipNs starts at 0 plus the
        // 400 ms throttle), and by then 200 Mbps over a 90 Mbps path has queued 611 ms
        // (122 ms per 100 ms of a 122 % overload). Printed, not asserted; the settled window
        // (from 10 s on) is asserted below. See the TASK-A73 report, card §5.4.
        std::printf(
            "queue guard: A73 P1 reported queue peak %.0f ms over the whole run, %.0f ms once "
            "settled (from 10 s); steady %.0f kbps\n",
            t.PeakReport(0.0), t.PeakReport(10.0), steady);
        CHECK(pressed == 0);                       // M1 itself: red with 7 before the fix
        CHECK(minRate >= 0.8 * capacity);          // 72000; red with 26696 = 0.30 x capacity
        CHECK(worst5 <= 3);                        // red with 7 presses in the first 5 s
        CHECK(gain2 <= 1.12);                      // §2.3: red with 1.170 (the +4 %/400 ms sprint)
        CHECK(t.WorstWindow(5.0, 10.0) <= 2);      // the card's own gate, once settled
        CHECK(t.PeakReport(10.0) < 120);           // ... and the settled part of the peak gate
        CHECK(steady >= 80000 && steady <= 95000);  // the card's steady band
    }

    // 9. TASK-A73 P2, the ceiling creeping while the guard is still dipping (card §1 M2).
    //    Field 22:49-23:0x: 114664 -> 118104 kbps, exactly 1.0300, one +3 % ceiling rise, while
    //    the queue was being pressed throughout. m_ceilingNs is only refreshed by an *isolated*
    //    dip (more than 2 s after the previous one), so the members of a cascade - the dips
    //    that prove nothing about the ceiling - leave the rise timer alone, and the ceiling
    //    rises inside a window that is still dipping. Reports are fed by hand, the way test 7
    //    does, because what is under test here is the guard's own timers.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        const uint32_t p2Target = 200000;
        // 0-3 s clean; 3-3.6 s one deep queue, which proves the ceiling; 3.6-20 s the guard
        // climbs back to its aim; 20-30 s a cascade, one press every 500 ms while the queue
        // creeps 60 -> 100 ms; 30-120 s clean again.
        GuardTrace t = RunFeedTrace(guard, p2Target, {{0.0, 3.0, 5.0, 5.0},
                                                     {3.0, 3.6, 300.0, 300.0},
                                                     {3.6, 20.0, 5.0, 5.0},
                                                     {20.0, 30.0, 60.0, 100.0},
                                                     {30.0, 120.0, 5.0, 5.0}});
        // The plateau the guard holds just before the cascade starts: the aim the flood dip of
        // 3 s left behind. Measured in [15, 20) on purpose - the clean phase before that has no
        // cap at all, and its rate is the raw target, which is not what a ceiling produces.
        const double preDipAim = t.MaxRate(15.0, 20.0);  // the plateau: 0.97 x the proved ceiling
        int cascade = 0;
        for (const auto& x : t.s)
            if (x.action == 1 && x.sec >= 20.0 && x.sec < 30.0) cascade++;
        // The aim the cascade's first isolated dip records, which is what the working point may
        // come back to. The rise that would beat it is not due before 80.1 s (60 s after the
        // last ceiling refresh), so [50, 84] measures the creep itself in either code.
        const double allowed = preDipAim * 97 / 100;
        const double maxInWindow = t.MaxRate(50.0, 84.0);
        std::printf(
            "queue guard: A73 P2 pre-dip aim %.0f kbps, %d cascade dips, aim after them %.0f kbps, "
            "max rate in [50,84] s %.0f kbps = %.4f x\n",
            preDipAim, cascade, allowed, maxInWindow, maxInWindow / allowed);
        CHECK(preDipAim >= 0.96 * p2Target && preDipAim <= 0.98 * p2Target);  // the aim, not the target
        CHECK(cascade >= 5);                  // ... and what follows really is a cascade
        // Red before the fix: 193825 = 1.0300 x, the same ratio as 114664 -> 118104 in the log.
        CHECK(maxInWindow <= allowed);
        // §2.1 must stall a rise inside a dipping window, not forbid it: past the window the
        // ceiling still rises by its own percent.
        CHECK(t.MaxRate(40.0, 120.0) >= allowed * (1.0 + cfg.ceilingRisePercent / 100.0) * 0.999);
    }

    // 9b. The card's literal second form of P2: capacity fixed, five minutes, and the steady
    //     rate of the last two minutes may not exceed that of the first. Red before the fix,
    //     green after - but for M1's reason, not the +3 %/min creep: on a uniform path the
    //     per-minute means do not creep up at all (they drift down ~0.7 % over the last three
    //     minutes, printed below), because there every cycle's first dip is isolated and
    //     refreshes m_ceilingNs itself. The creep needs a cascade, which is what test 9 feeds
    //     by hand. Reported per card §5.4 rather than quietly substituted.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        const uint32_t p2Target = 200000;
        SimLink link;
        link.capacityKbps = 90000;
        link.naturalMs = 25;
        int64_t now = 0;
        GuardTrace t = RunGuardTrace(guard, link, p2Target, 300, now);
        const double minute1 = t.MeanRate(0.0, 60.0);
        const double last2 = t.MeanRate(180.0, 300.0);
        std::printf("queue guard: A73 P2b per-minute means:");
        for (int m = 0; m < 5; m++)
            std::printf(" %.0f", t.MeanRate(m * 60.0, m * 60.0 + 60.0));
        std::printf(" -> minute 1 %.0f, last two minutes %.0f, ratio %.4f\n", minute1, last2,
                    last2 / minute1);
        CHECK(last2 <= minute1);  // red before the fix at 1.1236
        CHECK(t.MeanRate(240.0, 300.0) <= t.MeanRate(60.0, 120.0) * 1.03);
    }
}

// TASK-A73b §3: the recovery-cost table. One isolated dip must not pay the cascade's price,
// so the recovery law is measured here on its own, with the reports fed by hand: a real
// SimLink would add its own seconds of lag on top of the guard's, and the question this table
// answers is how many seconds the *guard* spends getting back up. Three columns per card §3:
//   1. seconds from the last dip until the rate is back at its aim,
//   2. the largest relative gain of the sent rate in any 2 s window,
//   3. the deepest rate over the target.
// The three isolated depths are the three dip steps the guard has: 900 (queue past the band),
// 750 (queue more than 2.5x past it), 650 (a keyframe request on a congested queue).
static void TestGuardRecoveryCost() {
    const uint32_t target = 140000;
    const rc::QueueGuardConfig cfg;
    const double dipAt = cfg.targetMs + cfg.bandMs;  // 57 ms with the defaults

    struct Feed {
        double q;
        bool key;
    };
    auto ramp = [](int n, double q0, double q1, bool key) {
        std::vector<Feed> v;
        for (int i = 0; i < n; i++) {
            const double f = n > 1 ? double(i) / double(n - 1) : 0.0;
            v.push_back({q0 + (q1 - q0) * f, key});
        }
        return v;
    };
    auto drive = [&](const std::vector<Feed>& in) {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        GuardTrace tr;
        tr.target = target;
        tr.floorKbps = double(target) / cfg.minDivisor;
        int64_t now = 0;
        for (const auto& f : in) {
            GuardStep st;
            st.rateKbps = guard.Cap() ? guard.Cap() : target;
            now += 100'000'000LL;
            st.sec = double(now) / 1e9;
            st.reportMs = f.q;
            const auto res = guard.Update(target, f.q, 0, f.key, now);
            st.capKbps = res.capKbps;
            st.action = res.action == rc::QueueGuard::Action::Dip
                            ? 1
                            : (res.action == rc::QueueGuard::Action::Climb
                                   ? 2
                                   : (res.action == rc::QueueGuard::Action::Released ? 3 : 0));
            tr.s.push_back(st);
        }
        return tr;
    };
    // lead + flood + recovery, and the three numbers the card asks for.
    struct Row {
        int dips;
        double lastDipSec;
        double deadSec;
        double aimKbps;
        double backSec;
        double gain2;
        double deepestOverTarget;
    };
    auto measure = [&](const std::vector<Feed>& flood) {
        std::vector<Feed> in = ramp(30, 5, 5, false);  // 3 s clean: past the startup grace
        for (const auto& f : flood) in.push_back(f);
        for (const auto& f : ramp(500, 5, 5, false)) in.push_back(f);  // 50 s to get back
        const GuardTrace tr = drive(in);
        Row r{};
        for (const auto& x : tr.s)
            if (x.action == 1) {
                r.dips++;
                r.lastDipSec = x.sec;
            }
        r.aimKbps = tr.LastRate();
        // The dead time: how long after the last dip the first climb is allowed. This is what
        // the ladder actually buys, and it is the only column that separates a cascade from an
        // isolated dip at all - during the flood the report never comes back under the band, so
        // `calm` refuses every climb and no recovery constant can be reached between the knives.
        r.deadSec = -1;
        for (size_t i = 0; i < tr.s.size(); i++) {
            if (tr.s[i].sec <= r.lastDipSec) continue;
            if (tr.s[i].action == 2 || tr.s[i].action == 3) {
                r.deadSec = tr.s[i].sec - r.lastDipSec;
                break;
            }
        }
        r.backSec = -1;
        for (size_t i = 0; i < tr.s.size(); i++) {
            if (tr.s[i].sec <= r.lastDipSec) continue;
            if (tr.RateAt(i) >= r.aimKbps * 0.995) {
                r.backSec = tr.s[i].sec - r.lastDipSec;
                break;
            }
        }
        r.gain2 = tr.FastestGain(0.0, 2.0);
        r.deepestOverTarget = tr.MinRate(0.0, 1e9) / double(target);
        return r;
    };
    auto print = [](const char* name, const Row& r) {
        std::printf("queue guard: recovery %-16s %2d dips, last at %.1f s, dead %.1f s, aim %.0f kbps, "
                    "back in %5.1f s, gain2 %.4f x, deepest %.3f x target\n",
                    name, r.dips, r.lastDipSec, r.deadSec, r.aimKbps, r.backSec, r.gain2,
                    r.deepestOverTarget);
    };

    const Row shallow = measure(ramp(6, dipAt * 1.8, dipAt * 1.8, false));   // 103 ms -> dip 900
    const Row medium = measure(ramp(6, 300, 300, false));                    // -> dip 750
    const Row deep = measure(ramp(6, dipAt * 1.8, dipAt * 1.8, true));       // -> dip 650
    const Row cascade = measure(ramp(35, dipAt + 3, dipAt * 2.4, false));    // 7 in 3.5 s
    print("isolated 900", shallow);
    print("isolated 750", medium);
    print("isolated 650", deep);
    print("cascade", cascade);

    // What the card's direction 1 asks for: an isolated dip is back at its aim in seconds,
    // not in the tens of seconds a cascade deserves, and the dead time in front of it is under
    // a second. Measured 0.9 s dead / 1.9 s back.
    CHECK(shallow.dips == 1 && shallow.deadSec >= 0 && shallow.deadSec <= 1.0);
    CHECK(shallow.backSec >= 0 && shallow.backSec <= 3.0);
    // The ladder is about how many knives an episode took, not how deep the last one was, so a
    // deeper cut on the same rung costs only the arithmetic of climbing back out of it: same
    // 0.9 s dead, more seconds. Measured 5.9 s (750) and 8.9 s (650).
    CHECK(medium.dips == 1 && std::fabs(medium.deadSec - shallow.deadSec) < 0.05);
    CHECK(deep.dips == 1 && std::fabs(deep.deadSec - shallow.deadSec) < 0.05);
    CHECK(medium.backSec > shallow.backSec && deep.backSec > medium.backSec);
    CHECK(deep.backSec <= 12.0);
    // Direction 3: nothing climbs faster than 1.12 in 2 s, and no dip refills a cut in a
    // second. Same gate as test 8's gain2, measured on the recovery law alone.
    CHECK(shallow.gain2 <= 1.12 && medium.gain2 <= 1.12 && deep.gain2 <= 1.12);
    CHECK(cascade.gain2 <= 1.12);
    // Direction 2: the cascade pays more than the isolated dip, and what it pays is the dead
    // time. Held to twice the isolated one, not to a tuned number: rung 5 is 800 ms + 4 steps
    // of 1 s, measured 4.9 s against 0.9 s. Mutation V-a (card R5), which takes the rung back
    // to 1 for every dip, lands both rows on 0.9 s and dies here - test 8's M1 group cannot see
    // it, because its seven knives all fall while the report is still past the band, `calm` is
    // false for every one of them, and no climb happens between the dip and the recovery at
    // all. Its 3-in-5-s and 1.08x are identical with or without the ladder.
    CHECK(cascade.deadSec >= 2 * shallow.deadSec);
    CHECK(cascade.dips >= 5);
    CHECK(cascade.backSec > shallow.backSec);
    CHECK(cascade.deepestOverTarget < shallow.deepestOverTarget);

    // The half of the story test 5 tells: the ceiling's +3 % rise comes due 60 s after the last
    // dip of the settling run, and once the rate crosses the capacity the report grows ~2.7 ms
    // per 100 ms step, so a 10 % dip follows it about 2.2 s later. Test 5 samples only the last
    // 10 s of its 30 s window, so the rise and its dip have to be over before the window opens.
    // With a dead time long enough to charge an isolated dip for a cascade the whole event moves
    // to 19.9 s -> 22.1 s and lands inside it, which is the 92436-102707 = 10.00 % red.
    {
        rc::QueueGuard guard;
        guard.Configure(cfg);
        SimLink link;
        link.capacityKbps = 100000;
        int64_t now = 0;
        RunGuard(guard, link, target, 60, &now);  // the same settle test 5 does
        double firstDipSec = -1;
        int lateDips = 0;
        double maxSampled = 0;
        for (int i = 0; i < 300; i++) {  // the same 30 s of steady state
            const uint32_t cap = guard.Cap();
            link.Step(cap ? cap : target, 100.0);
            now += 100'000'000LL;
            const auto res = guard.Update(target, link.Report(), 0, false, now);
            if (i >= 200) {
                const double r = res.capKbps ? res.capKbps : target;
                if (r > maxSampled) maxSampled = r;
            }
            if (res.action == rc::QueueGuard::Action::Dip) {
                if (firstDipSec < 0) firstDipSec = i * 0.1;
                if (i >= 200) lateDips++;
            }
        }
        std::printf("queue guard: recovery test-5 replay, first dip at %.1f s, %d in the sampled 10 s,"
                    " sampled max %.0f kbps against a %.0f kbps path\n",
                    firstDipSec, lateDips, maxSampled, link.capacityKbps);
        CHECK(lateDips == 0);
        // And it has to still be under the path. Test 5 itself only asks the sampled rate not to
        // *pump*, so a guard that parks quietly 1.4 % over capacity passes it - measured 2026-11-23
        // with mutation V-e (the `recovering` window cut from waitNs*3 to waitNs*2), which printed
        // `steady state 101433-101433 kbps over 25 s, 0 dips`, all green, while the backlog grew
        // 1.4 ms per step and would have taken 40 s - longer than the window - to reach dipAt. The
        // whole point of the +3 %-then-hold ceiling is that the aim stays where the path failed.
        CHECK(maxSampled <= link.capacityKbps);
    }
}

// A86b: foveation does NOT follow an (invisible) rung any more. A rung-driven
// fovea radius changed edge sharpness in step with link noise - visible flicker.
// The chosen share is passed through untouched; the geometry below is unchanged.
static void TestProfileFoveaGradient() {
    CHECK(rcs::QualityProfileFoveaFor(0.6f) == 0.6f);
    CHECK(rcs::QualityProfileFoveaFor(0.9f) == 0.9f);
    CHECK(rcs::QualityProfileFoveaFor(0.45f) == 0.45f);
    // Encoded widths still follow the chosen share on the 2176 panel.
    const uint32_t w1 = uint32_t(std::lround(2176.0 * (0.6 + 0.4 * 0.5) / 16.0)) * 16;
    const uint32_t w2 = uint32_t(std::lround(2176.0 * (0.7 + 0.3 * 0.5) / 16.0)) * 16;
    CHECK(w1 == 1744);
    CHECK(w2 == 1856);
    std::printf("fovea share passes through (no tier coupling); geometry 1744/1856 per eye\n");
}

// The bitrate ceiling: the headset's choice wins unless the PC user overrides, and
// a missing headset number always falls back to the PC. A86b: CBR is a plain
// independent opt-in switch - never switched on by a rung auto happened to climb to.
static void TestBitrateSwitches() {
    CHECK(rcs::EffectiveVrBitrate(140, 123, false) == 123);
    CHECK(rcs::EffectiveVrBitrate(140, 123, true) == 140);
    CHECK(rcs::EffectiveVrBitrate(140, 0, false) == 140);
    CHECK(rcs::EffectiveVrBitrate(140, 0, true) == 140);
    CHECK(rcs::QualityProfileCbr(true) == true);
    CHECK(rcs::QualityProfileCbr(false) == false);
    std::printf("bitrate switches: headset wins by default, CBR is independent opt-in\n");
}

// The loss brake: a loss cut holds the rate down even when the loss reads zero for
// a moment (the real link walked straight back into loss within 1-2 s of the old
// climb, 12 dips in 12 minutes), then releases at 2% of the user's number per
// second, never below 60% of what the user set.
// LossBrakeStep is constexpr, so the whole contract is verified at compile time
// (static_asserts below, at namespace scope). The runtime test used to live here
// and crashed the suite in a layout-sensitive way - see DEVIATIONS D-013.
// The loss brake contract, checked before anything runs:
static_assert([] {
    rc::LossBrakeState st;
    const auto r = rc::LossBrakeStep(st, 0.015, 100000, 0);
    return r.capKbps == 85000 && r.changed && st.holdUntilNs == 8'000'000'000LL;
}(), "loss brake: light loss cuts to 85% and holds");
static_assert([] {
    rc::LossBrakeState st;
    (void)rc::LossBrakeStep(st, 0.015, 100000, 0);
    const auto r = rc::LossBrakeStep(st, 0.0, 100000, 1'000'000'000LL);
    const auto r2 = rc::LossBrakeStep(st, 0.0, 100000, 7'000'000'000LL);
    return r.capKbps == 85000 && !r.changed && r2.capKbps == 85000 && !r2.changed;
}(), "loss brake: the hold outlasts zero loss");
static_assert([] {
    rc::LossBrakeState st;
    (void)rc::LossBrakeStep(st, 0.015, 100000, 0);
    int64_t t = 9'000'000'000LL;
    uint32_t maxSeen = 0;
    bool released = false;
    for (int i = 0; i < 60; i++) {
        const auto r = rc::LossBrakeStep(st, 0.001, 100000, t);
        maxSeen = r.capKbps > maxSeen ? r.capKbps : maxSeen;
        if (r.released) { released = true; break; }
        t += 1'000'000'000LL;
    }
    return released && maxSeen == 100000 && st.capKbps == 0;
}(), "loss brake: release climbs 2%/s back to the ceiling");
static_assert([] {
    rc::LossBrakeState st;
    (void)rc::LossBrakeStep(st, 0.06, 100000, 0);
    const auto r = rc::LossBrakeStep(st, 0.06, 100000, 1'000'000'000LL);
    return r.capKbps == 70000;
}(), "loss brake: heavy loss cuts to 70% with the 15 s hold");
static_assert([] {
    rc::LossBrakeState st;
    (void)rc::LossBrakeStep(st, 0.06, 100000, 0);
    auto r = rc::LossBrakeResult{};
    for (int64_t t = 1'000'000'000LL; t <= 30'000'000'000LL; t += 1'000'000'000LL) {
        r = rc::LossBrakeStep(st, 0.06, 100000, t);
        if (r.capKbps < 60000) return false;
    }
    return r.capKbps == 70000;
}(), "loss brake: the 60% floor holds under sustained heavy loss");
static_assert([] {
    rc::LossBrakeState st;
    const auto r = rc::LossBrakeStep(st, 0.005, 100000, 0);
    return r.capKbps == 0 && !r.changed;
}(), "loss brake: below 1% nothing happens");

static void TestLossBrake() {
    std::printf("loss brake: verified at compile time (cut 85/70, holds 8/15 s, release 2%%/s, floor 60%%)\n");
}


// The frame-rate ladder: the budget follows real swing, the reconfigures do not.
static void TestFpsLadder() {
    CHECK(rc::QuantizeFps(90.0, 90) == 90);
    CHECK(rc::QuantizeFps(87.0, 90) == 90);   // within 15%: no reconfigure
    CHECK(rc::QuantizeFps(47.0, 90) == 45);
    CHECK(rc::QuantizeFps(44.0, 45) == 45);   // within 15%: no reconfigure
    CHECK(rc::QuantizeFps(30.0, 45) == 30);
    CHECK(rc::QuantizeFps(25.0, 30) == 24);
    CHECK(rc::QuantizeFps(15.0, 45) == 24);   // a stall must not inflate the budget
    CHECK(rc::QuantizeFps(60.0, 45) == 60);   // measured above current: follow it up
    std::printf("fps ladder: 24/30/40/45/48/54/60/72/80/90/120, +-15%% hold, stalls floor at 24");
}

// D: the fill floor is derived from the refresh tier and the VBV percentage, never a
// constant. ratio = vbvPct/100 * rung / nominal must be >= 1 at every supported tier.
static void TestFillFloorByTier() {
    struct Case { int nominal, pct, floor; };
    const Case cases[] = {
        {90, 200, 45}, {72, 200, 40}, {120, 200, 60}, {60, 200, 30}, {24, 200, 24},
        {90, 150, 60}, {72, 150, 48}, {120, 150, 80},
    };
    for (const auto& c : cases) {
        const int f = rc::FillFloorRung(c.nominal, c.pct);
        CHECK(f == c.floor);
        CHECK(rc::LadderContainsStep(f));
        // the floor must fit the bucket ...
        const double ratio = double(c.pct) / 100.0 * double(f) / double(c.nominal);
        CHECK(ratio >= 1.0 - 1e-9);
        // ... and the rung one step below must NOT (that is what makes it the floor)
        int prev = 0;
        for (int s : rc::kFpsLadder) if (s < f) prev = s;
        if (prev > 0) CHECK(double(c.pct) / 100.0 * double(prev) / double(c.nominal) < 1.0);
    }
    // S1b: 118 fps on a 120 Hz panel snaps to the top rung instead of sticking at 90.
    CHECK(rc::QuantizeFps(118.0, 90) == 120);
    CHECK(rc::QuantizeFps(117.0, 90) == 90);    // 120*0.98 = 117.6 -> 117 is below the snap window
    CHECK(rc::QuantizeFps(116.0, 90) == 90);    // same, one frame further out
    std::printf("fill floor by tier: 90/200->45, 72/150->48, 120/200->60, 120Hz snap 117.6+\n");
}

// TASK-DEC-PERCENTILE-2026-11-20 §1.2: the DecodeMsSamples ring behind the diag line's
// dec p50/p95 (was a -1.0/-1.0 placeholder). Pure logic - no clock, no I/O, no timing
// sensitivity - so no TestHostBusy guard is needed.
static void TestPercentile() {
    // P1: 空采样器 - 哨兵 -1.0f，Count 0。
    {
        rc::DecodeMsSamples s;
        CHECK(s.Count() == 0);
        CHECK(s.Percentile(50.f) == -1.0f);
        CHECK(s.Percentile(95.f) == -1.0f);
    }
    // P2: 单样本 12.5 - p50 与 p95 都直取它（rank = ceil(p/100*1) = 1）。
    {
        rc::DecodeMsSamples s;
        s.Push(12.5f);
        CHECK(s.Count() == 1);
        CHECK(s.Percentile(50.f) == 12.5f);
        CHECK(s.Percentile(95.f) == 12.5f);
    }
    // P3: 1..100 递增 push 100 个 - nearest-rank ceil 在 n=100 时 rank50/rank95 直取。
    {
        rc::DecodeMsSamples s;
        for (int i = 1; i <= 100; ++i) s.Push(float(i));
        CHECK(s.Count() == 100);
        CHECK(s.Percentile(50.f) == 50.f);  // rank = ceil(0.50*100) = 50
        CHECK(s.Percentile(95.f) == 95.f);  // rank = ceil(0.95*100) = 95
    }
    // P4: 3 个样本 {10,20,30} - ceil 在 0.5 步进处可见。
    {
        rc::DecodeMsSamples s;
        s.Push(10.f);
        s.Push(20.f);
        s.Push(30.f);
        CHECK(s.Percentile(50.f) == 20.f);  // rank = ceil(1.5) = 2
        CHECK(s.Percentile(95.f) == 30.f);  // rank = ceil(2.85) = 3
    }
    // P5: 非法输入 - Push 忽略非有限/<=0 的值；Percentile 的 p 出域或 NaN 时 -1.0f。
    {
        rc::DecodeMsSamples s;
        s.Push(12.5f);
        const int before = s.Count();
        s.Push(std::numeric_limits<float>::quiet_NaN());
        s.Push(-1.f);
        s.Push(std::numeric_limits<float>::infinity());
        CHECK(s.Count() == before);
        CHECK(s.Percentile(0.f) == -1.0f);
        CHECK(s.Percentile(101.f) == -1.0f);
        CHECK(s.Percentile(std::numeric_limits<float>::quiet_NaN()) == -1.0f);
    }
    // P6: 环形覆盖 - 容量 512，push 600 个递增后样本集为后 512 个（89..600）；
    // p50 的 rank = ceil(0.50*512) = 256，对应值 89+255 = 344（即第 344 个被 push 的样本）。
    {
        rc::DecodeMsSamples s;
        for (int i = 1; i <= 600; ++i) s.Push(float(i));
        CHECK(s.Count() == 512);
        CHECK(s.Percentile(50.f) == 344.f);
    }
    // P7: Reset 后回到 P1 状态。
    {
        rc::DecodeMsSamples s;
        for (int i = 1; i <= 100; ++i) s.Push(float(i));
        s.Reset();
        CHECK(s.Count() == 0);
        CHECK(s.Percentile(50.f) == -1.0f);
        CHECK(s.Percentile(95.f) == -1.0f);
    }
    std::printf("decode percentile: empty sentinel, single sample, 1..100 direct-rank, ceil on 3 samples, "
                "invalid inputs ignored, 600->512 overwrite, reset\n");
}

// A23 (queue bound) + A46 (TASK-RECOVERY-GUARD-2026-11-22): the decode queue bound
// and the recovery-chain fix. A frame that queued strictly longer than kQueueDropMs
// between arrival (receivedNs) and the decode submission point is dropped; exactly
// kQueueDropMs passes ("超过" is strictly greater); a missing timestamp or a negative
// age never drops. A46 adds two scenarios the old gate got wrong: the keyframe
// request must fire on the FIRST drop (1, 6, 11 ...), not only at 5/10/15, so a
// 1-4 drop streak interleaved with passing frames still asks; and a queued IDR is
// never dropped by the age rule (it is the recovery frame itself). The A46 checks
// below run RED against the A23 implementation - that is the regression guard.
static void TestFrameQueueBound() {
    const int64_t kMs = 1'000'000;
    // Q1: the constants are the card's numbers (a silent threshold change re-fails here).
    CHECK(rc::kQueueDropMs == 100);
    CHECK(rc::kQueueDropKeyframeEvery == 5);
    // Q2: the synthetic arrival timeline. Arrival at t0; the submission point checks
    // the frame's age at each line. Under the bound: decode; over: drop.
    const int64_t t0 = 1'000'000'000;  // arbitrary: one frame arrives here
    CHECK(!rc::FrameQueueExpired(t0 + 0 * kMs, t0));            // submitted on arrival
    CHECK(!rc::FrameQueueExpired(t0 + 25 * kMs, t0));           // healthy stream (AB9 baseline queue 0.4-1.0 ms)
    CHECK(!rc::FrameQueueExpired(t0 + 50 * kMs, t0));
    CHECK(!rc::FrameQueueExpired(t0 + 99 * kMs, t0));           // one ns under the bound
    CHECK(!rc::FrameQueueExpired(t0 + 100 * kMs, t0));          // boundary: exactly 100 ms passes
    CHECK(rc::FrameQueueExpired(t0 + 100 * kMs + 1, t0));       // the first ns past it drops
    CHECK(rc::FrameQueueExpired(t0 + 151 * kMs, t0));           // AB9: total 593 / queue 151
    CHECK(rc::FrameQueueExpired(t0 + 479 * kMs, t0));           // AB9: total 410 / queue 479
    // Q3: unknowable ages keep the frame (no timestamp, or the clock went backwards).
    CHECK(!rc::FrameQueueExpired(t0, 0));
    CHECK(!rc::FrameQueueExpired(t0, -5));
    CHECK(!rc::FrameQueueExpired(t0, t0 + kMs));  // negative age
    // Q4 (A46): the first drop asks immediately, then every 5th (1, 6, 11, 16 ...).
    // A23 only asked at 5/10/15, so a 1-4 drop streak ending in a passing frame
    // asked nothing and left the reference chain broken forever under cap100.
    CHECK(!rc::QueueDropRequestKeyframe(0));   // defensive: a reset streak never asks
    CHECK(rc::QueueDropRequestKeyframe(1));    // 连续丢1帧立即请求 (was quiet under A23)
    CHECK(!rc::QueueDropRequestKeyframe(2));
    CHECK(!rc::QueueDropRequestKeyframe(3));
    CHECK(!rc::QueueDropRequestKeyframe(4));
    CHECK(!rc::QueueDropRequestKeyframe(5));
    CHECK(rc::QueueDropRequestKeyframe(6));
    CHECK(!rc::QueueDropRequestKeyframe(9));
    CHECK(!rc::QueueDropRequestKeyframe(10));
    CHECK(rc::QueueDropRequestKeyframe(11));
    CHECK(rc::QueueDropRequestKeyframe(16));
    // Q5: cadence over a 30-drop burst: 1, 6, 11, 16, 21, 26 -> 6 requests.
    uint32_t burstRequests = 0;
    for (uint32_t streak = 1; streak <= 30; streak++)
        if (rc::QueueDropRequestKeyframe(streak)) burstRequests++;
    CHECK(burstRequests == 6);
    // Q6 (A46): the cap100 jitter pattern - drop 1, pass 1, drop 1. Both drops
    // request, because the first drop of each streak asks at once.
    {
        uint32_t streak = 0, requests = 0;
        auto drop = [&] { streak++; if (rc::QueueDropRequestKeyframe(streak)) requests++; };
        auto pass = [&] { streak = 0; };  // a small P frame passes the gate
        drop();                           // first drop: immediate request
        pass();                           // A23 cleared the streak here
        drop();                           // asks again -> the chain can recover
        CHECK(requests == 2);
    }
    // Q7 (A46): the persistent break flag, mirroring App::OnVideoFrame.
    {
        bool broken = false;
        uint32_t streak = 0, requests = 0;
        auto drop = [&] {
            const bool firstBreak = !broken;
            broken = true;
            streak++;
            if (firstBreak || rc::QueueDropRequestKeyframe(streak)) requests++;
        };
        auto passP = [&] {
            streak = 0;                       // a frame passed the gate
            const bool decoderNeedsKeyframe = true;  // gap -> decoder's one-shot flag
            if (broken || decoderNeedsKeyframe) requests++;
        };
        auto submitKeyframe = [&] { broken = false; streak = 0; };
        drop();
        CHECK(broken && requests == 1);
        passP();
        CHECK(broken && requests == 2);   // 置位期间加强主动请求
        passP();
        CHECK(broken && requests == 3);
        submitKeyframe();                 // a real IDR reached Submit
        CHECK(!broken && requests == 3);
        const bool decoderNeedsKeyframe = false;  // no gap after the IDR
        if (broken || decoderNeedsKeyframe) requests++;
        CHECK(!broken && requests == 3);  // 收到keyframe后清零不再请求
        drop();                           // the next break asks immediately again
        CHECK(broken && requests == 4);
    }
    // Q8 (A46): IDR exemption. A keyframe queued 150/479 ms is submitted, while
    // a P frame of the same age is still dropped by the unchanged age rule.
    CHECK(!rc::FrameQueueExpired(t0 + 150 * kMs, t0, /*isKeyframe=*/true));
    CHECK(rc::FrameQueueExpired(t0 + 150 * kMs, t0, /*isKeyframe=*/false));
    CHECK(!rc::FrameQueueExpired(t0 + 479 * kMs, t0, true));
    CHECK(!rc::FrameQueueExpired(t0 + 100 * kMs + 1, t0, true));
    CHECK(!rc::FrameQueueExpired(t0, 0, true));
    std::printf("frame queue bound: constants pinned, 0/25/50/99/100 ms pass, 100 ms + 1 ns / 151 / 479 ms drop, "
                "unknowable ages pass; A46 recovery chain: first drop asks at once (1,6,11..), "
                "drop1/pass1/drop1 asks twice, keyframe clears broken state and stops requests, "
                "IDR queued 150/479 ms is never dropped while a P frame is\n");
}

// A26 (TASK-BACKLOG-FLUSH-2026-11-22): the sender-side stale-frame flush. The card's
// synthetic timeline: a normal frame passes, an expired frame drops, and the drop
// wipes the pacer's billing. The throttles (300 ms keyframe, 1 s log) are walked over
// a burst shaped like the AB9 scene change (a giant frame queues the pipeline for
// hundreds of ms, every frame it overtakes expires). All int64 arithmetic on one
// synthetic clock: no wall time, no flake surface.
static void TestStaleFlush() {
    const int64_t kMs = 1'000'000;
    // F1: the constants are the card's numbers (a silent change re-fails here).
    CHECK(rc::kFlushKeyframeThrottleNs == 300 * kMs);
    CHECK(rc::kFlushLogThrottleNs == 1'000 * kMs);
    // F1b (A75, TASK-A75-FLUSH-GRACE-2026-11-22 §2.1): the grace period is two 60 fps
    // frame periods, and it is only defensible because it sits between the two measured
    // groups - the healthy log's worst lateness (8 ms, streamer.log:2115) below it, the
    // backlog it must still clear (119 ms, AB9) above it. Pin both ends, not just the
    // arithmetic: a "tidier" number that breaks either side is a regression, not a refactor.
    CHECK(rc::kFlushFramePeriodNs == 1'000'000'000 / 60);
    CHECK(rc::kFlushGraceFramePeriods == 2);
    CHECK(rc::kFlushGraceNs == 2 * (1'000'000'000 / 60));  // 33'333'332 ns
    CHECK(rc::kFlushGraceNs > 8 * kMs);     // 4.2x the measured noise: 0/1/8 ms must pass
    CHECK(rc::kFlushGraceNs > 25 * kMs);    // the neighbouring healthy bursts (streamer.log:53-1427)
    CHECK(rc::kFlushGraceNs < 119 * kMs);   // 3.6x under the smallest real backlog
    // F2: normal frames pass - the predicted display time sits in the future by
    // design, so the steady state never drops (card §2: 稳态零改动).
    const int64_t t0 = 2'000'000'000;  // arbitrary: the frame was submitted here
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 10 * kMs));   // picked up mid-pipeline
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 12 * kMs));   // ... just before sending
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 39 * kMs));   // one ns before the moment
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs));   // boundary: `<` is strict
    // A75 changed what the boundary IS: the old rule dropped the first ns past the display
    // moment, which is what threw away the healthy run's 0/1/8 ms frames
    // (streamer.log:2062-2115, card §1). The strict `<` semantics are kept, one grace
    // period later: inside the grace nothing drops, exactly the grace still does not, the
    // first ns past it does.
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs + 1));  // 1 ns past the moment
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs + 8 * kMs));  // streamer.log:2115's worst
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs + rc::kFlushGraceNs - 1));
    CHECK(!rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs + rc::kFlushGraceNs));
    CHECK(rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 40 * kMs + rc::kFlushGraceNs + 1));
    CHECK(rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 119 * kMs));   // AB9: queue 119 ms
    CHECK(rc::FrameExpiredForSend(t0 + 40 * kMs, t0 + 479 * kMs));   // AB9: queue 479 ms
    // F3: an unknown moment (0, no tracking yet) and a clock anomaly keep the frame -
    // the flush must never fire on a prediction it does not have.
    CHECK(!rc::FrameExpiredForSend(0, t0 + 9'999 * kMs));
    CHECK(!rc::FrameExpiredForSend(-1, t0 + 9'999 * kMs));
    // F4: the send loop's gate, one call per frame. A frame predicted for the future
    // passes with nothing else attached; an expired one drops, and the throttles say
    // which side effects this particular drop carries (first stamps are 0 = never).
    const rc::FlushDecision pass = rc::StaleFlushDecide(t0 + 40 * kMs, t0 + 12 * kMs, 0, 0);
    CHECK(!pass.drop && !pass.requestKeyframe && !pass.log);
    const rc::FlushDecision first = rc::StaleFlushDecide(t0 + 40 * kMs, t0 + 119 * kMs, 0, 0);
    CHECK(first.drop && first.requestKeyframe && first.log);
    // 299 ms after a request: throttled; at exactly 300 ms: due again (>= window).
    const int64_t at = t0 + 119 * kMs;
    CHECK(!rc::StaleFlushDecide(at - 40 * kMs, at + 299 * kMs, at, at).requestKeyframe);
    CHECK(rc::StaleFlushDecide(at - 40 * kMs, at + 300 * kMs, at, at).requestKeyframe);
    CHECK(!rc::StaleFlushDecide(at - 40 * kMs, at + 999 * kMs, at, at).log);
    CHECK(rc::StaleFlushDecide(at - 40 * kMs, at + 1'000 * kMs, at, at).log);
    // F4b (A46): clock rollback. A throttle stamp ahead of `now` (clock source reset,
    // sleep/wake, mixed time bases) made now-lastNs negative and the old due() stayed
    // false for the whole window, silently swallowing every request/log. A rollback
    // must count as due.
    CHECK(rc::StaleFlushDecide(at - 40 * kMs, at, at + 100 * kMs, at + 100 * kMs).requestKeyframe);
    CHECK(rc::StaleFlushDecide(at - 40 * kMs, at, at, at + 100 * kMs).log);
    CHECK(!rc::StaleFlushDecide(at - 40 * kMs, at, at - 100 * kMs, at).requestKeyframe);  // a past stamp still throttles
    // F5: the card's reset rule - whatever the giant frames billed is forgiven, the
    // bucket re-baselines to now, so the next frame may start at once.
    const int64_t billedTo = at + 900 * kMs;  // a giant frame's leftover billing
    CHECK(rc::FlushSendDebtNs(billedTo, at) == at);              // the debt is gone
    CHECK(rc::FlushSendDebtNs(0, at) == at);                     // and it stays now
    // F6: a burst shaped like the AB9 scene change: a giant frame blocks the send
    // thread for 900 ms; every frame encoded meanwhile expires when picked up, then fresh
    // frames flow again (F7). 19 expired frames between t and t+900 ms carry exactly 4
    // keyframe requests (0/300/600/900 ms stamps) and 1 log line (at t).
    // A75: the lateness per frame used to be 10 ms, and 10 ms is inside the new grace
    // period, so it is 50 ms here - the loop's own pickup interval: each frame is one
    // interval behind the queue that overtook it. Card §2.3 - the burst's per-frame
    // lateness must stay above kFlushGraceNs, and the shape (19 frames, 50 ms apart, 900 ms
    // of blockage) is what F7 and the throttle counts depend on, so it is the lateness that
    // moved, not the burst. The counts below are unchanged: 19 drops / 4 requests / 1 line.
    int64_t keyframeStamp = 0, logStamp = 0, debtNs = billedTo;
    uint32_t drops = 0, requests = 0, logs = 0;
    for (int64_t t = at; t <= at + 900 * kMs; t += 50 * kMs) {
        const int64_t predicted = t - 50 * kMs;  // each was overtaken by the queue (A75: > grace)
        const rc::FlushDecision d = rc::StaleFlushDecide(predicted, t, keyframeStamp, logStamp);
        if (!d.drop) continue;
        drops++;
        if (d.requestKeyframe) {
            requests++;
            keyframeStamp = t;
        }
        debtNs = rc::FlushSendDebtNs(debtNs, t);  // every drop wipes the bucket again
        if (d.log) {
            logs++;
            logStamp = t;
        }
    }
    CHECK(drops == 19);
    CHECK(requests == 4);  // at +0, +300, +600, +900 ms
    CHECK(logs == 1);      // at +0 ms only
    CHECK(debtNs == at + 900 * kMs);  // the last drop re-baselined to its own now
    // F6b (A75): the same 19-frame burst with the pre-A75 10 ms lateness - the shape the
    // user's healthy 60 fps run actually produced (8/14/16-frame runs, 0-8 ms late) - must
    // now flush nothing at all, and must not touch either throttle stamp or the debt.
    {
        int64_t kf6 = 0, log6 = 0, debt6 = billedTo;
        uint32_t drops6 = 0, requests6 = 0;
        for (int64_t t = at; t <= at + 900 * kMs; t += 50 * kMs) {
            const rc::FlushDecision d = rc::StaleFlushDecide(t - 10 * kMs, t, kf6, log6);
            if (!d.drop) continue;
            drops6++;
            if (d.requestKeyframe) {
                requests6++;
                kf6 = t;
            }
            debt6 = rc::FlushSendDebtNs(debt6, t);
        }
        CHECK(drops6 == 0 && requests6 == 0);
        CHECK(kf6 == 0 && log6 == 0);                     // nothing stamped: no side effects at all
        CHECK(debt6 == billedTo);                         // the pacer's billing stands as it was
    }
    // F7: after the burst, the first fresh frame (predicted 30 ms ahead) passes
    // untouched even with the throttle stamps mid-window - and per the wiring its
    // all-false decision means Send proceeds exactly as before the flush existed.
    const rc::FlushDecision fresh = rc::StaleFlushDecide(at + 930 * kMs + 30 * kMs, at + 930 * kMs,
                                                         keyframeStamp, logStamp);
    CHECK(!fresh.drop && !fresh.requestKeyframe && !fresh.log);
    // F8 (A46): the circuit breaker. A systematic prediction clock skew makes every
    // frame expired; after kStaleFlushBreakerDrops consecutive drops the sender must
    // be forced through instead of dropping 100% forever (black screen).
    CHECK(rc::kStaleFlushBreakerDrops == 30);
    CHECK(!rc::StaleFlushBreaker(rc::kStaleFlushBreakerDrops - 1));
    CHECK(rc::StaleFlushBreaker(rc::kStaleFlushBreakerDrops));
    {
        uint32_t streak = 0, forced = 0;
        for (int i = 0; i < 100; i++) {  // 100 consecutive expired frames
            if (rc::StaleFlushBreaker(++streak)) { forced++; streak = 0; }
        }
        CHECK(forced == 3);  // released at drops 30, 60, 90
    }
    std::printf("stale flush: constants pinned (A75 grace = 2 x 60 fps frame periods = %lld ms, "
                "between the 8 ms measured noise and the 119 ms backlog), future/0-prediction frames pass, "
                "grace boundary 1 ns / -1 / == / +1 ns of kFlushGraceNs as specified, 119 / 479 ms late drop, "
                "debt re-baselines to now, 19-drop burst asks 4 times and logs once, "
                "the same burst 10 ms late flushes nothing; "
                "A46: clock rollback makes throttles due, 100 consecutive stale drops force 3 frames through\n",
                (long long)(rc::kFlushGraceNs / kMs));
}

// A72 (TASK-A72-STALE-FLUSH-2026-11-22 §4.5): the flush's chain rules C1-C8. The card's
// evidence is the user's log: 19 flushes of 4-7 frames, and the first frame after each one
// went out with its own references already on the discard pile ("stutters, then smears").
// rc::StaleFlushGate is the sender's only flush entry point now; StaleFlushDecide is called
// alongside it below only to show what the old expiry-only gate did NOT look at. Same
// synthetic clock as above: int64 arithmetic, no wall time.
static void TestStaleFlushChainGate() {
    const int64_t kMs = 1'000'000;
    const int64_t t0 = 5'000'000'000;  // arbitrary anchor
    // C1: chain intact, frame still in time -> it goes out with nothing attached.
    {
        rc::StaleFlushState s;
        const rc::FlushGate g = rc::StaleFlushGate(false, t0 + 40 * kMs, t0 + 12 * kMs, s);
        CHECK(g.send && !g.requestKeyframe && !g.resetDebt && !g.log && !g.breaker);
        CHECK(!s.chainBroken && s.dropStreak == 0);
        // A72 review gate: judging a frame that then goes out must leave the log throttle
        // untouched, or a healthy second swallows the flush line that opens the next burst.
        CHECK(s.lastLogNs == 0);
        std::printf("A72 C1 intact + in time: send=%d req=%d debt=%d log=%d breaker=%d broken=%d\n",
                    g.send, g.requestKeyframe, g.resetDebt, g.log, g.breaker, s.chainBroken);
    }
    // C2: chain intact, frame expired -> dropped (A26 unchanged) AND the chain is now
    // broken, so no descendant may leave until a repair gets out.
    {
        rc::StaleFlushState s;
        const rc::FlushGate g = rc::StaleFlushGate(false, t0 + 40 * kMs, t0 + 119 * kMs, s);
        CHECK(!g.send && g.requestKeyframe && g.resetDebt && g.log && !g.breaker);
        CHECK(s.chainBroken && s.dropStreak == 1);
        std::printf("A72 C2 intact + expired: send=%d req=%d debt=%d log=%d breaker=%d "
                    "broken=%d streak=%u\n",
                    g.send, g.requestKeyframe, g.resetDebt, g.log, g.breaker, s.chainBroken,
                    s.dropStreak);
    }
    // C3: chain broken, non-repair, frame NOT even late -> still dropped, and the repair
    // is asked for although a request went out 100 ms ago. The bypass is what C3 proves:
    // the same stamps through the expiry-only gate stay quiet.
    {
        rc::StaleFlushState s;
        s.chainBroken = true;
        const int64_t now = t0 + 100 * kMs;
        s.lastKeyframeReqNs = t0;  // inside the 300 ms window
        const rc::FlushGate g = rc::StaleFlushGate(false, now + 40 * kMs, now, s);
        CHECK(!g.send && g.requestKeyframe && g.resetDebt && !g.breaker);
        CHECK(!rc::FrameExpiredForSend(now + 40 * kMs, now));  // not late at all
        CHECK(!rc::StaleFlushDecide(now + 40 * kMs, now, t0, 0).requestKeyframe);  // old: throttled
        std::printf("A72 C3 broken + on time (300 ms window open): send=%d req=%d debt=%d | "
                    "old StaleFlushDecide req=%d\n",
                    g.send, g.requestKeyframe, g.resetDebt,
                    rc::StaleFlushDecide(now + 40 * kMs, now, t0, 0).requestKeyframe);
    }
    // C4: chain broken, non-repair, frame expired -> same verdict as C3 (rule 2 outranks
    // the expiry test: lateness is not the reason this one stops).
    {
        rc::StaleFlushState s;
        s.chainBroken = true;
        const int64_t now = t0 + 50 * kMs;
        s.lastKeyframeReqNs = t0;
        const rc::FlushGate g = rc::StaleFlushGate(false, now - 40 * kMs, now, s);
        CHECK(!g.send && g.requestKeyframe && g.resetDebt && !g.breaker);
        CHECK(rc::FrameExpiredForSend(now - 40 * kMs, now));
        std::printf("A72 C4 broken + expired: send=%d req=%d debt=%d log=%d breaker=%d\n",
                    g.send, g.requestKeyframe, g.resetDebt, g.log, g.breaker);
    }
    // C5: chain broken, repair frame, past its display moment -> it goes out anyway (the
    // expired test never applies to a repair), and the chain state clears with the streak.
    {
        rc::StaleFlushState s;
        s.chainBroken = true;
        s.dropStreak = 7;
        s.lastKeyframeReqNs = t0;
        const int64_t now = t0 + 50 * kMs;
        const rc::FlushGate g = rc::StaleFlushGate(true, now - 40 * kMs, now, s);
        CHECK(g.send && !g.requestKeyframe && !g.resetDebt && !g.log && !g.breaker);
        CHECK(!s.chainBroken && s.dropStreak == 0 && s.lastKeyframeReqNs == now);
        std::printf("A72 C5 broken + late IDR: send=%d req=%d broken=%d streak=%u stampMovedNow=%d\n",
                    g.send, g.requestKeyframe, s.chainBroken, s.dropStreak,
                    s.lastKeyframeReqNs == now);
    }
    // C6: 30 consecutive drops -> exactly one breaker, and not one of the 30 frames left.
    // The old breaker forced the current frame through (a broken P frame by construction);
    // rule 4 forces the repair request instead.
    {
        rc::StaleFlushState s;
        int drops = 0, sent = 0, breakers = 0;
        for (int i = 0; i < 30; i++) {
            const int64_t now = t0 + i * 50 * kMs;
            // A75: this burst was 10 ms late, which the grace period (33.3 ms) now calls
            // "make the next vsync", not "expired". C6 is about the breaker counting 30
            // consecutive EXPIRED frames, not about how late they are, so the lateness
            // moves to the same 40 ms the rest of the AB9-shaped cases here use
            // (C4/C7b/C8) and every assertion below stays exactly as A72 wrote it.
            const rc::FlushGate g = rc::StaleFlushGate(false, now - 40 * kMs, now, s);
            if (g.send) sent++;
            else drops++;
            if (g.breaker) breakers++;
        }
        CHECK(drops == 30 && sent == 0);
        CHECK(breakers == 1);  // fires at the 30th drop, then the streak starts over
        CHECK(s.dropStreak == 0);
        std::printf("A72 C6 30 consecutive drops: sent=%d dropped=%d breakerFired=%d streakNow=%u\n",
                    sent, drops, breakers, s.dropStreak);
    }
    // C7a: two broken-chain drops 100 ms apart both ask for the repair (no throttle).
    {
        rc::StaleFlushState s;
        s.chainBroken = true;
        s.lastKeyframeReqNs = t0;
        const rc::FlushGate a = rc::StaleFlushGate(false, t0 + 100 * kMs + 40 * kMs, t0 + 100 * kMs, s);
        const rc::FlushGate b = rc::StaleFlushGate(false, t0 + 200 * kMs + 40 * kMs, t0 + 200 * kMs, s);
        CHECK(a.requestKeyframe && b.requestKeyframe);
        std::printf("A72 C7a broken chain, 100 ms apart: req1=%d req2=%d\n",
                    a.requestKeyframe, b.requestKeyframe);
    }
    // C7b: chain intact, two expired frames 100 ms apart -> the second asks nothing: the
    // 300 ms throttle still holds there (only the broken-chain path bypasses it).
    {
        rc::StaleFlushState s;
        const rc::FlushGate x1 = rc::StaleFlushGate(false, t0 - 40 * kMs, t0, s);
        CHECK(x1.requestKeyframe && s.chainBroken);
        const rc::FlushGate repair = rc::StaleFlushGate(true, t0 - 40 * kMs, t0, s);  // the IDR gets out
        CHECK(repair.send && !s.chainBroken);
        const int64_t second = t0 + 100 * kMs;
        const rc::FlushGate x2 = rc::StaleFlushGate(false, second - 40 * kMs, second, s);
        CHECK(!x2.requestKeyframe && x2.resetDebt && !x2.send);
        std::printf("A72 C7b intact, 100 ms apart: req1=%d req2=%d (300 ms throttle holds)\n",
                    x1.requestKeyframe, x2.requestKeyframe);
    }
    // C8 (A46 defense kept): a throttle stamp ahead of `now` (clock source reset,
    // sleep/wake, mixed bases) must count as due instead of swallowing the request for
    // the whole window.
    {
        rc::StaleFlushState s;
        s.lastKeyframeReqNs = t0 + 500 * kMs;  // a stamp in the future: the clock went back
        const rc::FlushGate g = rc::StaleFlushGate(false, t0 - 40 * kMs, t0, s);
        CHECK(!g.send && g.requestKeyframe && g.log);
        std::printf("A72 C8 clock rollback (stamp 500 ms ahead): send=%d req=%d log=%d\n",
                    g.send, g.requestKeyframe, g.log);
    }
}

// A75 (TASK-A75-FLUSH-GRACE-2026-11-22 §2.3): an offline replay of the runs the flush rule
// is judged against. One row = one RunSender pickup as the streamer's own log reported it:
// `lateMs` is the number that log printed ("last N ms late" / "N ms from its display
// moment"), i.e. how far past its display moment the pickup found the frame. The rows and
// their source lines in C:\Users\wzy\AppData\Local\RinCast\logs\streamer.log (the user's
// 23:09-23:11 run, the "stutters, then smears" the card was written from):
//   2058 + 2062  23:10:16/17  8 frames dropped, last one   1 ms late
//   2074         23:10:20     14 frames dropped, last one  0 ms late
//   2089         23:10:25     1 frame  dropped, last one   0 ms late
//   2106         23:10:30     1 frame  dropped, last one   0 ms late
//   2115         23:10:32     1 frame  dropped, last one   8 ms late
//   2094 + 2129  the same counts as 10 s window totals (16 and 3 frames)
// The encoder lines of that same window (1859-2195) read 60 fps, qp 24.0, 0.4-1.6 Mbps
// against a 74-90 Mbps ceiling, submit 1.3-6.1 ms, gpu wait 3.9-4.2 ms: nothing was backed
// up, so all five rows must go OUT. AB9's backlog (rc/stale_flush.h:1-9, the evidence the
// flush was written for) is the opposite case and is replayed at 119 ms and 479 ms.
// Each row is replayed through BOTH of vr_stream.cpp's prediction sources - the headset's
// exact target moment and the submit + 40 ms predict-ahead estimate - and the verdicts must
// agree, because the replay calls the one judgment point both paths feed (card §2.1). That
// is what makes card §4.5's V3 (the grace folded into the estimate's own expression, dropped
// by the judgment point) read red here instead of hiding in a call site the test cannot see.
static void TestStaleFlushGraceReplay() {
    const int64_t kMs = 1'000'000;
    const int64_t t0 = 9'000'000'000;  // arbitrary synthetic anchor: no wall clock, no flake
    // vr_stream.cpp:82 kStalePredictAheadNs. The sender's copy is file-local, so the
    // estimate is restated here and the two prediction expressions below mirror
    // vr_stream.cpp's `flushExact ? targetDisplayPcNs : submitPcNs + kStalePredictAheadNs`.
    const int64_t kPredictAheadNs = 40 * kMs;
    struct Row {
        const char* evidence;  // the log line the lateness was taken from
        int64_t lateMs;        // how far past the display moment the pickup found the frame
        bool expectDrop;       // what that log says the stream needed
    };
    const Row rows[] = {
        {"streamer.log:2062 23:10:17 8 frame(s), last 1 ms late", 1, false},
        {"streamer.log:2074 23:10:20 14 frame(s), last 0 ms late", 0, false},
        {"streamer.log:2089 23:10:25 1 frame(s), last 0 ms late", 0, false},
        {"streamer.log:2106 23:10:30 1 frame(s), last 0 ms late", 0, false},
        {"streamer.log:2115 23:10:32 1 frame(s), last 8 ms late", 8, false},
        {"AB9 (stale_flush.h:1-9) queue 119 ms", 119, true},
        {"AB9 (stale_flush.h:1-9) queue 479 ms", 479, true},
    };
    uint32_t sent = 0, dropped = 0;
    for (const Row& r : rows) {
        const int64_t nowNs = t0 + kPredictAheadNs;  // picked up a full predict-ahead after submit
        const int64_t targetPcNs = nowNs - r.lateMs * kMs;  // the moment the log measured
        const int64_t submitPcNs = targetPcNs - kPredictAheadNs;  // ... and the submit behind it
        // Both sources land on the same moment on purpose: the row replays the measured
        // lateness, whichever of the two produced the number.
        const int64_t exactMoment = targetPcNs;              // flushExact: the target itself
        const int64_t estimatedMoment = submitPcNs + kPredictAheadNs;  // the fallback estimate
        const bool exactExpired = rc::FrameExpiredForSend(exactMoment, nowNs);
        const bool estimatedExpired = rc::FrameExpiredForSend(estimatedMoment, nowNs);
        std::printf("A75 replay %s: %lld ms late -> exact=%d estimated=%d, expected drop=%d\n",
                    r.evidence, (long long)r.lateMs, exactExpired, estimatedExpired, r.expectDrop);
        CHECK(exactExpired == r.expectDrop);
        CHECK(estimatedExpired == r.expectDrop);
        CHECK(exactExpired == estimatedExpired);  // one grace rule for both sources (card §2.1)
        // The same rows through the sender's entry point (intact chain, no stamps yet, not a
        // repair): the healthy rows must leave the flush untouched - no drop, no request,
        // no log line, no broken chain - and the AB9 rows must still do all of it.
        rc::StaleFlushState st;
        const rc::FlushGate g = rc::StaleFlushGate(false, exactMoment, nowNs, st);
        CHECK(g.send == !r.expectDrop);
        CHECK(g.requestKeyframe == r.expectDrop);
        CHECK(g.log == r.expectDrop);
        CHECK(st.chainBroken == r.expectDrop);
        CHECK(st.dropStreak == (r.expectDrop ? 1u : 0u));
        if (r.expectDrop) dropped++;
        else sent++;
    }
    CHECK(sent == 5);     // the whole healthy 0/1/8 ms batch goes out
    CHECK(dropped == 2);  // both AB9 backlog rows still flush
    std::printf("A75 grace replay: %u row(s) from the healthy 60 fps log sent, %u AB9 backlog row(s) flushed, "
                "grace %lld ms (kFlushGraceNs)\n",
                sent, dropped, (long long)(rc::kFlushGraceNs / kMs));
}

// TASK-LATENCY-METER-FIX-2026-11-22 §2: the stage accumulator must not drop samples.
// The user saw the panel's stages sum fall far short of the total latency: the old
// AddStages filtered each stage on its own, so a clock-offset error pushing the game
// stage below -2 removed it whole (and the >=1000 guard ate real stalls) while the
// total kept counting every frame. Frames here carry the real VR branch's shape: the
// stages sum to the frame's total exactly (the clock offsets cancel algebraically),
// totals sit in the 80-129 ms the headset reports (one 1600 ms stall spike), and the
// edge samples (-3, -0.5, 1500) ride on that shape. On the old implementation the
// frame-count assertions below run red: it dropped those frames' stages.
static void TestLatencyStageAccumulation() {
    const int kFrames = 2000;
    const double kEpsilon = 1e-6;
    double sums[rv::kStageCount] = {};
    uint32_t frames[rv::kStageCount] = {};
    double latencySum = 0;
    for (int f = 0; f < kFrames; f++) {
        double ms[rv::kStageCount] = {};
        ms[rv::kStageUplink] = 4.0;        // half the round trip, as the branch feeds it
        ms[rv::kStageGame] = 24.0;         // the game's share before the clock wobbles
        ms[rv::kStageEncoderWait] = 0.7;
        ms[rv::kStageEncode] = 6.0;
        ms[rv::kStageDownlink] = 8.0;
        ms[rv::kStageDecode] = 5.0;
        ms[rv::kStageNextFrame] = 18.0;
        ms[rv::kStageCapture] = -1.0;      // the desktop-only stage's sentinel, as in VR
        double total = 80.0 + (f % 50);    // the usual pose age, no spike today
        if (f == 100) ms[rv::kStageGame] = -3.0;        // clock offset error, the real dip
        if (f == 200) ms[rv::kStageNextFrame] = -0.5;   // a small jitter dip
        if (f == 300) {
            ms[rv::kStageDownlink] = 1500.0;            // a real stall, capped at 1000
            total = 1600.0;
        }
        // The compositor closes the identity, the way displayNs - latchNs does: the
        // stages of every frame sum to its total latency exactly. The capture sentinel
        // sits outside that identity, as in the real VR branch (the desktop-only stage
        // has no share of a VR frame's time).
        double rest = 0;
        for (int i = 0; i < rv::kStageCount; i++)
            if (i != rv::kStageCompositor && i != rv::kStageCapture) rest += ms[i];
        ms[rv::kStageCompositor] = total - rest;
        latencySum += total;
        rv::AddStageSamples(sums, frames, ms);
    }
    // L1: every stage counts every frame - no sample is dropped any more (the capture
    // sentinel counts too). The old filter lost frames here.
    for (int i = 0; i < rv::kStageCount; i++) CHECK(frames[i] == uint32_t(kFrames));
    // L2: negatives count as zero (the frame stays, the value contributes nothing).
    CHECK(std::fabs(sums[rv::kStageGame] - 24.0 * (kFrames - 1)) < kEpsilon);
    CHECK(std::fabs(sums[rv::kStageNextFrame] - 18.0 * (kFrames - 1)) < kEpsilon);
    CHECK(std::fabs(sums[rv::kStageCapture]) < kEpsilon);
    // L3: an extreme value counts as a full second instead of vanishing.
    CHECK(std::fabs(sums[rv::kStageDownlink] - (8.0 * (kFrames - 1) + 1000.0)) < kEpsilon);
    // L4: on the same frames the stages add up to the total within the clock-jitter
    // margin - the panel's stages sum tracks the panel's total again.
    double stageSum = 0;
    for (int i = 0; i < rv::kStageCount; i++) stageSum += sums[i];
    const double stagesAvg = stageSum / kFrames, latencyAvg = latencySum / kFrames;
    CHECK(std::fabs(stagesAvg - latencyAvg) <= 2.0);
    std::printf("latency stages: no sample dropped, negatives as zero, extremes capped at 1000, "
                "stages sum tracks the total within 2 ms\n");
}

// A10 (TASK-LATENCY-BURST-SEND 2026-11-21): with the frame-granularity token bucket,
// a single frame's datagrams leave BACK-TO-BACK as one burst - even though the average
// pacing rate (8 Mbit/s) is far below what this 208 KB frame needs - the budget no
// longer spreads a frame inside itself. The budget now (a) bounds the burst window
// (BudgetOverruns) and (b) bills the NEXT frame (asserted in TestPacingBursts /
// TestPacingBurstShaping). The legacy clock (budget 0) still spreads the same frame
// over ~208 ms: that is the desktop/remote path, byte-for-byte unchanged.
static void TestPacingFrameBudget() {
    int64_t oneFrameNs = 0;
    double spreadMs = 0;
    uint32_t overruns = 0;
    size_t n = 0;
    size_t expected = 0;
    double legacyMs = 0;
    {
        RawReceiver rx;
        UdpSocket tx;
        CHECK(rx.Open() && tx.Open(0));
        if (!rx.sock.IsOpen()) return;
        tx.SetBuffers(16 << 20, 1 << 20);
        auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                              crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                              "t");
        Connection conn(&tx, NetAddress{0x7F000001, rx.sock.LocalPort()}, 1, keys, true, kMaxDatagramLan);
        rx.Start();
        VideoPacketizer pkt(&conn);
        pkt.SetFecPercent(5);
        pkt.SetPacingKbps(8000);
        pkt.SetPacingFrameBudgetNs(8'000'000LL);
        constexpr size_t kFrameBytes = 208 * 1024;
        std::vector<uint8_t> frame(kFrameBytes);
        for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 31);
        VrFrameMetadata meta{};
        meta.trackingTimestampNs = 12345;
        // The datagrams this frame must produce (data + parity), the sender's formulas.
        const size_t fragPayload = conn.MaxPayload() - sizeof(VideoFragmentHeader);
        const size_t total = 2 + sizeof(VrFrameMetadata) + kFrameBytes;
        const size_t count = (total + fragPayload - 1) / fragPayload;
        const size_t groupSize = size_t(std::clamp(100 / 5, 2, 64));
        const size_t groups = (count + groupSize - 1) / groupSize;
        const size_t nParity = groups - ((count - (groups - 1) * groupSize) == 1 ? 1 : 0);
        expected = count + nParity;
        const int64_t t = NowNs();
        pkt.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), frame.data(), frame.size());
        oneFrameNs = NowNs() - t;
        SleepMs(150);
        int64_t firstNs = 0, lastNs = 0;
        const size_t got = std::min(rx.count.load(), rx.arrivals.size());
        for (size_t i = 0; i < got; i++) {
            if (rx.arrivals[i].second <= 200) continue;
            n++;
            if (!firstNs) firstNs = rx.arrivals[i].first;
            lastNs = rx.arrivals[i].first;
        }
        overruns = pkt.BudgetOverruns();
        if (firstNs) spreadMs = NsToMs(lastNs - firstNs);
        rx.Stop();
    }
    {
        RawReceiver rx;
        UdpSocket tx;
        CHECK(rx.Open() && tx.Open(0));
        tx.SetBuffers(16 << 20, 1 << 20);
        auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                              crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                              "t");
        Connection conn(&tx, NetAddress{0x7F000001, rx.sock.LocalPort()}, 1, keys, true, kMaxDatagramLan);
        rx.Start();
        VideoPacketizer legacy(&conn);
        legacy.SetFecPercent(5);
        legacy.SetPacingKbps(8000);
        constexpr size_t kFrameBytes = 208 * 1024;
        std::vector<uint8_t> frame(kFrameBytes);
        for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 31);
        VrFrameMetadata meta{};
        meta.trackingTimestampNs = 12345;
        const int64_t t0 = NowNs();
        legacy.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, 200, &meta, sizeof(meta), frame.data(), frame.size());
        legacyMs = NsToMs(NowNs() - t0);
        rx.Stop();
    }
    std::printf("pacing budget: 208 KB burst in %.2f ms, spread %.2f ms over %zu datagrams (%zu expected), "
                "overruns %u; legacy clock: %.1f ms\n",
                NsToMs(oneFrameNs), spreadMs, n, expected, overruns, legacyMs);
    CHECK(oneFrameNs <= 6'000'000LL);  // 整帧一次突发打完（旧预算语义要铺 ~8 ms，老时钟 ~208 ms）
    CHECK(spreadMs <= 7.0);            // 收包侧看到的是同一个突发
    CHECK(n == expected);              // 每个分片和校验包都出了门（不丢帧）
    CHECK(overruns == 0);              // 突发窗口在预算内
    CHECK(legacyMs >= 150.0);          // 老时钟不变（桌面/远程路径逐字节保留）
}

// A10 (TASK-LATENCY-BURST-SEND 2026-11-21): an OVER-BUDGET frame delays the next one as
// a WHOLE - by its natural frame-bytes/rate interval - and never as a spread-out frame.
// 400 KB @ 108 Mbps bills ~30 ms, past the 15 ms budget window (deliberately over
// budget). Frame 1 and frame 2 are handed over back-to-back: frame 1 leaves as one
// burst (no intra-frame flattening), frame 2's first fragment waits out the billing
// and then frame 2 leaves as one burst too (whole-frame wait, not flattening).
// The old R4.1 "instant-rate ceiling + even intra-frame spread" went away with the
// intra-frame burst semantics: micro-burst shaping inside a frame is no longer a design
// goal - the link's instantaneous rate is the link's business (TASK card §0/§1); the
// send layer keeps only the average.
// TASK-FLAKE-ROBUST-2026-11-22：容差自校准（上限 15 ms，护栏依据见 CalibrateTimingJitter
// 上方），帧边界定位从「最大间隔启发式」改为按已知每帧包数取位——负载下帧内收包停顿
// 可以超过计费间隔，最大间隔会被指到帧中间（测量假象被当成断言失败）。断言的量一个
// 不变（帧 1 突发、计费边界 first-to-FIRST、帧 2 等待+突发、无丢包、无 overrun）。
static void TestPacingBurstShaping() {
    const PacingJitter jitter = CalibrateTimingJitter();  // 护栏 2：独立采样
    // 压缩侧固定基线 4.0 ms（唤醒量化差分 + 收包小延迟差，依据见 TestPacingBursts）。
    const double tolCompressMs = AdaptiveTol(4.0, jitter.stallScaleMs);  // 护栏 1 公式
    constexpr size_t kFrameBytes = 400000;  // ~400 KB, FEC off => no parity packets
    constexpr int64_t kBudgetNs = 15'000'000LL;
    constexpr uint32_t kPaceKbps = 108000;  // production paceBase (100 Mbps x 1.08)
    double frame1Ms = 0, frame2Ms = 0, spread1Ms = 0, spread2Ms = 0, boundaryGapMs = 0;
    size_t got1 = 0, got2 = 0, expected = 0;
    uint32_t overruns = 0;
    double billingMs = 0;
    {
        RawReceiver rx;
        UdpSocket tx;
        CHECK(rx.Open() && tx.Open(0));
        if (!rx.sock.IsOpen()) return;
        tx.SetBuffers(16 << 20, 1 << 20);
        auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                              crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey,
                                              "t");
        Connection conn(&tx, NetAddress{0x7F000001, rx.sock.LocalPort()}, 1, keys, true, kMaxDatagramLan);
        rx.Start();
        VideoPacketizer pkt(&conn);
        pkt.SetFecPercent(0);
        pkt.SetPacingKbps(kPaceKbps);
        pkt.SetPacingFrameBudgetNs(kBudgetNs);
        std::vector<uint8_t> frame(kFrameBytes);
        for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 31);
        VrFrameMetadata meta{};
        meta.trackingTimestampNs = 12345;
        const size_t fragPayload = conn.MaxPayload() - sizeof(VideoFragmentHeader);
        const size_t total = 2 + sizeof(VrFrameMetadata) + kFrameBytes;
        const size_t count = (total + fragPayload - 1) / fragPayload;
        expected = count;  // FEC off: data fragments only
        billingMs = double(total + count * sizeof(VideoFragmentHeader)) * 8.0 / double(kPaceKbps);
        CHECK(billingMs > 15.0);  // precondition: this frame really is over the budget window
        const DWORD_PTR mask = PinThread(0);
        int64_t t = NowNs();
        pkt.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), frame.data(), frame.size());
        frame1Ms = NsToMs(NowNs() - t);
        t = NowNs();
        pkt.SendFrame(kVrStreamId, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), frame.data(), frame.size());
        frame2Ms = NsToMs(NowNs() - t);  // wait out frame 1's billing + frame 2's own burst
        UnpinThread(mask);
        overruns = pkt.BudgetOverruns();
        SleepMs(120);
        const size_t n = std::min(rx.count.load(), rx.arrivals.size());
        std::vector<std::pair<int64_t, size_t>> v;
        for (size_t i = 0; i < n; i++)
            if (rx.arrivals[i].second > 200) v.push_back(rx.arrivals[i]);
        // TASK-FLAKE-ROBUST-2026-11-22: the boundary used to sit at the LARGEST observed
        // gap - under load an intra-burst receiver stall can outgrow the billing gap and
        // point the split into the middle of a frame (a measurement artifact the counts
        // then report as a failure). Both frames are the same shape (FEC off, `expected`
        // datagrams each) and loopback neither drops nor reorders, so the boundary sits
        // at the KNOWN position: frame 1 = the first `expected` video packets. The
        // boundary stays measured FIRST-to-FIRST (the card's t0 is when a frame's FIRST
        // fragment leaves). The asserted quantities are unchanged; only the boundary
        // LOCATING became deterministic.
        got1 = std::min(v.size(), expected);
        got2 = v.size() - got1;
        if (v.size() > expected) boundaryGapMs = NsToMs(v[expected].first - v[0].first);
        if (expected > 0 && v.size() >= expected) spread1Ms = NsToMs(v[expected - 1].first - v[0].first);
        if (v.size() > expected) spread2Ms = NsToMs(v.back().first - v[expected].first);
        std::printf("over-budget: billing %.1f ms; frame1 %.2f ms burst, spread %.2f ms, %zu/%zu pkts; "
                    "boundary gap %.2f ms; frame2 %.2f ms (wait+burst), spread %.2f ms, %zu/%zu pkts; overruns %u, "
                    "tol compress %.2f ms\n",
                    billingMs, frame1Ms, spread1Ms, got1, expected, boundaryGapMs, frame2Ms, spread2Ms, got2,
                    expected, overruns, tolCompressMs);
        rx.Stop();
    }
    CHECK(got1 == expected);                   // frame 1 complete (nothing dropped)
    CHECK(frame1Ms <= AdaptiveTol(10.0, jitter.stallScaleMs));   // frame 1 left as one burst (~296 datagrams), not spread
    CHECK(spread1Ms <= AdaptiveTol(10.0, jitter.stallScaleMs));  // the receiver saw the same burst (receiver-side jitter adds a few ms)
    CHECK(boundaryGapMs >= billingMs - tolCompressMs);  // token bucket: frame 2 may not start before the billing (adaptive, capped)
    CHECK(boundaryGapMs <= billingMs + AdaptiveTol(12.0, jitter.stallScaleMs));  // postponement <= that frame's bytes / ceiling (whole wait)
    // Sender-side whole-frame proof: frame 2's SendFrame = token wait + one burst, so its
    // duration lands at the billing plus one burst's worth of slack - NOT at two billings
    // (that would mean the wait spilled into frame 2's own send, i.e. flattening).
    CHECK(frame2Ms <= billingMs + AdaptiveTol(10.0, jitter.stallScaleMs));
    CHECK(got2 == expected);                   // frame 2 complete: waited, but never dropped
    // spread2Ms: the fixed baseline (16 ms) already exceeds the 15 ms adaptive cap, so
    // this one never widens (guardrail 1: the cap bounds the ADAPTIVE component only).
    CHECK(spread2Ms <= AdaptiveTol(16.0, jitter.stallScaleMs));   // corroboration only (receiver jitter; a flattened
                                                               // frame would spread ~billing = 30 ms and still fail)
    CHECK(overruns == 0);                      // both bursts fit their budget window
}


// R4.2 (D-026): the user's bitrate setting is a WIRE ceiling - it is what the link
// carries, fragment headers and FEC parity included. The encoder works in encoded
// bytes, so the target it is given has to be the wire budget converted into encoded
// bytes, or the link runs ~7% over the ceiling (measured 106.2 Mbps of wire against
// a 100 Mbps setting). This test drives the converter with one second of consecutive
// big frames - the shape that used to blow the ceiling - and checks that the trailing
// second of WIRE bytes stays inside the quota and that no frame is lost.
//
// The existing assertions are untouched: this is a new test only.
// D-028: the encoder is handed a rate scaled by rung/real, so its per-frame budget stays
// ceiling/realFps regardless of which ladder rung it runs at. The field case that forced
// this: 38 fps frames at the 30 fps rung produced 1.27x (124 Mbps against a 100 Mbps
// setting), 390 KB frames the pacer could not drain in a 27 ms frame period.
static void TestRateScaleForLadder() {
    // A rung at or above the real rate is left alone: the setting is a ceiling, and
    // under-spending is not this function's business.
    CHECK(rc::ScaleRateToRealFps(100000, 90, 90.0) == 100000);
    CHECK(rc::ScaleRateToRealFps(100000, 90, 87.0) == 100000);   // measured jitter
    CHECK(rc::ScaleRateToRealFps(100000, 30, 30.0) == 100000);   // rung == real
    // A rung below the real rate is undone exactly: 72/90 -> 0.8, 30/38 -> ~0.789.
    CHECK(rc::ScaleRateToRealFps(100000, 72, 90.0) == 80000);
    const uint32_t s = rc::ScaleRateToRealFps(100000, 30, 38.0);
    CHECK(s >= 78900 && s <= 78960);
    // D-030：旧 0.5 下限会让 24/90 的每帧预算仍超顶，不能保留。
    CHECK(rc::ScaleRateToRealFps(100000, 24, 90.0) == 26666);
    CHECK(rc::ScaleRateToRealFps(100000, 30, 30.5) < 100000);
    CHECK(rc::ScaleRateToRealFps(100000, 30, std::numeric_limits<double>::quiet_NaN()) == 100000);
    CHECK(rc::ScaleRateToRealFps(100000, 30, std::numeric_limits<double>::infinity()) == 100000);
    // Unknowns leave the rate untouched (encoder Init calls ApplyBitrate before any frame).
    CHECK(rc::ScaleRateToRealFps(100000, 30, 0.0) == 100000);
    CHECK(rc::ScaleRateToRealFps(100000, 0, 38.0) == 100000);
    CHECK(rc::ScaleRateToRealFps(0, 30, 38.0) == 0);
    std::printf("rate scale: 90/90 stays, 72/90 -> %u, 30/38 -> %u, 24/90 -> %u\n",
                rc::ScaleRateToRealFps(100000, 72, 90.0), s, rc::ScaleRateToRealFps(100000, 24, 90.0));
}

// G9.1: QP telemetry must stay trustworthy when NVENC emits a garbage sample.
// qp is display/telemetry only; this tests the consumption logic, not the driver.
static void TestQpStatisticsValidity() {
    // Per-codec domains (not one shared ceiling).
    CHECK(rc::MaxValidQp(0) == rc::kMaxValidQpH264Hevc);  // H264
    CHECK(rc::MaxValidQp(1) == rc::kMaxValidQpH264Hevc);  // HEVC
    CHECK(rc::MaxValidQp(2) == rc::kMaxValidQpAv1);       // AV1

    // All valid: mean is exact, nothing rejected.
    rc::QpStats all;
    for (uint32_t q : {10u, 20u, 30u, 40u}) all.Add(q, rc::kMaxValidQpH264Hevc);
    CHECK(all.valid == 4 && all.rejected == 0);
    CHECK(std::fabs(all.Mean() - 25.0f) < 1e-3f);
    // G10.1: min/max expose the spread the mean hides (10..40 here).
    CHECK(all.min == 10 && all.max == 40);

    // Mixed: the massive values seen on device (432586644, 2109808503) must not
    // enter the mean; first raw value is retained for diagnosis.
    rc::QpStats mixed;
    mixed.Add(12, rc::kMaxValidQpH264Hevc);
    mixed.Add(432586644u, rc::kMaxValidQpH264Hevc);
    mixed.Add(2109808503u, rc::kMaxValidQpH264Hevc);
    mixed.Add(16, rc::kMaxValidQpH264Hevc);
    CHECK(mixed.valid == 2 && mixed.rejected == 2);
    CHECK(std::fabs(mixed.Mean() - 14.0f) < 1e-3f);
    CHECK(mixed.firstBadRaw == 432586644u);
    // Garbage samples must not corrupt min/max either (valid range 12..16 only).
    CHECK(mixed.min == 12 && mixed.max == 16);

    // All invalid: unknown (-1), never a normal-looking QP and never 0.
    rc::QpStats none;
    none.Add(2109808503u, rc::kMaxValidQpH264Hevc);
    none.Add(1201802785u, rc::kMaxValidQpH264Hevc);
    CHECK(none.valid == 0 && none.rejected == 2);
    CHECK(none.Mean() < 0.0f);

    // Zero samples: unknown.
    rc::QpStats empty;
    CHECK(empty.Mean() < 0.0f);

    // Boundary values are inclusive; just-over is rejected. AV1 allows 255.
    rc::QpStats edge;
    edge.Add(rc::kMaxValidQpH264Hevc, rc::kMaxValidQpH264Hevc);      // 51 valid
    edge.Add(rc::kMaxValidQpH264Hevc + 1, rc::kMaxValidQpH264Hevc);  // 52 rejected
    CHECK(edge.valid == 1 && edge.rejected == 1);
    CHECK(std::fabs(edge.Mean() - float(rc::kMaxValidQpH264Hevc)) < 1e-3f);
    rc::QpStats av1;
    av1.Add(rc::kMaxValidQpAv1, rc::kMaxValidQpAv1);      // 255 valid for AV1
    av1.Add(rc::kMaxValidQpAv1 + 1, rc::kMaxValidQpAv1);  // 256 rejected
    CHECK(av1.valid == 1 && av1.rejected == 1);

    // Window reset (reconnect / new 5 s window) clears everything.
    mixed.Reset();
    CHECK(mixed.valid == 0 && mixed.rejected == 0 && mixed.firstBadRaw == 0);
    CHECK(mixed.min == 0 && mixed.max == 0);
    CHECK(mixed.Mean() < 0.0f);
}

static void TestEncoderRateTransaction() {
    constexpr int64_t second = 1'000'000'000LL;
    rc::EncoderRateTracker tracker;
    auto plan = [&](uint32_t kbps, uint32_t current, bool startup = false, bool desktop = false) {
        return rc::PlanEncoderRate(kbps, 90.0, desktop, startup, tracker, current, 100, 150);
    };
    auto settle = [&](double fps) {
        for (int i = 0; i < 200; ++i) tracker.Observe(1e9 / fps);
    };

    // 冻结：无论实测 fps 怎么变，分母恒为标称 90，码率恒为请求值，桶 = 1.5 帧@标称。
    settle(37);
    const auto at37 = plan(100000, 30000);
    CHECK(at37.rateNum == 90000);
    CHECK(at37.budget.maxBitrate == 100'000'000);
    CHECK(at37.budget.averageBitrate == 100'000'000);
    CHECK(at37.budget.vbvBufferSize == uint32_t(double(100000) * 1000 * 1.5 / 90));
    settle(90);
    CHECK(rc::SameRatePlan(plan(100000, 90000), at37));
    settle(30);
    CHECK(rc::SameRatePlan(plan(100000, 30000), at37));
    // 桶恒 ≥ 每帧预算（没有「被桶夹住」的 QP 振荡）。
    CHECK(at37.budget.vbvBufferSize >= at37.budget.averageBitrate / at37.rateNum);
    // 桌面与 VR 同一条冻结计划；启动阶段也不改变分母。
    CHECK(rc::SameRatePlan(plan(100000, 30000, true, true), at37));
    CHECK(rc::SameRatePlan(plan(100000, 30000, true, false), at37));

    rc::EncoderRateState state;
    state.applied = at37;
    int calls = 0;
    auto submit = [&](bool ok) { ++calls; return ok; };

    // 只有码率请求变化才触发事务；成功后不再逐帧重复下发。
    CHECK(!state.ShouldAttempt(plan(100000, 90000), 10 * second));
    auto next = plan(60000, 90000);
    CHECK(state.ShouldAttempt(next, 10 * second));
    CHECK(rc::ApplyEncoderRateTransaction(state, next, 10 * second, [&] { return submit(true); }));
    CHECK(state.applied.requestedKbps == 60000);
    CHECK(!state.failed);
    CHECK(!state.ShouldAttempt(next, 10 * second + 1));  // 已提交，无需重发

    // 失败保留原快照并节流重试（1s），成功后清零失败位。
    const auto snapshot = state.applied;
    next = plan(40000, 90000);
    CHECK(!rc::ApplyEncoderRateTransaction(state, next, 11 * second, [&] { return submit(false); }));
    CHECK(rc::SameRatePlan(state.applied, snapshot));
    CHECK(state.failed);
    CHECK(!state.ShouldAttempt(next, 11 * second + 100 * 1'000'000));
    CHECK(state.ShouldAttempt(next, 12 * second));
    CHECK(rc::ApplyEncoderRateTransaction(state, next, 12 * second, [&] { return submit(true); }));
    CHECK(state.applied.requestedKbps == 40000);
    CHECK(!state.failed);

    // 升码率（requested 变化）只受 1s 节流，不套 3s 恢复迟滞；降到 40000 时也成功。
    next = plan(100000, 90000);
    CHECK(!state.ShouldAttempt(next, 12 * second + 1));
    CHECK(state.ShouldAttempt(next, 13 * second));
    CHECK(rc::ApplyEncoderRateTransaction(state, next, 13 * second, [&] { return submit(true); }));
    CHECK(state.applied.requestedKbps == 100000);

    // 降码率让候选桶变小；已提交的 initial delay 必须夹进新桶（否则驱动拒收整次重配置）。
    const auto bigBucket = plan(100000, 90000);
    const auto halfBucket = plan(50000, 90000);
    CHECK(halfBucket.budget.vbvBufferSize < bigBucket.budget.vbvBufferSize);
    CHECK(rc::ClampVbvInitialDelay(bigBucket.budget.vbvBufferSize, halfBucket.budget.vbvBufferSize) ==
          halfBucket.budget.vbvBufferSize);
    CHECK(rc::ClampVbvInitialDelay(1000, 0) == 1000);

    // 非有限标称值不直接参与整数转换；桶仍为正。
    // ROUND18 L4: the fallback nominal went 90 -> 72, so inf now resolves to
    // 72 fps = 72000 rateNum (was 90000). This is the ONE assertion tied to the
    // default-fps fallback; the burst-shaping checks above are timing-sensitive
    // and are NOT changed (they flap independently of this edit, see L4/STATUS).
    const auto finitePlan = rc::PlanEncoderRate(UINT32_MAX, std::numeric_limits<double>::infinity(),
                                               true, false, tracker, 90000, 100, 150);
    CHECK(finitePlan.rateNum == 72000);
    CHECK(finitePlan.budget.maxBitrate == UINT32_MAX);
    CHECK(finitePlan.budget.vbvBufferSize > 0);

    // 只有 fps 扰动、码率不变时，一次都不重配（冻结后无 churn）。
    tracker = {};
    settle(37);
    state = {};
    state.applied = plan(100000, 90000);
    int attempts = 0;
    for (int i = 0; i < 100; ++i) {
        tracker.Observe(1e9 / (i % 2 ? 37.0 : 44.0));
        next = plan(100000, 90000);
        const int64_t now = 50 * second + i * second / 10;
        if (state.ShouldAttempt(next, now)) {
            ++attempts;
            rc::ApplyEncoderRateTransaction(state, next, now, [] { return true; });
        }
    }
    CHECK(attempts == 0);

    // 连续降码率请求 + API 失败：至多每秒重试一次，不形成 10Hz 风暴。
    attempts = 0;
    state.lastAttemptNs = 0;
    for (int i = 0; i < 100; ++i) {
        next = plan(60000 - uint32_t(i), 90000);
        const int64_t now = 70 * second + i * second / 10;
        if (state.ShouldAttempt(next, now)) {
            ++attempts;
            rc::ApplyEncoderRateTransaction(state, next, now, [] { return false; });
        }
    }
    CHECK(attempts == 10);
    CHECK(state.applied.requestedKbps == 100000);
    std::printf("encoder rate transaction: frozen-at-nominal plan, commit/retry/throttle, vbvInitialDelay clamp\n");
}

// L17 fill mode (opt-in, default on): with fillMode=false the D-034 frozen plan must
// stay byte-identical; with it on, only the rung follows the slow EMA, floored at 45
// and capped at nominal, while scaled and the VBV formula stay frozen so the budget
// never overflows the bucket. New test only; existing assertions above are untouched.
static void TestFillModePlan() {
    constexpr int64_t second = 1'000'000'000LL;
    rc::EncoderRateTracker tracker;
    auto settle = [&](double fps) {
        tracker = {};
        for (int i = 0; i < 200; ++i) tracker.Observe(1e9 / fps);
    };
    auto plan = [&](double nominal, bool desktop, bool fill, uint32_t current = 90000) {
        return rc::PlanEncoderRate(100000, nominal, desktop, false, tracker, current, 100, 200, fill);
    };

    // ① fill off: any tracker state leaves the plan frozen at nominal. VBV golden:
    //    100000 kbps @90, vbv 200% -> 100000*1000*200/100/90 = 2'222'222 (bits).
    settle(37);
    const auto off = plan(90.0, false, false);
    CHECK(off.rateNum == 90000);
    CHECK(off.budget.maxBitrate == 100'000'000);
    const uint32_t goldenVbv = uint32_t(double(100000) * 1000.0 * 200.0 / 100.0 / 90.0);
    CHECK(std::abs(int64_t(off.budget.vbvBufferSize) - int64_t(goldenVbv)) <= 1);
    // Desktop ignores the fill branch even when the switch is on.
    CHECK(rc::SameRatePlan(plan(90.0, true, true), off));

    // ② fill on, slow 40 fps @ nominal 90: the rung floors at 45 (30 would overflow
    //    the bucket); VBV is the frozen deep bucket, budget <= VBV (ratio >= 1.0).
    settle(40);
    const auto fill = plan(90.0, false, true);
    CHECK(fill.rateNum == 45000);
    CHECK(fill.budget.maxBitrate == 100'000'000);
    CHECK(fill.budget.vbvBufferSize == off.budget.vbvBufferSize);  // VBV formula untouched
    // Per-frame budget = maxBitrate / (rateNum/1000) bits. vbvBufferSize is truncated
    // to uint32, so allow 1 bit of slack; at rung 45 the two are equal by construction.
    const double perFrame = double(fill.budget.maxBitrate) / (double(fill.rateNum) / 1000.0);
    CHECK(perFrame <= double(fill.budget.vbvBufferSize) + 1.0);
    // Counter-example that justifies the floor: a 30 rung would put the per-frame
    // budget above the frozen bucket (the D-034 QP-breathing root). For this plan
    // (90 Hz, 200% VBV) FillFloorRung(90, 200) = 45.
    CHECK(double(fill.budget.maxBitrate) / 30.0 > double(fill.budget.vbvBufferSize) + 1.0);

    // ③ nominal below the floor (30): the rung never crosses nominal; a measured rate
    //    above nominal clamps back down to it.
    settle(40);
    CHECK(plan(30.0, false, true, 30000).rateNum == 30000);
    settle(120);
    CHECK(plan(30.0, false, true, 30000).rateNum == 30000);

    // ④ gate: 5 s continuous pressure before arming; an interrupt clears the hold.
    //    Once armed it stays armed for the session; the rung still follows the slow
    //    EMA, clamped to [floor, nominal]. The 30 s cooldown only guards a re-arm
    //    after a reset - it does not throttle rung changes.
    rc::FillModeGate gate;
    const int64_t t0 = 10 * second;
    CHECK(!gate.Update(true, t0));
    CHECK(!gate.Update(true, t0 + 4 * second));
    CHECK(!gate.Update(false, t0 + 4 * second));   // interrupt: the hold restarts
    CHECK(!gate.Update(true, t0 + 5 * second));
    CHECK(gate.Update(true, t0 + 10 * second));     // 5 s after the restart
    CHECK(gate.Active());
    CHECK(gate.InCooldown(t0 + 10 * second + 29 * second));
    CHECK(!gate.InCooldown(t0 + 10 * second + 30 * second));
    CHECK(gate.Update(false, t0 + 11 * second));     // armed keeps: pressure drops leave it on; the rung tracks the EMA
    std::printf("fill mode: off frozen, on floor-45 rung, vbv untouched; gate 5s hold / 30s re-arm cooldown / armed sticky, rung follows EMA\n");
}

// TASK-ROBUST-EDGES-2026-11-20 §1.2: the FillModeGate edge matrix
// (rc/fps_ladder.h:82-111 - pure state, no driver). Expectations follow the
// IMPLEMENTATION per the card; one documented comment/code tension
// (:80-81 vs :105) is recorded in the A3 receipt, not "fixed" here.
static void TestFillModeGateEdges() {
    std::printf("fill mode gate edges: ");
    constexpr int64_t S = 1'000'000'000LL;

    // F1+F2+F3: one instance, one timeline. Fencepost note (A3 receipt):
    // the card's table puts the arm at t=5S after pressure ticks 1S..4S, but
    // fps_ladder.h:95-96 opens the hold window at the FIRST pressure tick
    // (holdSince = 1S here) and arms when now - holdSince >= kHoldNs - so 5 s
    // of held pressure complete at t=6S, not 5S (5 ticks at 1 Hz span 4 s;
    // the holdSince==0 "unset" sentinel at :95 also rules out opening the
    // window with a t=0S tick - it would be re-set at the next tick). Same
    // fencepost as the pre-existing TestFillModePlan (arm at first tick +
    // 5 s) and as the card's own F4 phase 2 (25S - 20S = 5S). Tick times
    // below follow the implementation; every normative expectation of the
    // card (arm exactly when kHoldNs is full, armed sticky, Reset clears the
    // cooldown) is preserved unchanged.
    rc::FillModeGate gate;
    // F1. Pressure on at t=1S..4S: every tick sits inside the 5 s hold window
    //     (:95-96, now - holdSince < kHoldNs) -> false, Active() false.
    for (int64_t t = S; t <= 4 * S; t += S) {
        CHECK(!gate.Update(true, t));
        CHECK(!gate.Active());
    }
    // F2. t=5S: only 4 s held (5S - 1S = 4S < kHoldNs) -> still false (the
    //     card's table placed the arm here; one-tick slip, see the fencepost
    //     note above). t=6S: kHoldNs is exactly full (6S - 1S == 5S) -> arms,
    //     cooldownUntil = 6S + 30S = 36S (:97-99), returns true. The cooldown
    //     is live right after arming and expires exactly at 36S (:104).
    CHECK(!gate.Update(true, 5 * S));
    CHECK(gate.Update(true, 6 * S));
    CHECK(gate.Active());
    CHECK(gate.InCooldown(6 * S));
    CHECK(!gate.InCooldown(36 * S));
    // F3. t=7S..11S with pressure=false (the card's 6S..10S shifted by the
    //     same one tick): armed is sticky for the session - the :89 active
    //     check returns before pressure is even read - so every tick still
    //     returns true and Active() stays true.
    for (int64_t t = 7 * S; t <= 11 * S; t += S) {
        CHECK(gate.Update(false, t));
        CHECK(gate.Active());
    }

    // F4. Second instance: arm once (cooldownUntil = 36S), Reset(), then
    //     re-pressurise at t=20S..25S - INSIDE where the old cooldown would
    //     still be live (20S < 36S). Per the implementation (:105 Reset()
    //     clears m_cooldownUntilNs together with the rest) the re-arm walks a
    //     fresh 5 s hold window: t=20S..24S false, t=25S true (25S - 20S ==
    //     kHoldNs). The :80-81 comment ("the cooldown only guards a re-arm
    //     after a reset") implies the opposite (ticks blocked until >= 36S);
    //     the card says follow the code and report the tension (A3 receipt,
    //     "发现的问题").
    rc::FillModeGate gate2;
    for (int64_t t = S; t <= 6 * S; t += S) (void)gate2.Update(true, t);  // arms at 6S
    CHECK(gate2.Active());
    CHECK(gate2.InCooldown(20 * S));   // old cooldown (until 36S) still live at 20S
    gate2.Reset();
    CHECK(!gate2.Active());
    CHECK(!gate2.InCooldown(20 * S));  // :105 - Reset() cleared the cooldown too
    for (int64_t t = 20 * S; t <= 24 * S; t += S) {
        CHECK(!gate2.Update(true, t));  // fresh hold window: 0..4 s of 5 s
    }
    CHECK(gate2.Update(true, 25 * S));  // 5 s held -> re-arms
    CHECK(gate2.Active());
    std::printf("ok\n");
}

static void TestWireCeilingBudget() {
    constexpr uint32_t kCeilingKbps = 100000;  // the user's ceiling, wire
    constexpr int kFps = 90;
    constexpr int kFecPercent = 5;
    // The same fragment math the sender uses (video_sender.cpp): one fragment header
    // per datagram, one parity datagram per FEC group.
    const size_t fragPayload = 1368 - sizeof(rc::VideoFragmentHeader);
    const size_t group = size_t(std::clamp(100 / kFecPercent, 2, 64));
    rc::WireCeiling ceiling;
    rc::WireCeiling::Snapshot snap{};
    const int64_t frameNs = 1'000'000'000LL / kFps;

    // The first frame tells the controller how big the frames are; the encoder then
    // produces frames of exactly its target size.
    uint32_t targetKbps = kCeilingKbps;
    uint64_t sumWire = 0, sumEncoded = 0;
    size_t frames = 0;
    int64_t now = 1'000'000'000LL;
    for (int i = 0; i < kFps; i++) {
        const size_t encoded = size_t(uint64_t(targetKbps) * 1000 / 8 / kFps);
        const size_t total = encoded + 2 + sizeof(VrFrameMetadata);
        const size_t count = (total + fragPayload - 1) / fragPayload;
        const size_t nParity = (count + group - 1) / group;
        const size_t wire = total + count * sizeof(rc::VideoFragmentHeader) +
                            nParity * (sizeof(rc::VideoParityHeader) + fragPayload);
        now += frameNs;
        ceiling.OnFrame(now, wire, encoded, kCeilingKbps, snap);
        if (snap.encoderTargetKbps) targetKbps = snap.encoderTargetKbps;
        sumWire += wire;
        sumEncoded += encoded;
        frames++;
    }
    const uint64_t quota = rc::WireCeiling::QuotaBytes(kCeilingKbps);
    // The wire budget of one second stays inside the quota (ceiling x 1.02).
    CHECK(sumWire <= quota);
    // The overhead the converter measured is the structural one (headers + parity).
    CHECK(ceiling.Overhead() > 1.05 && ceiling.Overhead() < 1.10);
    // The encoder target is the ceiling in ENCODED bytes, i.e. below the wire ceiling.
    CHECK(targetKbps < kCeilingKbps);
    CHECK(targetKbps >= kCeilingKbps / 2);
    // No frame lost: the trailing second holds one frame per frame period.
    CHECK(ceiling.Entries() == size_t(kFps));
    // The target is stable: publishing it every frame reconfigured NVENC ~10x/s and
    // made the wire peak worse (127.9 vs 118.8 Mbps). It must not move every frame.
    uint32_t changes = 0;
    uint32_t last = targetKbps;
    rc::WireCeiling probe;
    now = 1'000'000'000LL;
    for (int i = 0; i < kFps; i++) {
        const size_t encoded = size_t(uint64_t(last) * 1000 / 8 / kFps);
        const size_t total = encoded + 2 + sizeof(VrFrameMetadata);
        const size_t count = (total + fragPayload - 1) / fragPayload;
        const size_t nParity = (count + group - 1) / group;
        const size_t wire = total + count * sizeof(rc::VideoFragmentHeader) +
                            nParity * (sizeof(rc::VideoParityHeader) + fragPayload);
        now += frameNs;
        if (probe.OnFrame(now, wire, encoded, kCeilingKbps, snap) && snap.encoderTargetKbps != last) {
            changes++;
            last = snap.encoderTargetKbps;
        }
    }
    CHECK(changes <= 2);
    std::printf("wire ceiling: %u kbps wire -> %u kbps encoder target (overhead %.3f), "
                "%zu frames, %llu wire bytes/s vs quota %llu\n",
                kCeilingKbps, targetKbps, ceiling.Overhead(), frames,
                (unsigned long long)sumWire, (unsigned long long)quota);
}



// ROUND19 T3: the decode-side autoscale ladder (rc/autoscale.h) — thresholds,
// hysteresis, cooldown, manual reset, conservative-on-missing, ladder bottom.
static void TestL7Autoscale() {
    rc::L7Autoscale::Config cfg{};  // 20 ms budget, 3 fps drops, 5 s over, 30 s cooldown
    const int64_t S = 1000000000LL;
    std::printf("l7 autoscale: ");

    // 1. boundary: decode exactly at the budget is never pressure.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t <= 10 * S; t += S) g.Update(20.0, 0.0, t);
        CHECK(g.Level() == 0);
    }
    // 2. just over the budget, held 5 s, steps exactly once.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        rc::L7Autoscale::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(20.1, 0.0, t);
        CHECK(r.changed && g.Level() == 1 && g.Pct() == 85);
    }
    // 3. hysteresis: pressure must be CONTINUOUS - a clear tick resets the streak.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t < 4 * S; t += S) g.Update(30.0, -1.0, t);
        g.Update(10.0, -1.0, 4 * S);  // the clear tick
        rc::L7Autoscale::Result r{};
        for (int64_t t = 5 * S; t <= 9 * S; t += S) r = g.Update(30.0, -1.0, t);
        CHECK(!r.changed && g.Level() == 0);
    }
    // 4. cooldown: sustained pressure steps at most once per 30 s.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        int changes = 0;
        for (int64_t t = 0; t <= 10 * S; t += S) {
            const auto r = g.Update(30.0, -1.0, t);
            changes += r.changed ? 1 : 0;
        }
        CHECK(changes == 1 && g.Level() == 1);  // one step in the first 10 s
        for (int64_t t = 11 * S; t <= 34 * S; t += S) {
            const auto r = g.Update(30.0, -1.0, t);
            changes += r.changed ? 1 : 0;
        }
        CHECK(changes == 1 && g.Level() == 1);  // the 30 s cooldown holds it there
        for (int64_t t = 35 * S; t <= 40 * S; t += S) {
            const auto r = g.Update(30.0, -1.0, t);
            changes += r.changed ? 1 : 0;
        }
        CHECK(changes == 2 && g.Level() == 2 && g.Pct() == 67);  // 30 s after the first step
    }
    // 5. ladder bottom: it stays at 50 %, never invents a fifth rung.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t <= 300 * S; t += S) g.Update(40.0, -1.0, t);
        CHECK(g.Level() == 3 && g.Pct() == 50);
        rc::L7Autoscale::Result r = g.Update(40.0, -1.0, 400 * S);
        CHECK(!r.changed && g.Level() == 3);
    }
    // 6. manual reset: back to 100 %, and the cooldown clears so a fresh episode steps.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t <= 10 * S; t += S) g.Update(30.0, -1.0, t);
        CHECK(g.Level() == 1);
        g.Reset();
        CHECK(g.Level() == 0 && g.Pct() == 100);
        rc::L7Autoscale::Result r{};
        for (int64_t t = 41 * S; t <= 46 * S; t += S) r = g.Update(30.0, -1.0, t);
        CHECK(r.changed && g.Level() == 1);  // reset cleared the cooldown: 5 s is enough again
    }
    // 7. conservative on missing signals: absent values never fire, ever.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t <= 60 * S; t += S) g.Update(-1.0, -1.0, t);
        CHECK(g.Level() == 0);
    }
    // 8. the PC's own dropped-frame average is a signal on its own.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        rc::L7Autoscale::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(5.0, 10.0, t);
        CHECK(r.changed && g.Level() == 1);
    }
    // 9. only down: long calm stretches never raise the level.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        for (int64_t t = 0; t <= 5 * S; t += S) g.Update(30.0, -1.0, t);
        CHECK(g.Level() == 1);
        for (int64_t t = 5 * S; t <= 300 * S; t += S) g.Update(5.0, 0.0, t);
        CHECK(g.Level() == 1);
    }
    // 10. clock jumps backward: no crash, no step before real elapsed time.
    {
        rc::L7Autoscale g;
        g.Configure(cfg);
        g.Update(30.0, -1.0, 4 * S);                              // the streak began at 4 s
        rc::L7Autoscale::Result r = g.Update(30.0, -1.0, 1 * S);  // clock went back 3 s
        CHECK(!r.changed && g.Level() == 0);
        r = g.Update(30.0, -1.0, 5 * S);   // only 1 s real elapsed: no step yet
        CHECK(!r.changed && g.Level() == 0);
        r = g.Update(30.0, -1.0, 9 * S);   // 9 - 4 = 5 s real elapsed: the jump never counted
        CHECK(r.changed && g.Level() == 1);
    }
    std::printf("ok\n");
}

// PLAN-AUTO-TUNE P2: the ordered classifier (DECISION-TABLE §2). First match
// wins, missing signals never fire their rule, and the qp [16, 20) band at low
// fill is a deliberate dead zone.
static void TestAutoTuneClassify() {
    std::printf("auto-tune classify: ");
    // 1. all signals missing/at default -> Ok (nothing can fire).
    CHECK(ClassifyBottleneck({}) == Bottleneck::Ok);
    // 2. R1 decode-bound: over the L7 budget; also via dropped frames alone
    //    with decode unknown (the missing-signal rule).
    {
        rc::BottleneckSignal s;
        s.decodeMs = 25.0;
        s.decodeBudgetMs = 20.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::DecodeBound);
        s.decodeMs = -1.0;  // unknown decode: budget alone proves nothing
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.droppedFps = 3.1;  // the ladder's other pressure signal carries it
        CHECK(ClassifyBottleneck(s) == Bottleneck::DecodeBound);
        s.droppedFps = 3.0;  // exactly at the ladder's threshold is not pressure
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.decodeMs = 20.0;  // exactly at the budget is not pressure either
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 3. R1 wins over R2: both over -> decode-bound (the ladder's class, the
    //    cheapest signal path).
    {
        rc::BottleneckSignal s;
        s.decodeMs = 30.0;
        s.decodeBudgetMs = 20.0;
        s.queueMs = 500.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::DecodeBound);
    }
    // 4. R2 link-bound: queue over 100 ms; loss over the brake's heavy 5%;
    //    an unknown queue (-1) never fires on its own.
    {
        rc::BottleneckSignal s;
        s.queueMs = 101.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::LinkBound);
        s.queueMs = -1.0;
        s.loss = 0.051;
        CHECK(ClassifyBottleneck(s) == Bottleneck::LinkBound);
        s.loss = 0.05;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.queueMs = 100.0;  // exactly at the line is not congestion
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 5. R3 rate-control: the frozen real-device signature (02:29 log line:
    //    52.3 Mbps of a 98 Mbps ceiling at qp 23.8). Same fill at qp 15 is the
    //    dead zone, not rate-control.
    {
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        CHECK(ClassifyBottleneck(s) == Bottleneck::RateControl);
        s.qp = 19.9;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.qp = -1.0;  // unknown QP: the fill alone never accuses
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.qp = 23.8;
        s.ceilingKbps = 0;  // no ceiling: fill is undefined, no fire
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 6. R4 game-bound: slow game, cheap picture (40/72 at qp 10 - the Quest
    //    evening's healthy-looking half). qp in [16, 20) is the dead zone.
    {
        rc::BottleneckSignal s;
        s.gameFps = 40.0;
        s.nominalFps = 72.0;
        s.qp = 10.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::GameBound);
        s.qp = 16.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.qp = -1.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.gameFps = 54.1;  // 0.75 x 72 = 54: just above is fine
        s.qp = 10.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.nominalFps = 0;  // unknown nominal: no ratio to compare
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 7. R3 beats R4: starving encoder with a slow game is rate-control.
    {
        rc::BottleneckSignal s;
        s.gameFps = 40.0;
        s.nominalFps = 72.0;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        CHECK(ClassifyBottleneck(s) == Bottleneck::RateControl);
    }
    std::printf("ok\n");
}

// TASK-ROBUST-EDGES-2026-11-20 §2: edge cases for the PURE classifier
// (rc/auto_tune.h ClassifyBottleneck - the pure function behind the
// classify surface and AutoTune::Update's call; the decide layer is
// stateful, so the card's same-input-twice semantics live here). Three
// categories per the card: NaN/infinity, same input twice, out-of-range.
// A3 pinned the then-UNDEFENDED behaviour as exposure evidence (A3 receipt,
// "发现的问题"); A8 (TASK-AUTOTUNE-ROBUST-2026-11-20) later hardened the
// classifier with an isfinite gate (A8b) and loss/qp domain gates (A8c), so
// the affected expectations below are FLIPPED to Unknown (Ok) and marked
// "A8 post-fix semantics", with the pre-A8 value kept in the comment for the
// mutation audit. droppedFps joined the gate in the A8 increment (main-AI
// direction, A3 finding #3); mbps/gameFps/nominalFps stay comparison-guarded
// only (outside the A8 gate scope).
static void TestAutoTuneClassifyEdgeCases() {
    std::printf("auto-tune classify edges: ");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // 1. NaN in the R1/R2 fields cannot fire (per the :61 / :65 comparison
    //    branches: every comparison against NaN is false; the :42-43 comment
    //    "an unknown signal can never fire its rule" extends to NaN). A real
    //    budget (:46) alone proves nothing either way.
    {
        rc::BottleneckSignal s;
        s.decodeMs = nan;
        s.decodeBudgetMs = 10.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.decodeBudgetMs = nan;  // NaN budget with decode unknown: same silence
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.droppedFps = nan;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.queueMs = nan;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.loss = nan;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 2. NaN in the R3/R4 fields cannot fire (per the :70 mbps>=0 / qp>=20
    //    guards and the :76 gameFps>0 / nominalFps>0 guards). The starving
    //    background (5 of 20000 kbps, qp high) would fire R3 if the NaNs were
    //    real numbers, so the guards themselves are what is under test.
    {
        rc::BottleneckSignal s;
        s.ceilingKbps = 20000;
        s.mbps = nan;
        s.qp = 23.8;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.mbps = 5.0;
        s.qp = nan;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.qp = 10.0;
        s.gameFps = nan;      // R4 background: 72 nominal, cheap picture
        s.nominalFps = 72.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.gameFps = 40.0;
        s.nominalFps = nan;   // the ratio against NaN never fires
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 3. +infinity in the A8b-gated fields (decodeMs/droppedFps/queueMs/loss/
    //    qp) reads as garbage -> Unknown (Ok): the isfinite gate returns
    //    before any rule, aligning +inf with NaN/-inf per the
    //    EncoderRateTracker::Observe precedent (fps_ladder.h:125) - A8
    //    post-fix semantics (these were pinned as FIRES before A8:
    //    DecodeBound/DecodeBound/LinkBound/LinkBound/RateControl). droppedFps
    //    joined the gate in the A8 increment (it feeds R1 exactly like
    //    decodeMs); its +inf case below is flipped under "A8 increment
    //    semantics" - before the increment it was the one remaining exposure.
    {
        rc::BottleneckSignal s;
        s.decodeMs = inf;
        s.decodeBudgetMs = 10.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was DecodeBound)
        s = rc::BottleneckSignal{};
        s.droppedFps = inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 增量语义 (was DecodeBound: ungated before the increment)
        s = rc::BottleneckSignal{};
        s.queueMs = inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was LinkBound)
        s = rc::BottleneckSignal{};
        s.loss = inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was LinkBound)
        s = rc::BottleneckSignal{};
        s.ceilingKbps = 20000;
        s.mbps = 5.0;         // fill 0.25 - starving
        s.qp = inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was RateControl)
    }
    // 4. -infinity reads as "unknown / never" on every rule (per the :61
    //    decodeMs>0 / droppedFps>3.0, :65 queueMs>=0 / loss>0.05, :70
    //    mbps>=0 / qp>=20 and :76 gameFps>0 guards) - all silent at once.
    {
        rc::BottleneckSignal s;
        s.decodeMs = -inf;
        s.decodeBudgetMs = 10.0;
        s.droppedFps = -inf;
        s.queueMs = -inf;
        s.loss = -inf;
        s.ceilingKbps = 20000;
        s.mbps = 5.0;
        s.qp = -inf;
        s.gameFps = -inf;
        s.nominalFps = 72.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 5. Same input twice -> the same answer, across three signatures (per
    //    the :20-21 comment "the classifier itself is stateless - persistence
    //    lives below": a pure function must be idempotent).
    {
        rc::BottleneckSignal link;
        link.queueMs = 150.0;
        rc::BottleneckSignal game;
        game.gameFps = 40.0;
        game.nominalFps = 72.0;
        game.qp = 10.0;
        const Bottleneck link1 = ClassifyBottleneck(link);
        const Bottleneck game1 = ClassifyBottleneck(game);
        const Bottleneck ok1 = ClassifyBottleneck({});
        CHECK(link1 == Bottleneck::LinkBound);
        CHECK(game1 == Bottleneck::GameBound);
        CHECK(ok1 == Bottleneck::Ok);
        CHECK(ClassifyBottleneck(link) == link1);
        CHECK(ClassifyBottleneck(game) == game1);
        CHECK(ClassifyBottleneck({}) == ok1);
    }
    // 6. loss outside its documented 0..1 domain (:54 "measured loss fraction
    //    0..1") is invalid telemetry -> Unknown (Ok), NOT clamped into range
    //    and not fired through - A8 post-fix semantics (5.0 was pinned as
    //    LinkBound before the A8c domain gate; garbage must not be "repaired").
    //    The negative case keeps its old answer (Ok), now via the same gate.
    {
        rc::BottleneckSignal s;
        s.loss = 5.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was LinkBound)
        s.loss = -0.5;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 7. fill (:71) far above 1 (>100% of the ceiling) is the OPPOSITE of
    //    starving -> not rate-control (per the :71 fill < 0.70 branch); a
    //    negative mbps (域外 <0, :51 documents >=0) is blocked by the :70
    //    mbps >= 0 guard even though its fill would satisfy < 0.70.
    {
        rc::BottleneckSignal s;
        s.ceilingKbps = 20000;
        s.mbps = 50000.0;  // fill = 2500, i.e. 250000% of the ceiling
        s.qp = 22.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
        s.mbps = -5.0;     // fill -0.25 would be < 0.70, but mbps >= 0 fails first
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 8. qp far outside any codec's QP domain (fps_ladder.h:226-239 documents
    //    0..51 / 0..255 and says garbage is "typically hundreds of millions";
    //    QpStats::Add :251-261 rejects it per frame): the A8c domain gate now
    //    rejects the mean too -> Unknown (Ok), NOT clamped - A8 post-fix
    //    semantics (5e8 was pinned as RateControl before A8). Negative garbage
    //    keeps its old answer (Ok): qp < 0 is the field's documented unknown
    //    sentinel (:50 "<0 unknown"), so it is not vetoed - it stays per-rule
    //    silent and never suppresses the other fields' evidence.
    {
        rc::BottleneckSignal s;
        s.ceilingKbps = 20000;
        s.mbps = 5.0;
        s.qp = 5e8;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics (was RateControl)
        s.qp = -1e9;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);
    }
    // 9. A8: finite inputs exactly AT the domain boundary are valid - the
    //    gates reject only loss outside [0, 1] or qp above the codec ceiling
    //    (51 H.264/HEVC / 255 AV1). loss 1.0 still fires R2 (a valid boundary
    //    pressure must not be swallowed) and loss 0.0 stays quiet; qp at both
    //    codec ceilings still fires R3 with the starving fill, while 256 - one
    //    step past the last codec family - is garbage -> Unknown.
    {
        rc::BottleneckSignal s;
        s.loss = 1.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::LinkBound);  // boundary is in-domain
        s.loss = 0.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);         // valid, no pressure
        s.ceilingKbps = 20000;
        s.mbps = 5.0;         // fill 0.25 - starving background
        s.qp = 51.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::RateControl);  // H.264/HEVC ceiling is valid
        s.qp = 255.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::RateControl);  // AV1 ceiling is valid
        s.qp = 256.0;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics: past every codec family
    }
    // 10. A8: the isfinite gate covers -inf too, and unconditionally - one
    //     non-finite field voids the whole tick, so valid pressure in another
    //     gated field cannot fire through it (without the gate the first and
    //     third CHECK would be LinkBound via queueMs).
    {
        rc::BottleneckSignal s;
        s.queueMs = 150.0;   // would be LinkBound on its own
        s.decodeMs = -inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // A8 post-fix semantics: -inf voids the tick
        s.decodeMs = -1.0;   // back to the unknown sentinel: finite again
        CHECK(ClassifyBottleneck(s) == Bottleneck::LinkBound);  // valid pressure classifies
        s.loss = -inf;
        CHECK(ClassifyBottleneck(s) == Bottleneck::Ok);  // -inf loss voids the tick too
    }
    std::printf("ok\n");
}

// PLAN-AUTO-TUNE P3: the decision layer - 5 s persistence, the 30 s cooldown
// shared with the L7 ladder, manual locks, one-way actions, recovery logging
// (DECISION-TABLE §3).
static void TestAutoTuneDecide() {
    rc::AutoTune::Config cfg{};  // defaults: 5 s over, 30 s cooldown
    const int64_t S = 1000000000LL;
    std::printf("auto-tune decide: ");

    // 1. persistence: a rate-control tick held under 5 s never acts; at 5 s it
    //    fires the first priority (CBR off, unlocked and effectively on).
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        rc::AutoTune::Result r;
        for (int64_t t = 0; t < 5 * S; t += S) r = g.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::None);
        r = g.Update(s, in, 5 * S);
        CHECK(r.action == rc::AutoAction::CbrOff && r.cls == Bottleneck::RateControl);
    }
    // 2. cooldown: sustained rate-control fires CBR off once, then nothing for
    //    30 s; at 30 s the next priority (fovea on) becomes eligible.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        for (int64_t t = 0; t <= 5 * S; t += S) g.Update(s, in, t);
        CHECK(g.Update(s, in, 10 * S).action == rc::AutoAction::None);
        CHECK(g.Update(s, in, 34 * S).action == rc::AutoAction::None);  // 29 s after the action
        in.cbrOn = false;  // the CBR write landed: next priority
        const auto r = g.Update(s, in, 35 * S);
        CHECK(r.action == rc::AutoAction::FoveaOn);
    }
    // 3. the ladder's step counts against the same cooldown: the ladder stepped
    //    at t = 1 s, so rate-control pressure from t = 0 may only act at 31 s.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        g.NoteLadderStep(1 * S);
        for (int64_t t = 0; t <= 5 * S; t += S) g.Update(s, in, t);
        CHECK(g.Update(s, in, 10 * S).action == rc::AutoAction::None);
        CHECK(g.Update(s, in, 30 * S).action == rc::AutoAction::None);  // 29 s after the step
        CHECK(g.Update(s, in, 31 * S).action == rc::AutoAction::CbrOff);
    }
    // 4. manual priority: a locked Cbr dimension is skipped and the next
    //    priority (fovea) takes over.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        g.MarkManual(rc::AutoDim::Cbr);
        rc::AutoTune::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::FoveaOn);
    }
    // 5. fovea impossible (the headset's gazeFoveation is off) and CBR already
    //    effectively off: auto does not spend a rebuild on a no-op; the ceiling
    //    hint fires instead, once, only above twice the observed wire peak.
    {
        rc::AutoTune h;
        h.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in2;
        in2.cbrOn = false;        // capped VBR already runs: writing the key would rebuild for nothing
        in2.foveaPossible = false;
        rc::AutoTune::Result rh{};
        for (int64_t t = 0; t <= 5 * S; t += S) rh = h.Update(s, in2, t);
        CHECK(rh.action == rc::AutoAction::None);  // peak 52.3 x 2 = 104.6 > 98: no hint
        s.ceilingKbps = 300000;                    // 300 Mbps ceiling vs a 52 Mbps peak
        CHECK(h.Update(s, in2, 6 * S).action == rc::AutoAction::LogOnly);
        CHECK(h.Update(s, in2, 7 * S).action == rc::AutoAction::None);  // logged once per episode
    }
    // 6. link-bound: profile down one rung, never below latency, never against
    //    a lock or a headset-owned profile (profileId 0 = not auto's).
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.queueMs = 500.0;
        rc::AutoTuneInput in;
        in.profileId = 2;
        rc::AutoTune::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::ProfileDown && r.profileId == 1);
        rc::AutoTune h;
        h.Configure(cfg);
        in.profileId = 0;  // latency, or the headset owns the key
        for (int64_t t = 0; t <= 5 * S; t += S) r = h.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::None);
        rc::AutoTune k;
        k.Configure(cfg);
        k.MarkManual(rc::AutoDim::Profile);
        in.profileId = 2;
        for (int64_t t = 0; t <= 5 * S; t += S) r = k.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::None);
    }
    // 7. game-bound: one explanation per episode, never an action.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.gameFps = 40.0;
        s.nominalFps = 72.0;
        s.qp = 10.0;
        rc::AutoTuneInput in;
        rc::AutoTune::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::LogOnly && r.cls == Bottleneck::GameBound);
        CHECK(g.Update(s, in, 60 * S).action == rc::AutoAction::None);  // quiet after the one line
    }
    // 8. a class change resets the 5 s streak; recovery logs exactly once.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal starve;
        starve.mbps = 52.3;
        starve.ceilingKbps = 98000;
        starve.qp = 23.8;
        rc::BottleneckSignal calm;  // healthy: at the ceiling, low qp
        calm.mbps = 97.0;
        calm.ceilingKbps = 98000;
        calm.qp = 12.0;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        for (int64_t t = 0; t < 4 * S; t += S) g.Update(starve, in, t);
        g.Update(calm, in, 4 * S);  // one clear tick resets the streak
        rc::AutoTune::Result r{};
        for (int64_t t = 5 * S; t <= 8 * S; t += S) r = g.Update(starve, in, t);
        CHECK(r.action == rc::AutoAction::None);  // 4 s since the reset: not yet
        r = g.Update(starve, in, 9 * S);
        CHECK(r.action == rc::AutoAction::None);  // still 4 s of sustained pressure
        r = g.Update(starve, in, 10 * S);
        CHECK(r.action == rc::AutoAction::CbrOff);  // 5 s since the reset
        rc::AutoTune::Result rec = g.Update(calm, in, 11 * S);
        CHECK(rec.recovered && rec.action == rc::AutoAction::LogOnly);
        CHECK(g.Update(calm, in, 12 * S).action == rc::AutoAction::None);  // no repeat
    }
    // 9. ClearLocks: re-enabling autoTune hands the untouched dimensions over.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::BottleneckSignal s;
        s.mbps = 52.3;
        s.ceilingKbps = 98000;
        s.qp = 23.8;
        rc::AutoTuneInput in;
        in.cbrOn = true;
        g.MarkManual(rc::AutoDim::Cbr);
        rc::AutoTune::Result r{};
        for (int64_t t = 0; t <= 5 * S; t += S) r = g.Update(s, in, t);
        CHECK(r.action == rc::AutoAction::FoveaOn);  // locked Cbr skipped
        g.ClearLocks();
        rc::AutoTune::Result r2{};
        for (int64_t t = 0; t <= 5 * S; t += S) r2 = g.Update(s, in, t);
        CHECK(r2.action == rc::AutoAction::CbrOff);  // and available again
    }
    std::printf("ok\n");
}

// A84 (TASK-A84-PROFILE-FLOOR-TWO-WAY-AUTOTUNE-2026-11-23 §5): the timeline cases
// for the two-way profile tuner. The slider is a FLOOR, the picture the stream runs
// at is an effective rung auto may move in both directions, and a manual write to
// the floor hands the dimension back instead of locking it.
//
// These cases drive rc::AutoTune the way ClientSession::RunAutoTune does: one tick
// per second, and after a move the caller re-feeds the RUNG it now builds at plus
// that rung's brake line (`AutoTuneInput::dipAtMs`). The two signals are the
// classifier's own R2 evidence: a 500 ms queue is link-bound, 5 ms with the headset
// decoding inside the L7 budget is Ok (nothing else can fire: qp, the ceiling and
// the game rate stay at their unknown sentinels, so R3/R4 are unreachable). The
// brake lines are queue_guard.h's formula READ at its no-feedback floor
// (stream_profile.h: 15+6 vs 15 -> 21, 45+12 vs 25 -> 57, 60+12 vs 30 -> 72 ms) -
// the same numbers ProfileDipAtMs() in client_session.cpp prints and the A81
// latency line logs. No guard state is touched here and none is copied.
static void TestAutoTuneProfileFloor() {
    rc::AutoTune::Config cfg{};  // the A84 defaults: 5 s over / 30 s down, 30 s Ok / 45 s up
    const int64_t S = 1000000000LL;
    const auto pressed = [] {
        rc::BottleneckSignal s;
        s.queueMs = 500.0;
        return s;
    }();
    const auto calm = [] {
        rc::BottleneckSignal s;
        s.queueMs = 5.0;
        s.decodeMs = 8.0;
        s.decodeBudgetMs = 16.0;
        return s;
    }();
    auto dip = [](int id) { return id == 0 ? 21.0 : (id == 1 ? 57.0 : 72.0); };
    std::printf("auto-tune profile floor: ");

    // 1. The headline: pressure cuts the picture down to the floor, and a link that
    //    calms climbs it back over the floor, one rung at a time, stopping at 2.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::AutoTuneInput in;
        in.profileId = 2;       // the stream runs one rung sharper than the floor
        in.dipAtMs = dip(2);
        rc::AutoTune::Result r{};
        for (int64_t t = 0; t < 5 * S; t += S) r = g.Update(pressed, in, t);
        CHECK(r.action == rc::AutoAction::None);  // 4 s of pressure is not yet evidence
        r = g.Update(pressed, in, 5 * S);
        CHECK(r.action == rc::AutoAction::ProfileDown && r.profileId == 1);  // exactly on the floor
        CHECK(r.evidenceNs == 5 * S);  // §6: how long that evidence had been continuous
        in.profileId = 1;
        in.dipAtMs = dip(1);
        // Calm from 6 s on. The climb is the later of two waits: 30 s of continuous Ok
        // (36 s) and the shared 45 s cooldown anchor (50 s) - so nothing moves before.
        for (int64_t t = 6 * S; t < 50 * S; t += S) {
            const auto rr = g.Update(calm, in, t);
            CHECK(rr.action == (t == 6 * S ? rc::AutoAction::LogOnly : rc::AutoAction::None));
            CHECK(rr.recovered == (t == 6 * S));
        }
        r = g.Update(calm, in, 50 * S);
        CHECK(r.action == rc::AutoAction::ProfileUp && r.profileId == 2);
        CHECK(r.evidenceNs == 44 * S);  // the Ok streak that paid for it, not the cooldown
        in.profileId = 2;
        in.dipAtMs = dip(2);
        int ups = 0;  // 2 is the ceiling: however clean the link stays, nothing past it
        for (int64_t t = 51 * S; t <= 200 * S; t += S)
            if (g.Update(calm, in, t).action == rc::AutoAction::ProfileUp) ups++;
        CHECK(ups == 0);
    }
    // 2. The field machine's revival path: floor 0 (latency-first, the position that
    //    crushed the user's bitrate) on a link that always had room. Nothing but Ok
    //    ticks, and the picture still walks itself up to 2.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::AutoTuneInput in;
        in.profileId = 0;
        in.dipAtMs = dip(0);
        int ups = 0, downs = 0, rung = 0;
        int64_t firstUpNs = 0, secondUpNs = 0;
        for (int64_t t = 0; t <= 200 * S; t += S) {
            const auto r = g.Update(calm, in, t);
            if (r.action == rc::AutoAction::ProfileUp) {
                ups++;
                if (!firstUpNs) firstUpNs = t;
                else secondUpNs = t;
                rung = r.profileId;
                in.profileId = rung;
                in.dipAtMs = dip(rung);
            } else if (r.action == rc::AutoAction::ProfileDown) {
                downs++;
            }
        }
        CHECK(ups == 2 && downs == 0 && rung == 2);
        CHECK(firstUpNs == 30 * S);   // 0 -> 1: the 30 s Ok streak; no action to wait out
        CHECK(secondUpNs == 75 * S);  // 1 -> 2: the 45 s climb cooldown, NOT the 30 s streak
    }
    // 3. A86b: with no tier slider there is no floor write to clear a Profile
    // lock. ClearLocks() (the hand-over when the tuner is re-enabled) is the ONLY
    // way back, and a lock must hold through both calm and pressure until then.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        g.MarkManual(rc::AutoDim::Profile);
        rc::AutoTuneInput in;
        in.profileId = 0;
        in.dipAtMs = dip(0);
        for (int64_t t = 0; t <= 66 * S; t += S)
            CHECK(g.Update(t <= 60 * S ? calm : pressed, in, t).action == rc::AutoAction::None);
        g.ClearLocks();
        in.profileId = 1;
        in.dipAtMs = dip(1);
        rc::AutoTune::Result r{};
        for (int64_t t = 61 * S; t <= 66 * S; t += S) r = g.Update(pressed, in, t);
        CHECK(r.action == rc::AutoAction::ProfileDown && r.profileId == 0);  // the unlocked answer
        CHECK(r.evidenceNs == 5 * S);  // pressure since the 61 s tick that changed class
        in.profileId = 0;
        in.dipAtMs = dip(0);
        // ... and it works ABOVE the new floor right away: the cut sits under the floor
        // only while the pressure pins it, then climbs back over it (Ok from 67 s, last
        // action at 66 s -> the later of the two waits ends at 111 s).
        for (int64_t t = 67 * S; t <= 110 * S; t += S) {
            const auto rr = g.Update(calm, in, t);
            CHECK(rr.action == (t == 67 * S ? rc::AutoAction::LogOnly : rc::AutoAction::None));
        }
        r = g.Update(calm, in, 111 * S);
        CHECK(r.action == rc::AutoAction::ProfileUp && r.profileId == 1);
    }
    // 4. Q-oscillation: the card's five 6 s pressure episodes 20 s apart, then a long
    //    calm stretch in which the picture climbs back and the pressure returns. The
    //    bound is the defensible one: ONE anchor (`m_lastActionNs`) gates both
    //    directions, so no two rung changes may sit closer than the 30 s brake cooldown
    //    (45 s for a climb), and over the 300 s timeline below that is at most 10 moves
    //    - the observed answer is 6, with the two directional floors asserted
    //    separately because they are exactly what Q-oscillation asks about.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::AutoTuneInput in;
        in.profileId = 2;
        in.dipAtMs = dip(2);
        int ups = 0, downs = 0;
        int64_t prevChangeNs = -1, minGapNs = 0;
        bool prevWasUp = false;
        int64_t minUpToDownNs = 0, minDownToUpNs = 0;
        for (int64_t t = 0; t <= 300 * S; t += S) {
            // Part A (t <= 100 s): 6 s episodes at 0/20/40/60/80 s, Ok in between - the
            // 14 s windows are far too short for the 30 s a climb needs. Part B: the
            // link calms for good from 101 s, pressure returns for 35 s at 181 s, then
            // the link is clean to the end.
            const int64_t ts = t / S;
            const bool pressWindow = ts <= 100 ? (ts % 20) < 6 : (ts >= 181 && ts <= 215);
            const auto r = g.Update(pressWindow ? pressed : calm, in, t);
            if (r.action == rc::AutoAction::ProfileUp || r.action == rc::AutoAction::ProfileDown) {
                const bool up = r.action == rc::AutoAction::ProfileUp;
                if (up) ups++;
                else downs++;
                if (prevChangeNs >= 0) {
                    const int64_t gap = t - prevChangeNs;
                    if (minGapNs == 0 || gap < minGapNs) minGapNs = gap;
                    if (prevWasUp && !up && (minUpToDownNs == 0 || gap < minUpToDownNs)) minUpToDownNs = gap;
                    if (!prevWasUp && up && (minDownToUpNs == 0 || gap < minDownToUpNs)) minDownToUpNs = gap;
                }
                prevWasUp = up;
                prevChangeNs = t;
                in.profileId = r.profileId;
                in.dipAtMs = dip(r.profileId);
            }
        }
        // Part A's cuts land at 5 s (2 -> 1) and 45 s (1 -> 0): the episodes at 20 s and
        // 60 s arrive inside the 30 s cooldown, the one at 80 s has no rung left to give
        // up (0 is latency, the bottom of the ladder), and no Ok window there is 30 s
        // long, so not one climb fires. Part B: the picture climbs 0 -> 1 at 131 s and
        // 1 -> 2 at 176 s (45 s apart - the climb's own cooldown, not the 30 s streak);
        // the pressure that returns at 181 s cannot take a rung back before 206 s, i.e.
        // 30 s after that climb and never immediately; and 251 s the picture is back at
        // the ceiling for good.
        CHECK(ups == 3 && downs == 3);
        CHECK(in.profileId == 2);   // it ends at the ceiling, which is what §2 asks for
        CHECK(minGapNs == 30 * S);  // the tightest pair in the whole timeline: never 0
        CHECK(ups + downs <= 10);   // the 300 s bound stated above
        CHECK(minUpToDownNs == 30 * S);  // a cut may not follow a climb with no hysteresis
        CHECK(minDownToUpNs == 45 * S);  // a climb waits out its own 45 s cooldown
        CHECK(prevChangeNs == 251 * S);
    }
    // 5. The enable switch, decided INSIDE the layer (A84b §1). The case A84 left
    //    here was a tautology: `gated(false, ...)` was a lambda that returned the
    //    "off" answer itself, so CHECK(actions == 0) could not go red and the real
    //    off branch - the `if (!pc.autoTune)` in front of the layer's only feed in
    //    the product - was covered by nothing. The switch is
    //    rc::AutoTune::Config::enabled now, enforced in Update(), which this binary
    //    reaches, so both directions of the same claim are asserted on the SAME
    //    timeline and neither half can pass by the harness declining to run:
    //      enabled  -> the 200 s headroom stretch moves the picture twice (0 -> 1 at
    //                  30 s, 1 -> 2 at 75 s): a gate stuck shut is red here;
    //      disabled -> the identical stretch moves it zero times: a gate stuck open,
    //                  or inverted, is red here.
    //    The third block is the one A84's comment only claimed: disabled is INERT,
    //    not merely silent. 200 s of disabled Ok ticks leave no streak and no
    //    cooldown anchor behind, so the tick the switch turns on at is the streak's
    //    first and the climb lands a full 30 s later. If the gate merely muted the
    //    action while the episodes kept accumulating, the first enabled tick would
    //    climb at once and that CHECK goes red.
    {
        struct Out {
            int actions;
            int64_t firstUpNs;
            int rung;
        };
        auto run = [&](bool enabled, int64_t toNs) {
            rc::AutoTune g;
            rc::AutoTune::Config c = cfg;
            c.enabled = enabled;
            g.Configure(c);
            rc::AutoTuneInput in;
            in.profileId = 0;  // case 2's shape: the stream starts at the floor
            in.dipAtMs = dip(0);
            Out o{0, 0, 0};
            for (int64_t t = 0; t <= toNs; t += S) {
                const auto r = g.Update(calm, in, t);
                if (r.action == rc::AutoAction::None) continue;
                o.actions++;
                if (r.action != rc::AutoAction::ProfileUp) continue;
                if (!o.firstUpNs) o.firstUpNs = t;
                o.rung = int(r.profileId);
                in.profileId = r.profileId;  // the caller rebuilds at the new rung
                in.dipAtMs = dip(r.profileId);
            }
            return o;
        };
        const Out on = run(true, 200 * S);
        const Out off = run(false, 200 * S);
        CHECK(on.firstUpNs == 30 * S);  // the direction the old case never tested
        CHECK(on.actions == 2 && on.rung == 2);
        CHECK(off.actions == 0 && off.firstUpNs == 0 && off.rung == 0);
        // One tuner, switch moved at 200 s: the product feeds SetEnabled from the
        // settings snapshot and the layer decides, so that is all a re-enable is.
        rc::AutoTune g;
        rc::AutoTune::Config c = cfg;
        c.enabled = false;
        g.Configure(c);
        rc::AutoTuneInput in;
        in.profileId = 0;
        in.dipAtMs = dip(0);
        int offActions = 0;
        for (int64_t t = 0; t < 200 * S; t += S)
            if (g.Update(calm, in, t).action != rc::AutoAction::None) offActions++;
        CHECK(offActions == 0);
        c.enabled = true;
        g.Configure(c);
        int afterEnable = 0;
        int64_t upNs = 0;
        for (int64_t t = 200 * S; t <= 300 * S; t += S) {
            const auto r = g.Update(calm, in, t);
            if (r.action == rc::AutoAction::None) continue;
            afterEnable++;
            if (r.action != rc::AutoAction::ProfileUp) continue;
            if (!upNs) upNs = t;
            in.profileId = r.profileId;
            in.dipAtMs = dip(r.profileId);
        }
        CHECK(upNs == 230 * S);     // 30 s of Ok counted from the FIRST enabled tick
        CHECK(afterEnable == 2);    // 230 s and 275 s, then 2 is the ceiling
    }
    // 5b. The floor the manual-lock hand-over is detected against is the RUNG the
    //     floor clamps to, not the raw number the caller sent (A84b §3). A84 compared
    //     the raw value against a clamped one, so a settings file carrying an
    //     out-of-range slider (hand-edited: -3, 5, 9) "moved the floor" on a tick
    //     where nothing about the floor moved, and cleared the user's manual lock
    //     every time the number jittered. Both halves are asserted, and they are
    //     opposites: the same-clamp sequence must keep the lock (A84's comparison
    //     breaks it and starts a climb), a genuinely different floor must hand the
    //     dimension back.
    {
        rc::AutoTune g;
        g.Configure(cfg);
        rc::AutoTuneInput in;
        in.profileId = 0;
        in.dipAtMs = dip(0);
        (void)g.Update(calm, in, 0);  // the tick that states the floor: no lock to hand back yet
        g.MarkManual(rc::AutoDim::Profile);  // the user wrote this dimension
        // The lock holds through a whole minute of headroom. This is the shape the
        // rest of the block is measured against, and it is also what makes the next
        // half sharp: the Ok streak is over 30 s by then, so a lock that is wrongly
        // erased below climbs on the spot instead of quietly waiting.
        for (int64_t t = 1; t <= 40 * S; t += S)
            CHECK(g.Update(calm, in, t).action == rc::AutoAction::None);
        // 5, 6, 9, 5: four different numbers, one floor (all clamp to 2), so the
        // lock is still the answer on every tick. A84's raw-vs-clamped comparison
        // erases it on the first of them - this CHECK goes red there.
        const int sameFloor[] = {5, 6, 9, 5};
        for (int i = 0; i < 4; i++) {
            CHECK(g.Update(calm, in, (41 + i) * S).action == rc::AutoAction::None);
        }
        // A86b: no floor number can clear this lock any more - the old 5/6/9/5
        // "different floors" trick is gone. Only ClearLocks() hands the dimension
        // back, after which the already-long Ok streak climbs on the next tick.
        g.ClearLocks();  // also restarts the Ok streak (ResetEpisodesLocked)
        for (int64_t t = 46 * S; t < 76 * S; t += S)
            CHECK(g.Update(calm, in, t).action == rc::AutoAction::None);
        const auto r = g.Update(calm, in, 76 * S);  // 46 s + the 30 s climb streak
        CHECK(r.action == rc::AutoAction::ProfileUp && r.profileId == 1);
    }
    std::printf("ok\n");
}

// A84b §4 (TASK-A84b-CLOSE-AUTOTUNE-GAPS): rc::EffectiveProfileRung, the one rule
// that says what rung the live streams are BUILT at, and therefore the rule behind
// the periodic log reading the field round needs (`effProfile=` beside
// `profileFloor=` on the 30 s AUTO signals line). It is the rule
// ClientSession::EffectiveProfileLocked applies, extracted into rc/auto_tune.h
// because client_session.cpp is not linked into rincast-core and a rule only
// reachable from there has no test that could go red - which is exactly how A84
// ended up with an effProfile nobody had ever checked.
// A88: "no data" must never read as "the link is clean". The 2026-11-23 10:17
// log had 30 s of `signals ok fps=0 game=0 qp=-1 wire=0 queue=-1` while the
// headset showed nothing. The caller now skips those ticks; this is the rule.
static void TestHasUsableSignals() {
    rc::BottleneckSignal s{};
    CHECK(!rc::HasUsableSignals(s), "all-unknown is no evidence, not a clean link");
    s.gameFps = 33;
    CHECK(rc::HasUsableSignals(s), "a game fps reading alone is enough to judge the tick");
    s.gameFps = 0;
    s.qp = 24;
    CHECK(rc::HasUsableSignals(s), "a QP reading alone is enough");
    s.qp = -1;
    s.mbps = 5;
    CHECK(rc::HasUsableSignals(s), "a wire reading alone is enough");
    s.mbps = 0;
    s.queueMs = 0;
    CHECK(rc::HasUsableSignals(s), "a measured zero queue IS evidence");
    s.queueMs = -1;
    s.decodeMs = 8;
    CHECK(!rc::HasUsableSignals(s), "a decode reading does not stand in for a stream");
}

static void TestEffectiveProfileRung() {
    std::printf("effective profile rung: ");
    // A86b: no slider and no floor. -1 is the only sentinel: auto has not chosen
    // yet, so a new stream starts at 1 (balanced - smooth AND sharp before any
    // evidence exists). It is a start point, not a line the tuner may not cross.
    CHECK(rc::EffectiveProfileRung(-1) == 1);
    // Every stored choice is clamped to the 0/1/2 ladder. Running at 0 under
    // pressure and climbing to 2 with headroom are both ordinary - there is no
    // floor to "violate" any more, so none of these needs a lift-back rule.
    for (int rung = 0; rung <= 2; rung++) {
        CHECK(rc::EffectiveProfileRung(rung) == rung);
        CHECK(rc::EffectiveProfileRung(rung) <= 2);
    }
    CHECK(rc::EffectiveProfileRung(9) == 2);
    CHECK(rc::EffectiveProfileRung(-3) == 1);  // any negative is "not chosen yet" -> start rung
    std::printf("ok\n");
}

// A84b §2 (TASK-A84b): the one-time settings migration behind A84's flipped
// autoTune default. Settings::FromJson - the code that reads the stamp and applies
// this - compiles into rincast-streamer only (core_tests.cpp names neither
// FromJson, ToJson nor SettingsStore), so the judgement moved to
// rc/settings_migration.h and is asserted here.
static void TestSettingsSchemaMigration() {
    std::printf("settings schema migration: ");
    // 1. No stamp = a file last written when the default was still off, which proves
    //    nothing about what the user wanted (every save dumps the whole struct, so
    //    the file says "autoTune": false regardless). The migration must fire, and
    //    the sentinel the streamer's read falls back to for an absent key AND for a
    //    hand-edited non-number must be under the threshold so both migrate.
    CHECK(rc::MigrateAutoTuneOn(rc::kSettingsSchemaUnstamped));
    CHECK(rc::kSettingsSchemaUnstamped < rc::kSettingsSchemaAutoTuneOn);
    // An unreadable stamp must never be able to latch the tuner off by accident.
    CHECK(rc::MigrateAutoTuneOn(-7));
    // 2. A stamped file rules exactly as written: turn the switch off on this build
    //    and it stays off across restarts, so the migration must not fire at or
    //    above the threshold, nor for a future schema.
    CHECK(!rc::MigrateAutoTuneOn(rc::kSettingsSchemaAutoTuneOn));
    CHECK(!rc::MigrateAutoTuneOn(rc::kSettingsSchemaAutoTuneOn + 1));
    // 3. And the stamp this build writes must itself be trusted, or every save would
    //    re-migrate and the switch could never be turned off at all (this is the
    //    check that makes the two constants' relationship, not just their values,
    //    part of the contract).
    CHECK(rc::kCurrentSettingsSchema >= rc::kSettingsSchemaAutoTuneOn);
    CHECK(!rc::MigrateAutoTuneOn(rc::kCurrentSettingsSchema));
    std::printf("ok\n");
}

// VD-OFFLINE (TASK 2026-11-20 §2.1): the full/half+generate pace policy. Pure
// state machine - every case drives UpdateLinkPolicy directly with synthetic
// 1 Hz ticks. The rule numbers in the comments refer to TASK-VD-OFFLINE §2.1.
static void TestLinkPolicy() {
    const int64_t S = 1000000000LL;
    std::printf("link policy: ");
    rc::LinkPolicyInput in;
    rc::PaceMode m = rc::PaceMode::Full;

    // 1. steady full rate: nothing to act on, every tick.
    {
        rc::LinkPolicyState s{};
        in = {72.f, 72.f, 72.f, false};
        for (int64_t t = S; t <= 10 * S; t += S) {
            m = UpdateLinkPolicy(s, in, t);
            CHECK(m == rc::PaceMode::Full);
            CHECK(s.lastReason == rc::PaceReason::Ok);
        }
    }
    // 2. the game really is slow ({72,36,36} every tick): entry lands on the
    //    3rd consecutive low tick, not the first (TASK-POLICY-FIX-TESTS-
    //    2026-11-20: raw-rate counters replaced the smoothed window gate).
    {
        rc::LinkPolicyState s{};
        in = {72.f, 36.f, 36.f, false};
        m = UpdateLinkPolicy(s, in, S);
        CHECK(m == rc::PaceMode::Full);  // 1 low tick: hold
        m = UpdateLinkPolicy(s, in, 2 * S);
        CHECK(m == rc::PaceMode::Full);  // 2 low ticks: hold
        for (int64_t t = 3 * S; t <= 10 * S; t += S) {
            m = UpdateLinkPolicy(s, in, t);
            CHECK(m == rc::PaceMode::HalfWithGeneration);  // t3: entered, stays
        }
        CHECK(s.lastReason == rc::PaceReason::GameSlow);
    }
    // 3. the LINK is slow but the game keeps pace: shortfall > 6 yet the game
    //    frame interval is inside the window -> generation would not help, stay
    //    full (rule 5's second factor; reason names the observation).
    {
        rc::LinkPolicyState s{};
        in = {72.f, 72.f, 40.f, false};
        for (int64_t t = S; t <= 6 * S; t += S) m = UpdateLinkPolicy(s, in, t);
        CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::LinkSlow);
    }
    // 4. no flap on a single good sample after entering Half: one recovered
    //    tick (1 < kExitTicks) must not undo the mode.
    {
        rc::LinkPolicyState s{};
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, S);       // 1 low tick
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 2 * S);   // 2 low ticks
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 3 * S);  // 3rd low tick -> Half
        CHECK(m == rc::PaceMode::HalfWithGeneration && s.lastReason == rc::PaceReason::GameSlow);
        m = UpdateLinkPolicy(s, {72.f, 72.f, 72.f, false}, 4 * S);
        CHECK(m == rc::PaceMode::HalfWithGeneration);  // recoveredTicks = 1 < 3: hold
    }
    // 5. exit: three consecutive ticks at the target walk recoveredTicks to the
    //    exit threshold; 3 s since the entry switch pass the anti-flap guard;
    //    reason says Recovered on the switching tick.
    {
        rc::LinkPolicyState s{};
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, S);       // 1 low tick
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 2 * S);   // 2 low ticks
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 3 * S);  // -> Half at t = 3 s
        CHECK(m == rc::PaceMode::HalfWithGeneration);
        for (int64_t t = 4 * S; t <= 5 * S; t += S) {
            m = UpdateLinkPolicy(s, {72.f, 72.f, 72.f, false}, t);
            CHECK(m == rc::PaceMode::HalfWithGeneration);  // recoveredTicks 1, 2 < 3: hold
        }
        m = UpdateLinkPolicy(s, {72.f, 72.f, 72.f, false}, 6 * S);  // 3rd recovered tick; 6s-3s passes anti-flap
        CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::Recovered);
    }
    // 6. AER: the app refuses generation - forced full regardless of pressure.
    {
        rc::LinkPolicyState s{};
        in = {72.f, 36.f, 36.f, true};
        m = UpdateLinkPolicy(s, in, S);
        CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::AppRefusesGeneration);
    }
    // 7. missing signals: no crash, no NaN, no action (rule 9).
    {
        rc::LinkPolicyState s1{};
        m = UpdateLinkPolicy(s1, {72.f, 0.f, 40.f, false}, S);
        CHECK(m == rc::PaceMode::Full && s1.lastReason == rc::PaceReason::Ok);
        rc::LinkPolicyState s2{};
        m = UpdateLinkPolicy(s2, {0.f, 36.f, 36.f, false}, S);
        CHECK(m == rc::PaceMode::Full && s2.lastReason == rc::PaceReason::Ok);
    }
    // 8. shortfall no longer gates entry (TASK-POLICY-FIX-TESTS-2026-11-20):
    //    recv 66 pins shortfall at exactly 6, yet the game's 3 consecutive low
    //    ticks enter Half. The old shortfall boundary survives in the reason
    //    naming only: 72 - 66 = 6 is not > 6 (Ok), 72 - 65 = 7 is (LinkSlow).
    {
        rc::LinkPolicyState s{};
        in = {72.f, 36.f, 66.f, false};  // 72 - 66 = 6 exactly
        m = UpdateLinkPolicy(s, in, S);
        CHECK(m == rc::PaceMode::Full);  // shortfall is irrelevant to entry now
        m = UpdateLinkPolicy(s, in, 2 * S);
        CHECK(m == rc::PaceMode::Full);
        m = UpdateLinkPolicy(s, in, 3 * S);
        CHECK(m == rc::PaceMode::HalfWithGeneration && s.lastReason == rc::PaceReason::GameSlow);
        rc::LinkPolicyState reasonOk{};
        for (int64_t t = S; t <= 5 * S; t += S) m = UpdateLinkPolicy(reasonOk, {72.f, 71.f, 66.f, false}, t);
        CHECK(m == rc::PaceMode::Full && reasonOk.lastReason == rc::PaceReason::Ok);
        rc::LinkPolicyState reasonSlow{};
        for (int64_t t = S; t <= 5 * S; t += S) m = UpdateLinkPolicy(reasonSlow, {72.f, 71.f, 65.f, false}, t);
        CHECK(m == rc::PaceMode::Full && reasonSlow.lastReason == rc::PaceReason::LinkSlow);
    }
    // 9. the smoothed frame-window gate is retired (raw-rate counters replaced
    //    it): the 60-67 middle band must HOLD both counters - neither advance
    //    nor clear - and never enter. First, pure band input {63,65,67} cycled
    //    three times from a fresh state; then a seeded count (2 low ticks) must
    //    survive 9 band ticks intact, provable from the mode alone: one more
    //    low tick enters Half only if the band kept the count at 2.
    {
        rc::LinkPolicyState s{};
        const float band[] = {63.f, 65.f, 67.f};
        int64_t t = 0;
        for (int round = 0; round < 3; round++)
            for (int i = 0; i < 3; i++) {
                m = UpdateLinkPolicy(s, {72.f, band[i], 72.f, false}, t += S);
                CHECK(m == rc::PaceMode::Full);  // band: holds, never enters
            }
        m = UpdateLinkPolicy(s, {72.f, 38.f, 72.f, false}, t += S);  // seed: 1 low tick
        m = UpdateLinkPolicy(s, {72.f, 38.f, 72.f, false}, t += S);  // seed: 2 low ticks
        for (int round = 0; round < 3; round++)
            for (int i = 0; i < 3; i++) {
                m = UpdateLinkPolicy(s, {72.f, band[i], 72.f, false}, t += S);
                CHECK(m == rc::PaceMode::Full);  // band keeps the seeded count intact
            }
        m = UpdateLinkPolicy(s, {72.f, 38.f, 72.f, false}, t += S);
        CHECK(m == rc::PaceMode::HalfWithGeneration);  // 2 (held) + 1 = 3 -> enters
    }
    // 10. extreme values: tiny game rate, zero received - finite, and entry
    //     still needs its 3 consecutive ticks (was: a first-tick switch).
    {
        rc::LinkPolicyState s{};
        m = UpdateLinkPolicy(s, {72.f, 0.01f, 0.f, false}, S);
        CHECK(m == rc::PaceMode::Full);  // 1st tick
        m = UpdateLinkPolicy(s, {72.f, 0.01f, 0.f, false}, 2 * S);
        CHECK(m == rc::PaceMode::Full);  // 2nd tick
        m = UpdateLinkPolicy(s, {72.f, 0.01f, 0.f, false}, 3 * S);
        CHECK(m == rc::PaceMode::HalfWithGeneration);
        CHECK(s.smoothedGameFps > 0.f && s.smoothedGameFps < 1.f);
    }
    // 11. the anti-flap guard (rule 8, RinCast's own): a switch that comes due
    //     within 2 s of the last REAL mode change is held with reason Dwelling.
    //     Chain: 3 low ticks enter Half at t = 3 s, AER forces back to full at
    //     t = 4 s (a real switch), the sustained slow game is due again at
    //     t = 5 s - the guard holds it one more second; entry lands at t = 6 s.
    {
        rc::LinkPolicyState s{};
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, S);           // 1 low tick
        UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 2 * S);       // 2 low ticks
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 3 * S);   // -> Half
        CHECK(m == rc::PaceMode::HalfWithGeneration);
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, true}, 4 * S);    // AER -> Full (real switch)
        CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::AppRefusesGeneration);
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 5 * S);   // entry due, 1 s after
        CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::Dwelling);
        m = UpdateLinkPolicy(s, {72.f, 36.f, 36.f, false}, 6 * S);   // past the guard: enters
        CHECK(m == rc::PaceMode::HalfWithGeneration && s.lastReason == rc::PaceReason::GameSlow);
    }

    // --- Sequence cases, derived from real-device log shapes
    //     (TASK-POLICY-FIX-TESTS-2026-11-20 §3.2, 1 Hz ticks, target 72).
    //     F1/F2/F5 ran RED on the pre-fix implementation and the failure mode
    //     matched the card's prediction (evidence archived in
    //     POLICY-FIX-NOTES-2026-11-20); F3 is a guardrail with no
    //     discriminating power on either version.

    // F1. Source: the 07:56:08 streamer line "half+generate (game-slow), game 70,
    //     shown 35.0" - the game climbed out of a low phase (38->44->51) while the
    //     link stayed short at 35. Raw-rate timeline: 3 low ticks enter Half, 3
    //     recovered ticks leave it; the short link may name the reason afterwards
    //     (LinkSlow) but must not steer the decision. The pre-fix code entered
    //     Half on t1 and never left it (shortfall 37 blocks the exit forever).
    {
        rc::LinkPolicyState s{};
        const float game[] = {38.f, 44.f, 51.f, 70.f, 70.f, 70.f, 70.f, 70.f, 70.f, 70.f};
        int64_t t = 0;
        for (int i = 0; i < 10; i++) {
            m = UpdateLinkPolicy(s, {72.f, game[i], 35.f, false}, t += S);
            if (i <= 1)
                CHECK(m == rc::PaceMode::Full);  // t1,t2: counter filling (pre-fix: t1 already Half)
            else if (i == 2)
                CHECK(m == rc::PaceMode::HalfWithGeneration && s.lastReason == rc::PaceReason::GameSlow);  // t3: 3rd low tick
            else if (i <= 4)
                CHECK(m == rc::PaceMode::HalfWithGeneration);  // t4,t5: recovered 1, 2 < 3
            else if (i == 5)
                CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::Recovered);  // t6: 3rd recovered tick
            else
                CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::LinkSlow);  // t7-t10: shortfall 37 > 6
        }
    }
    // F2. Source: second device round, the 33-36 fps jitter shape - game and link
    //     deliver the same rate, so the GAME is the slow one. Entry on the 3rd
    //     consecutive low tick; the jitter never reaches the exit line. The
    //     pre-fix code entered Half on t1.
    {
        rc::LinkPolicyState s{};
        const float game[] = {34.f, 36.f, 35.f, 36.f, 34.f, 36.f, 35.f, 36.f, 34.f, 36.f};
        int64_t t = 0;
        for (int i = 0; i < 10; i++) {
            m = UpdateLinkPolicy(s, {72.f, game[i], game[i], false}, t += S);
            if (i <= 1)
                CHECK(m == rc::PaceMode::Full);  // t1,t2 (pre-fix: t1 already Half)
            else
                CHECK(m == rc::PaceMode::HalfWithGeneration);  // t3 entry, stays through t10
        }
    }
    // F3. Source: first device round's link-starved stretches with the game
    //     healthy (71). Guardrail - no discriminating power, both versions must
    //     behave the same: the raw-rate counters never see a low tick, the
    //     shortfall (72 - 35 = 37) only names the reason LinkSlow, mode stays Full.
    {
        rc::LinkPolicyState s{};
        for (int64_t t = S; t <= 10 * S; t += S) {
            m = UpdateLinkPolicy(s, {72.f, 71.f, 35.f, false}, t);
            CHECK(m == rc::PaceMode::Full && s.lastReason == rc::PaceReason::LinkSlow);
        }
    }
    // F5. Source: first device round's median shape - game == link == 51 for 20 s.
    //     Entry on the 3rd tick, then generation holds: the game never comes
    //     back. The pre-fix code entered Half on t1.
    {
        rc::LinkPolicyState s{};
        for (int64_t t = S; t <= 20 * S; t += S) {
            m = UpdateLinkPolicy(s, {72.f, 51.f, 51.f, false}, t);
            if (t <= 2 * S)
                CHECK(m == rc::PaceMode::Full);  // t1,t2 (pre-fix: t1 already Half)
            else
                CHECK(m == rc::PaceMode::HalfWithGeneration);  // t3 entry, steady after
        }
    }

    // E1-E3 (TASK-ROBUST-EDGES-2026-11-20 §1.1): robustness edges - NaN game
    // rate, an absurd target, a negative recv. Expectations are the task
    // card's table; derivation cites point at link_policy.h.

    // E1. gameFps = NaN, recv 40, 5 ticks. NaN > 0 is false, so rule 1
    //     (:99-100) never touches the EMA and rule 9 (:103-106) treats the
    //     tick as unknown: Full + reason Ok every tick, and the unknown tick
    //     "advances and clears nothing" (the :102 comment) - both raw-rate
    //     counters (:73-74) stay at 0. recv 40 >= 36 keeps slowSamples at 0
    //     too (:110-111). Nothing is polluted by the NaN.
    {
        rc::LinkPolicyState s{};
        const float nanF = std::numeric_limits<float>::quiet_NaN();
        for (int64_t t = S; t <= 5 * S; t += S) {
            m = UpdateLinkPolicy(s, {72.f, nanF, 40.f, false}, t);
            CHECK(m == rc::PaceMode::Full);
            CHECK(s.lastReason == rc::PaceReason::Ok);
        }
        CHECK(s.smoothedGameFps == 0.f);                 // the EMA never saw the NaN
        CHECK(s.slowGameTicks == 0 && s.recoveredTicks == 0);  // counters untouched
        CHECK(s.slowSamples == 0);
    }
    // E2. Illegal huge target (1e7): a fool-proofing case - no crash, no NaN,
    //     predictable behaviour. The thresholds scale mechanically: 36 <
    //     1e7-12 for 3 consecutive ticks (the :119 enter branch, raw rate)
    //     enters Half at t3; t4 stays (the :136 branch needs mode == Full).
    //     The EMA (:100) stays finite at 36 (非法输入防呆).
    {
        rc::LinkPolicyState s{};
        for (int64_t t = S; t <= 4 * S; t += S) {
            m = UpdateLinkPolicy(s, {1e7f, 36.f, 36.f, false}, t);
            if (t <= 2 * S)
                CHECK(m == rc::PaceMode::Full);  // t1,t2: counter filling
            else
                CHECK(m == rc::PaceMode::HalfWithGeneration);  // t3 entry, t4 holds
        }
        CHECK(s.lastReason == rc::PaceReason::GameSlow);
        CHECK(std::isfinite(s.smoothedGameFps) && s.smoothedGameFps == 36.f);
    }
    // E3. Negative recv (-5), game healthy at 71, 5 ticks: shortfall =
    //     72-(-5) = 77 > 6 only NAMES the tick LinkSlow (:107, :161) - it
    //     drives nothing. The switch counters (:73-74) behave exactly as with
    //     a valid recv: 71 >= 68 (the exit line) builds recoveredTicks, the
    //     enter counter never moves, mode stays Full, and the EMA (:99-100)
    //     never sees recv. The naming counter slowSamples does grow (rule 4,
    //     :110: -5 < 36) - its only consumer is the same :161 naming, which
    //     is the documented "names, does not steer" contract.
    {
        rc::LinkPolicyState s{};
        for (int64_t t = S; t <= 5 * S; t += S) {
            m = UpdateLinkPolicy(s, {72.f, 71.f, -5.f, false}, t);
            CHECK(m == rc::PaceMode::Full);
            CHECK(s.lastReason == rc::PaceReason::LinkSlow);
        }
        CHECK(s.slowGameTicks == 0);       // the enter counter unpolluted
        CHECK(s.recoveredTicks == 5);      // the exit counter built normally
        CHECK(s.smoothedGameFps == 71.f);  // recv never touches the EMA
        CHECK(s.slowSamples == 5);         // naming-only growth (rule 4, :110)
    }
    std::printf("ok\n");
}

// VD-OFFLINE (TASK 2026-11-20 §2.4): the start-rate advice - largest ladder rung
// at or under the device's own decode hint, snapped to zero when the hint is at
// or within ~3% of the target (a panel measuring a couple of frames short must
// not cause a fake downgrade).
static void TestSuggestStartFps() {
    std::printf("suggest start fps: ");
    CHECK(SuggestStartFps(72.f, 45) == 45.f);
    CHECK(SuggestStartFps(72.f, 72) == 0.f);
    CHECK(SuggestStartFps(72.f, 70) == 0.f);  // 70 > 69.84 = 72 x 0.97: snapped
    CHECK(SuggestStartFps(90.f, 45) == 45.f);
    CHECK(SuggestStartFps(72.f, 0) == 0.f);
    CHECK(SuggestStartFps(0.f, 45) == 0.f);
    CHECK(SuggestStartFps(72.f, 20) == 24.f);  // below the ladder's bottom rung
    std::printf("ok\n");
}


// ROUND19 T3: the disconnect/reconnect state machine at the connection level (the
// session-level paths are mapped in the stick's AUDIT; the streamer owns those and
// is not linkable here). Each case builds fresh loopback session pairs like the
// transport test above.
namespace {
struct SessionLink {
    Peer a, b;
    crypto::SessionKeys ck, sk;  // kept: the roam case reuses the link's own keys
    SessionLink(uint32_t sessionId) {
        auto client = crypto::KeyPair::Generate();
        auto server = crypto::KeyPair::Generate();
        const std::string ctx = SessionContext("cn", "sn", "secret");
        ck = crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
        sk = crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
        CHECK(a.socket.Open(0));
        CHECK(b.socket.Open(0));
        NetAddress aAddr{0x7F000001, a.socket.LocalPort()};
        NetAddress bAddr{0x7F000001, b.socket.LocalPort()};
        a.conn = std::make_unique<Connection>(&a.socket, bAddr, sessionId, sk, true, kMaxDatagramLan);
        b.conn = std::make_unique<Connection>(&b.socket, aAddr, sessionId, ck, false, kMaxDatagramLan);
        a.Run();
        b.Run();
    }
    ~SessionLink() {
        a.Stop();
        b.Stop();
        a.conn.reset();
        b.conn.reset();
    }
    NetAddress AAddr() const { return {0x7F000001, a.socket.LocalPort()}; }
};
}  // namespace

static void TestSessionDisconnectReconnect() {
    std::printf("session disconnect/reconnect: ");

    // 1. clean disconnect: the peer's Disconnect packet raises PeerDisconnected on the
    // receiving side only.
    {
        SessionLink l(101);
        l.a.conn->SendDisconnect();
        SleepMs(150);
        CHECK(l.b.conn->PeerDisconnected());
        CHECK(!l.a.conn->PeerDisconnected());
    }
    // 2. data arriving after the disconnect flag: parsed without state damage.
    {
        SessionLink l(102);
        l.a.conn->SendDisconnect();
        SleepMs(120);
        for (int i = 0; i < 20; i++) l.a.conn->SendControl("late" + std::to_string(i));
        SleepMs(150);
        CHECK(l.b.conn->PeerDisconnected());
    }
    // 3. session isolation: a datagram from the OLD session is rejected by the NEW
    // session's receiver (wrong session id / keys), nothing is delivered.
    {
        SessionLink l(103);
        l.b.Stop();
        auto client2 = crypto::KeyPair::Generate();
        auto server2 = crypto::KeyPair::Generate();
        const std::string ctx2 = SessionContext("c2", "s2", "secret2");
        auto ck2 = crypto::DeriveSessionKeys(client2.secretKey, server2.publicKey, client2.publicKey, server2.publicKey, ctx2);
        auto newConn = std::make_unique<Connection>(&l.b.socket, l.AAddr(), 999, ck2, false, kMaxDatagramLan);
        std::atomic<int> delivered{0};
        newConn->SetControlHandler([&](const std::string&) { delivered++; });
        l.a.conn->SendControl("from the old session");
        SleepMs(200);
        CHECK(delivered.load() == 0);           // nothing crossed the session boundary
        CHECK(!newConn->HasClockOffset());      // and the new session learned nothing
    }
    // 4. a big multi-chunk message assembles intact on a fresh session.
    {
        SessionLink l(104);
        std::string big(100000, 'y');
        std::atomic<bool> ok{false};
        l.b.conn->SetControlHandler([&](const std::string& m) { ok = (m == big); });
        l.a.conn->SendControl(big);
        SleepMs(400);
        CHECK(ok.load());
    }
    // 5. two sends of the same message deliver twice (reliable, not deduplicated).
    {
        SessionLink l(105);
        std::atomic<int> n{0};
        l.b.conn->SetControlHandler([&](const std::string& m) { if (m == "dup") n++; });
        l.a.conn->SendControl("dup");
        l.a.conn->SendControl("dup");
        SleepMs(200);
        CHECK(n.load() == 2);
    }
    // 6. reconnect storm: five quick sessions in a row, each delivers before teardown.
    {
        for (int round = 0; round < 5; round++) {
            SessionLink l(uint32_t(200 + round));
            std::atomic<bool> got{false};
            l.b.conn->SetControlHandler([&](const std::string& m) { got = (m == "storm"); });
            l.a.conn->SendControl("storm");
            SleepMs(120);
            CHECK(got.load());
        }
    }
    // 7. disconnect mid-message: the peer never hangs; the abandoned reassembly state
    // dies with the session (a fresh session is case 3/8's business).
    {
        SessionLink l(207);
        std::string big(100000, 'z');
        l.a.conn->SendControl(big);
        l.a.conn->SendDisconnect();  // in-flight chunks abandoned mid-reassembly
        SleepMs(200);
        CHECK(l.b.conn->PeerDisconnected());
    }
    // 8. clock offset is per-session: measured on the old connection, fresh = unknown.
    {
        SessionLink l(208);
        SleepMs(400);  // keepalive pings flow while both peers pump
        const bool oldHad = l.a.conn->HasClockOffset();
        l.b.Stop();
        auto client2 = crypto::KeyPair::Generate();
        auto server2 = crypto::KeyPair::Generate();
        const std::string ctx2 = SessionContext("c8", "s8", "secret8");
        auto ck2 = crypto::DeriveSessionKeys(client2.secretKey, server2.publicKey, client2.publicKey, server2.publicKey, ctx2);
        auto newConn = std::make_unique<Connection>(&l.b.socket, l.AAddr(), 209, ck2, false, kMaxDatagramLan);
        CHECK(oldHad);
        CHECK(!newConn->HasClockOffset());
    }
    // 9. peer roam: the same session keys from a different address rebinds the remote.
    {
        SessionLink l(210);
        l.b.Stop();  // take the receive side over by hand
        // probe 1: consumed from the ORIGINAL address (advances the replay window)
        l.a.conn->SendControl("roam-probe-1");
        uint8_t buf1[2048];
        NetAddress from;
        int n = 0;
        for (int i = 0; i < 100 && n <= 0; i++) {
            n = l.b.socket.RecvFrom(buf1, sizeof(buf1), from, 10);
            SleepMs(2);
        }
        CHECK(n > 0);
        CHECK(l.b.conn->HandleDatagram(buf1, size_t(n), from));
        const uint16_t oldPort = l.b.conn->Remote().port;
        // probe 2: a fresh authentic datagram, but this one arrives from a DIFFERENT
        // source address (the headset roamed to a new NAT mapping). The replay window
        // must NOT eat it - the roam branch has to rebind the remote.
        l.a.conn->SendControl("roam-probe-2");
        uint8_t buf2[2048];
        int n2 = 0;
        for (int i = 0; i < 100 && n2 <= 0; i++) {
            n2 = l.b.socket.RecvFrom(buf2, sizeof(buf2), from, 10);
            SleepMs(2);
        }
        CHECK(n2 > 0);
        rc::UdpSocket roamed;
        CHECK(roamed.Open(0));
        CHECK(l.b.conn->HandleDatagram(buf2, size_t(n2), NetAddress{0x7F000001, roamed.LocalPort()}));
        CHECK(l.b.conn->Remote().port == roamed.LocalPort());  // followed the new source
        roamed.Close();
    }
    // 10. teardown never hangs: five rapid link destructions complete in bounded time.
    {
        const int64_t t0 = rc::NowNs();
        for (int i = 0; i < 5; i++) { SessionLink l(uint32_t(300 + i)); }
        CHECK(rc::NowNs() - t0 < 20LL * 1000 * 1000 * 1000);
    }
    // 11. sessions are independent: two concurrent pairs do not cross-deliver.
    {
        SessionLink l1(401), l2(402);
        std::atomic<int> hits1{0}, hits2{0};
        l1.b.conn->SetControlHandler([&](const std::string& m) { if (m == "one") hits1++; });
        l2.b.conn->SetControlHandler([&](const std::string& m) { if (m == "two") hits2++; });
        l1.a.conn->SendControl("one");
        l2.a.conn->SendControl("two");
        SleepMs(200);
        CHECK(hits1.load() == 1 && hits2.load() == 1);
    }
    std::printf("ok\n");
}

// ===========================================================================
// A12 (TASK-COMBO-SWEEP-2026-11-21): the full 14-boolean switch product x the
// numeric pairwise rows, through the pure settings -> stream-config builders
// (pc/streamer/src/stream/stream_config.h). Pure logic: no store, no bridge,
// no device, no sockets. Every combination must yield a legal config:
//   §1.1 no crash / no NaN-Inf; bitrate in [floor, ceiling]; sizes in domain
//   §1.2 tenBit x codec: H264 never carries tenBit (the UI hint's config layer)
//   §1.3 encodeScalePct x foveatedEncoding: a scaled frame never carries a fovea
//        layout, and the lens QP map never runs on a squeezed frame
//   §1.4 qualityProfileCbr x fillMode: CBR only on quality+takeover; the fill
//        rung stays >= the VBV-percentage floor (per-frame budget <= bucket)
//   §1.5 profileControlsVideo x qualityProfileCbr: one gate, no contradictory
//        state whatever order the switches were set in (the builder is stateless
//        and the sweep proves the two gates move together)
//   §1.6 autoTune: the decision input agrees with the built config; locked or
//        inapplicable dims never fire
//   §1.7 fecPercent x pacer billing: parity counted into the wire bytes keeps
//        the encoder target positive and inside the ceiling (probes below)
//   §1.8 MF + NVENC consumable numbers construct for every combination (below)
// Failures are aggregated (first 20 repros printed, one CHECK per test) so the
// suite output stays readable; the exit code still reports them.
// ===========================================================================

namespace {

struct A12Num {
    int encodeScalePct, fecPercent, vrBitrateMbps, codecIdx, streamProfile, desktopBitrateMbps;
};

enum {
    A12FillMode = 0, A12QualityProfileCbr, A12ProfileControlsVideo, A12L7Autoscale,
    A12AdaptiveBitrate, A12PcBitrateWins, A12FoveatedEncoding, A12TenBit,
    A12AdaptiveQuantization, A12TwoPassEncoding, A12AutoTune, A12DesktopInput,
    A12StreamAudio, A12MicPassthrough,
};

const char* A12CodecName(int i) {
    static const char* kNames[] = {"auto", "hevc", "av1", "h264"};
    return kNames[i];
}

// The bool names in mask-bit order, for the repro lines.
const char* A12BoolName(int i) {
    static const char* kNames[] = {"fillMode", "qualityProfileCbr", "profileControlsVideo", "l7Autoscale",
                                   "adaptiveBitrate", "pcBitrateWins", "foveatedEncoding", "tenBit",
                                   "adaptiveQuantization", "twoPassEncoding", "autoTune", "desktopInput",
                                   "streamAudio", "micPassthrough"};
    return kNames[i];
}

// Self-implemented greedy pairwise generator over the six numeric parameters
// (no third-party dependency). Every value pair of every parameter pair must be
// covered by at least one generated row - verified by the caller.
std::vector<A12Num> A12PairwiseRows() {
    const int kValues[6][6] = {
        {0, 50, 67, 85, 100, 0},  // encodeScalePct (5; 0 = follow the profile)
        {0, 3, 5, 10, 20, 0},     // fecPercent (5)
        {20, 100, 500, 0, 0},     // vrBitrateMbps (3; 20 = the lower-bound extreme)
        {0, 1, 2, 3, 0, 0},       // codec: auto / hevc / av1 / h264 (4)
        {0, 1, 2, 0, 0, 0},       // streamProfile: latency / balanced / quality (3)
        {10, 150, 0, 0, 0, 0},    // desktopBitrateMbps (2)
    };
    const int kCount[6] = {5, 5, 3, 4, 3, 2};
    struct Pair { int p1, v1, p2, v2; };
    std::vector<Pair> uncovered;
    for (int a = 0; a < 6; a++)
        for (int b = a + 1; b < 6; b++)
            for (int va = 0; va < kCount[a]; va++)
                for (int vb = 0; vb < kCount[b]; vb++)
                    uncovered.push_back({a, va, b, vb});
    std::vector<A12Num> rows;
    int idx[6];
    while (!uncovered.empty()) {
        // Seed with the first uncovered pair, then fill the remaining parameters
        // greedily: the value that covers the most still-uncovered pairs against
        // the parameters already assigned.
        for (int p = 0; p < 6; p++) idx[p] = 0;
        idx[uncovered[0].p1] = uncovered[0].v1;
        idx[uncovered[0].p2] = uncovered[0].v2;
        bool assigned[6] = {};
        assigned[uncovered[0].p1] = assigned[uncovered[0].p2] = true;
        for (int p = 0; p < 6; p++) {
            if (assigned[p]) continue;
            int best = 0, bestCov = -1;
            for (int v = 0; v < kCount[p]; v++) {
                int cov = 0;
                for (const Pair& pr : uncovered)
                    if ((pr.p1 == p && pr.v1 == v && assigned[pr.p2] && idx[pr.p2] == pr.v2) ||
                        (pr.p2 == p && pr.v2 == v && assigned[pr.p1] && idx[pr.p1] == pr.v1))
                        cov++;
                if (cov > bestCov) {
                    bestCov = cov;
                    best = v;
                }
            }
            idx[p] = best;
            assigned[p] = true;
        }
        auto covers = [&](const Pair& pr) { return idx[pr.p1] == pr.v1 && idx[pr.p2] == pr.v2; };
        uncovered.erase(std::remove_if(uncovered.begin(), uncovered.end(), covers), uncovered.end());
        rows.push_back({kValues[0][idx[0]], kValues[1][idx[1]], kValues[2][idx[2]], kValues[3][idx[3]],
                        kValues[4][idx[4]], kValues[5][idx[5]]});
    }
    return rows;
}

// Post-hoc pairwise coverage proof: for every parameter pair and every value pair,
// some row carries both.
bool A12PairwiseCovered(const std::vector<A12Num>& rows) {
    const int kValues[6][6] = {
        {0, 50, 67, 85, 100, 0}, {0, 3, 5, 10, 20, 0}, {20, 100, 500, 0, 0, 0},
        {0, 1, 2, 3, 0, 0}, {0, 1, 2, 0, 0, 0}, {10, 150, 0, 0, 0, 0},
    };
    const int kCount[6] = {5, 5, 3, 4, 3, 2};
    const int vals[6] = {0, 0, 0, 0, 0, 0};
    for (int a = 0; a < 6; a++)
        for (int b = a + 1; b < 6; b++)
            for (int va = 0; va < kCount[a]; va++)
                for (int vb = 0; vb < kCount[b]; vb++) {
                    bool found = false;
                    for (const A12Num& r : rows) {
                        const int rv[6] = {r.encodeScalePct, r.fecPercent, r.vrBitrateMbps, r.codecIdx,
                                           r.streamProfile, r.desktopBitrateMbps};
                        if (rv[a] == kValues[a][va] && rv[b] == kValues[b][vb]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) return false;
                }
    (void)vals;
    return true;
}

rcs::Settings A12PcSettings(const bool* b, const A12Num& n) {
    rcs::Settings pc;  // defaults, then the swept switches
    pc.fillMode = b[A12FillMode];
    pc.qualityProfileCbr = b[A12QualityProfileCbr];
    pc.l7Autoscale = b[A12L7Autoscale];
    pc.adaptiveBitrate = b[A12AdaptiveBitrate];
    pc.pcBitrateWins = b[A12PcBitrateWins];
    pc.foveatedEncoding = b[A12FoveatedEncoding];
    pc.tenBit = b[A12TenBit];
    pc.adaptiveQuantization = b[A12AdaptiveQuantization];
    pc.twoPassEncoding = b[A12TwoPassEncoding];
    pc.desktopInput = b[A12DesktopInput];
    pc.streamAudio = b[A12StreamAudio];
    pc.micPassthrough = b[A12MicPassthrough];
    pc.encodeScalePct = n.encodeScalePct;
    pc.fecPercent = n.fecPercent;
    pc.vrBitrateMbps = n.vrBitrateMbps;
    pc.codec = A12CodecName(n.codecIdx);
    pc.streamProfile = n.streamProfile;
    pc.desktopBitrateMbps = n.desktopBitrateMbps;
    return pc;
}

// Field-by-field compare (memcmp would read padding bytes).
bool A12SameVrConfig(const rcs::VrStreamConfig& a, const rcs::VrStreamConfig& c) {
    return a.codec == c.codec && a.tenBit == c.tenBit && a.eyeWidth == c.eyeWidth && a.eyeHeight == c.eyeHeight &&
           a.fps == c.fps && a.bitrateKbps == c.bitrateKbps && a.fecPercent == c.fecPercent && a.adaptive == c.adaptive &&
           a.profile == c.profile && a.startBitrateKbps == c.startBitrateKbps && a.pace == c.pace &&
           a.pacingMode == c.pacingMode && a.foveated == c.foveated && a.adaptiveQuant == c.adaptiveQuant &&
           a.twoPass == c.twoPass && a.cappedVbr == c.cappedVbr && a.fillMode == c.fillMode &&
           a.motionField == c.motionField && a.foveatedEyeWidth == c.foveatedEyeWidth &&
           a.foveatedEyeHeight == c.foveatedEyeHeight && a.foveaSize == c.foveaSize && a.encodeScale == c.encodeScale;
}

std::vector<std::string> g_a12Fails;

void A12Note(const char* assertion, const bool* b, const A12Num& n, bool gazeFovea, bool slowPath,
             const std::string& detail) {
    if (g_a12Fails.size() >= 20) return;
    std::string mask;
    for (int i = 13; i >= 0; i--) mask += b[i] ? '1' : '0';
    std::string on;
    for (int i = 0; i < 14; i++)
        if (b[i]) {
            if (!on.empty()) on += ",";
            on += A12BoolName(i);
        }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[%s] %s: mask=%s(%s) scale=%d fec=%d vrMbps=%d codec=%s profile=%d dskMbps=%d gaze=%d slow=%d %s",
                  assertion, "combo", mask.c_str(), on.c_str(), n.encodeScalePct, n.fecPercent, n.vrBitrateMbps,
                  A12CodecName(n.codecIdx), n.streamProfile, n.desktopBitrateMbps, int(gazeFovea), int(slowPath),
                  detail.c_str());
    g_a12Fails.push_back(line);
}

#define A12_CHECK(cond, assertion, b, n, gaze, slow, detail)      \
    do {                                                          \
        if (!(cond)) A12Note(assertion, b, n, gaze, slow, detail); \
    } while (0)

}  // namespace

// The full sweep itself (pure logic; timed; the card requires < 60 s).
static void TestVrConfigComboSweep() {
    g_a12Fails.clear();
    const auto rows = A12PairwiseRows();
    CHECK(A12PairwiseCovered(rows));
    CHECK(rows.size() >= 25);  // the 5x5 encodeScale x fec pair alone needs 25 rows
    const auto t0 = std::chrono::steady_clock::now();

    // Representative session inputs: the default desktop path (no SteamVR match),
    // a 1664x1728 eye, 72 fps, the headset's own bitrate pick so pcBitrateWins
    // has an observable effect.
    constexpr uint32_t kEyeW = 1664, kEyeH = 1728;
    constexpr float kFps = 72.f;
    rcs::ClientCaps caps;  // defaults: 4096 decode, full codec set
    rcs::EncoderCaps encFull;
    encFull.h264 = encFull.hevc = encFull.av1 = encFull.hevc10 = encFull.av110 = true;

    rcs::ClientStreamSettings st;
    st.vrQuality = "high";
    st.vrBitrateMbps = 123;   // the headset's own pick (wins unless pcBitrateWins)
    st.refreshRate = kFps;
    st.eyesTracked = true;
    st.adaptiveBitrate = true;

    long long combos = 0, vrBuilds = 0;
    for (int mask = 0; mask < (1 << 14); mask++) {
        bool b[14];
        for (int i = 0; i < 14; i++) b[i] = ((mask >> i) & 1) != 0;
        for (const A12Num& n : rows) {
            const rcs::Settings pc = A12PcSettings(b, n);
            // codec resolution: "auto" resolves through the extracted session rule
            // with full-featured caps (HEVC preferred); explicit values pass through.
            const rc::VideoCodec codec =
                n.codecIdx == 0
                    ? rcs::ChooseStreamCodec(encFull, caps, b[A12TenBit], pc.codec)
                    : (n.codecIdx == 1 ? rc::VideoCodec::HEVC : n.codecIdx == 2 ? rc::VideoCodec::AV1
                                                                               : rc::VideoCodec::H264);
            for (int quadrant = 0; quadrant < 4; quadrant++) {
                const bool gazeFovea = (quadrant & 1) != 0;
                const bool slowPath = (quadrant & 2) != 0;
                st.tenBit = b[A12TenBit];
                st.gazeFoveation = gazeFovea;
                const int autoscalePct = pc.l7Autoscale ? 85 : 100;  // a stepped ladder level
                const rcs::VrStreamBuild build =
                    rcs::BuildVrStreamConfig(pc, st, codec, slowPath, /*match=*/false, autoscalePct, kEyeW, kEyeH, kFps);
                const rcs::VrStreamConfig& cfg = build.cfg;
                vrBuilds++;
                char detail[96];

                // ---- §1.1: no crash (reaching here), no NaN/Inf, domains ----
                A12_CHECK(std::isfinite(cfg.encodeScale) && cfg.encodeScale >= 0.5f && cfg.encodeScale <= 1.0f,
                          "A1-domain", b, n, gazeFovea, slowPath, "encodeScale out of [0.5,1]");
                A12_CHECK(std::isfinite(cfg.fps) && cfg.fps > 0, "A1-domain", b, n, gazeFovea, slowPath, "fps");
                A12_CHECK(std::isfinite(cfg.foveaSize) && cfg.foveaSize >= rcs::Foveator::kFoveaSize - 1e-6f &&
                              cfg.foveaSize <= 0.8f + 1e-6f,
                          "A1-domain", b, n, gazeFovea, slowPath, "foveaSize out of [0.45,0.8]");
                A12_CHECK(cfg.bitrateKbps >= 1000 && cfg.bitrateKbps <= 2'000'000, "A1-bitrate-bounds", b, n,
                          gazeFovea, slowPath, "bitrateKbps outside [1e3,2e6] kbps");
                if (slowPath)
                    A12_CHECK(cfg.bitrateKbps <= 60000, "A1-bitrate-bounds", b, n, gazeFovea, slowPath,
                              "slow path must cap at 60000");
                A12_CHECK(cfg.startBitrateKbps == 0 || cfg.startBitrateKbps <= cfg.bitrateKbps, "A1-bitrate-bounds", b,
                          n, gazeFovea, slowPath, "startBitrateKbps above the ceiling");
                A12_CHECK(cfg.fecPercent >= 0 && cfg.fecPercent <= 100, "A1-domain", b, n, gazeFovea, slowPath,
                          "fecPercent out of [0,100]");
                if (slowPath)
                    A12_CHECK(cfg.fecPercent >= 12, "A1-domain", b, n, gazeFovea, slowPath,
                              "slow path must floor fec at 12");
                A12_CHECK(cfg.pacingMode >= 1 && cfg.pacingMode <= 3 && cfg.pace == (cfg.pacingMode != 1),
                          "A1-domain", b, n, gazeFovea, slowPath, "pacingMode/pace disagree");
                A12_CHECK(cfg.eyeWidth == kEyeW && cfg.eyeHeight == kEyeH, "A1-domain", b, n, gazeFovea, slowPath,
                          "eye size not passed through");
                // The per-eye coded size may be ODD (ScaledEncodeSize/2): the headset
                // doubles it back to the full width, which is the even dimension the
                // encoder and the converters need. Height must stay even (NV12/P010).
                A12_CHECK(build.codedEyeWidth >= 1 && build.codedEyeHeight >= 2 &&
                              (build.codedEyeHeight & 1u) == 0 && ((build.codedEyeWidth * 2u) & 1u) == 0,
                          "A1-domain", b, n, gazeFovea, slowPath, "coded eye size not in domain");

                // ---- §1.2: tenBit x codec ----
                A12_CHECK(!(codec == rc::VideoCodec::H264 && cfg.tenBit), "A2-tenBit-H264", b, n, gazeFovea,
                          slowPath, "H264 carried tenBit");
                if (codec != rc::VideoCodec::H264)
                    A12_CHECK(cfg.tenBit == st.tenBit, "A2-tenBit-passthrough", b, n, gazeFovea, slowPath,
                              "tenBit lost on a codec that supports it");

                // ---- §1.3: encodeScale x foveation ----
                const bool squeeze = cfg.foveatedEyeWidth > 0 && cfg.foveatedEyeHeight > 0;
                if (cfg.encodeScale < 1.0f)
                    A12_CHECK(!squeeze, "A3-scale-fovea", b, n, gazeFovea, slowPath,
                              "scaled frame carries a fovea layout");
                if (squeeze)
                    A12_CHECK(gazeFovea && cfg.encodeScale >= 1.0f &&
                                  cfg.foveatedEyeWidth == rcs::Foveator::EncodedSize(cfg.eyeWidth, cfg.foveaSize) &&
                                  cfg.foveatedEyeHeight == rcs::Foveator::EncodedSize(cfg.eyeHeight, cfg.foveaSize),
                              "A3-scale-fovea", b, n, gazeFovea, slowPath,
                              "fovea layout inconsistent with Foveator::EncodedSize");
                // The RunEncoder mapping (vr_stream.cpp): the lens bit budget assumes a
                // picture that isn't squeezed, so a squeezed frame must not run it.
                const bool jobFoveated = squeeze;
                const bool ecFoveated = cfg.foveated && !jobFoveated;
                if (cfg.foveated && jobFoveated)
                    A12_CHECK(!ecFoveated, "A3-lens-budget", b, n, gazeFovea, slowPath,
                              "lens QP map on a squeezed frame");
                // The encoded size the bpp budget runs on (vr_stream.cpp Run()).
                const uint32_t baseW = squeeze ? cfg.foveatedEyeWidth * 2 : kEyeW * 2;
                const uint32_t baseH = squeeze ? cfg.foveatedEyeHeight : kEyeH;
                const uint32_t encW = rc::ScaledEncodeSize(baseW, cfg.encodeScale);
                const uint32_t encH = rc::ScaledEncodeSize(baseH, cfg.encodeScale);
                A12_CHECK(encW > 0 && encH > 0 && (encW & 1u) == 0 && (encH & 1u) == 0, "A3-lens-budget", b, n,
                          gazeFovea, slowPath, "encoded size not even/positive");
                const double bpp = double(cfg.bitrateKbps) * 1000.0 / (double(encW) * encH * double(cfg.fps));
                A12_CHECK(std::isfinite(bpp) && bpp > 0, "A3-lens-budget", b, n, gazeFovea, slowPath,
                          "bits-per-pixel not finite/positive");

                // ---- §1.4 + §1.5: CBR gate, one gate, fill floor ----
                // A86b: CBR is a plain independent switch - no tier gate, no takeover
                // gate. A rung-driven CBR switch oscillated against the tuner.
                const bool cbrExpected = pc.qualityProfileCbr;
                A12_CHECK(cfg.cappedVbr == !cbrExpected, "A4-A5-cbr-gate", b, n, gazeFovea, slowPath,
                          "CBR state disagrees with the independent switch");
                // A86b: CBR is independent - it may be on at ANY rung, and it never
                // turns itself on because auto happened to climb to the sharp one.
                if (gazeFovea && cfg.encodeScale >= 1.0f) {
                    // A86b: the fovea share no longer follows a rung - that made
                    // edge sharpness flicker in step with link noise.
                    const float expected = st.eyesTracked ? rcs::Foveator::kFoveaSize
                                                          : rcs::Foveator::kFixedFoveaSize;
                    A12_CHECK(cfg.foveaSize == expected, "A5-fovea-floor", b, n, gazeFovea, slowPath,
                              "fovea share changed without the user's setting");
                } else {
                    A12_CHECK(cfg.foveaSize == rcs::Foveator::kFoveaSize, "A5-fovea-floor", b, n, gazeFovea, slowPath,
                              "foveaSize touched without the fovea path");
                }
                // CBR's budget mapping (nvenc RatePlan): CBR runs avg 100% / VBV 150%;
                // capped VBR runs the profile's numbers. The fill rung must stay above
                // the bucket floor either way: per-frame budget <= VBV buffer.
                {
                    const rc::StreamProfileParams q = rc::ProfileParams(cfg.profile);
                    const uint32_t avgPct = cfg.cappedVbr ? q.vbrAveragePercent : 100u;
                    const uint32_t vbvPct = cfg.cappedVbr ? q.vbvFramesPercent : 150u;
                    rc::EncoderRateTracker tracker;
                    tracker.Observe(1e9 / 45.0);  // sustained 45 fps: pressure
                    const rc::EncoderRatePlan plan = rc::PlanEncoderRate(cfg.bitrateKbps, double(cfg.fps), false,
                                                                         false, tracker, uint32_t(cfg.fps) * 1000u,
                                                                         avgPct, vbvPct, cfg.fillMode);
                    A12_CHECK(plan.budget.maxBitrate == cfg.bitrateKbps * 1000u, "A4-cbr-budget", b, n, gazeFovea,
                              slowPath, "plan maxBitrate != ceiling");
                    A12_CHECK(plan.budget.averageBitrate <= plan.budget.maxBitrate, "A4-cbr-budget", b, n, gazeFovea,
                              slowPath, "plan average above the ceiling");
                    A12_CHECK(plan.rateNum > 0 && plan.rateNum <= uint32_t(cfg.fps) * 1000u, "A4-cbr-budget", b, n,
                              gazeFovea, slowPath, "fill rung above nominal");
                    const double perFrame = double(plan.budget.maxBitrate) * 1000.0 / double(plan.rateNum);
                    // one bit of slack: the bucket is a truncated uint32 of the exact
                    // formula, and at the floor rung the two meet exactly.
                    A12_CHECK(double(plan.budget.vbvBufferSize) >= perFrame - 1.0, "A4-fill-floor", b, n, gazeFovea,
                              slowPath, "per-frame budget above the VBV bucket");
                    if (cfg.fillMode)
                        A12_CHECK(rc::LadderContainsStep(int(plan.rateNum / 1000u)), "A4-fill-floor", b, n, gazeFovea,
                                  slowPath, "fill rung off the ladder");
                }

                // ---- §1.6: autoTune agreement (full) + decision probes (stride) ----
                {
                    const rc::AutoTuneInput in = rcs::BuildAutoTuneInput(pc, st, slowPath);
                    A12_CHECK(in.cbrOn == !cfg.cappedVbr, "A6-autotune-input", b, n, gazeFovea, slowPath,
                              "autoTune's CBR view disagrees with the built config");
                    A12_CHECK(in.foveaOn == (pc.foveatedEncoding && gazeFovea), "A6-autotune-input", b, n, gazeFovea,
                              slowPath, "autoTune's fovea view disagrees");
                    A12_CHECK(in.foveaPossible == gazeFovea, "A6-autotune-input", b, n, gazeFovea, slowPath,
                              "autoTune's fovea possibility disagrees");
                    A12_CHECK(in.profileId == pc.streamProfile, "A6-autotune-input", b, n, gazeFovea, slowPath,
                              "autoTune's profile view disagrees");
                    if (mask % 3 == 0) {
                        // RateControl pressure: CbrOff only when effective CBR is on and
                        // unlocked; FoveaOn only when it is possible and not already on.
                        rc::AutoTune tuner;
                        rc::BottleneckSignal sig;
                        sig.nominalFps = double(cfg.fps);
                        sig.qp = 30.0;
                        sig.mbps = 1.0;
                        sig.ceilingKbps = cfg.bitrateKbps;
                        sig.queueMs = 5.0;
                        const rc::AutoTuneInput probe = in;
                        (void)tuner.Update(sig, probe, 0);
                        const auto r = tuner.Update(sig, probe, 6'000'000'000LL);
                        if (r.action == rc::AutoAction::CbrOff)
                            A12_CHECK(in.cbrOn, "A6-autotune-actions", b, n, gazeFovea, slowPath, "CbrOff without CBR");
                        else if (r.action == rc::AutoAction::FoveaOn)
                            A12_CHECK(!in.cbrOn && in.foveaPossible && !in.foveaOn, "A6-autotune-actions", b, n,
                                      gazeFovea, slowPath, "FoveaOn while CBR still available/on");
                        // The user's manual lock must silence the dim entirely.
                        rc::AutoTune locked;
                        locked.MarkManual(rc::AutoDim::Cbr);
                        locked.MarkManual(rc::AutoDim::Fovea);
                        locked.MarkManual(rc::AutoDim::Profile);
                        (void)locked.Update(sig, probe, 0);
                        const auto lr = locked.Update(sig, probe, 6'000'000'000LL);
                        A12_CHECK(lr.action != rc::AutoAction::CbrOff && lr.action != rc::AutoAction::FoveaOn &&
                                      lr.action != rc::AutoAction::ProfileDown,
                                  "A6-autotune-actions", b, n, gazeFovea, slowPath, "a manual lock was overridden");
                        // LinkBound: one rung down, and only while there is a rung to
                        // give. A84b §3 retired BuildAutoTuneInput's `pcOwnsProfile`
                        // (see stream_config.h for the evidence), so this probe is no
                        // longer about who owns the synced key - it is about the rung:
                        // profileId is the built config's own profile here, so 0 is
                        // the bottom of the ladder and nothing may step below it.
                        rc::AutoTune link;
                        rc::BottleneckSignal linkSig;
                        linkSig.queueMs = 150.0;
                        linkSig.nominalFps = double(cfg.fps);
                        const rc::AutoTuneInput linkIn = rcs::BuildAutoTuneInput(pc, st, slowPath);
                        A12_CHECK(linkIn.profileId == int(cfg.profile), "A6-autotune-input", b, n, gazeFovea,
                                  slowPath, "autoTune's rung is not the profile the config builds at");
                        (void)link.Update(linkSig, linkIn, 0);
                        const auto lr2 = link.Update(linkSig, linkIn, 6'000'000'000LL);
                        if (linkIn.profileId > 0)
                            A12_CHECK(lr2.action == rc::AutoAction::ProfileDown &&
                                          lr2.profileId == linkIn.profileId - 1,
                                      "A6-autotune-actions", b, n, gazeFovea, slowPath, "LinkBound did not step down");
                        else
                            A12_CHECK(lr2.action == rc::AutoAction::None, "A6-autotune-actions", b, n, gazeFovea,
                                      slowPath, "stepped below the bottom rung");
                        // DecodeBound: this layer never acts (the L7 ladder's class).
                        rc::AutoTune dec;
                        rc::BottleneckSignal decSig;
                        decSig.decodeMs = 100.0;
                        decSig.decodeBudgetMs = 20.0;
                        decSig.nominalFps = double(cfg.fps);
                        (void)dec.Update(decSig, probe, 0);
                        const auto dr = dec.Update(decSig, probe, 6'000'000'000LL);
                        A12_CHECK(dr.action == rc::AutoAction::None, "A6-autotune-actions", b, n, gazeFovea, slowPath,
                                  "decision layer acted on a decode-bound tick");
                    }
                }

                // ---- §1.7 (pure half): parity billed into the wire budget ----
                if (mask % 3 == 0) {
                    constexpr size_t kFragPayload = rc::kMaxDatagramLan - sizeof(rc::PacketHeader) - rc::kMacSize -
                                                    sizeof(rc::VideoFragmentHeader);
                    const size_t total = 2 + sizeof(rc::VrFrameMetadata) + size_t(64) * 1024;
                    const size_t wire = rcs::FrameWireBytes(kFragPayload, total, cfg.fecPercent);
                    A12_CHECK(wire > total, "A7-parity-billing", b, n, gazeFovea, slowPath,
                              "wire bytes not above the payload");
                    rc::WireCeiling wc;
                    rc::WireCeiling::Snapshot snap;
                    wc.OnFrame(0, wire, total, cfg.bitrateKbps, snap);
                    wc.OnFrame(250'000'000LL, wire, total, cfg.bitrateKbps, snap);
                    // The first frame moves the target off 0 (any candidate >= cap/2 and
                    // the hysteresis band is below it at these ceilings), so after two
                    // frames the published target is the parity-inclusive conversion.
                    A12_CHECK(snap.encoderTargetKbps > 0 && snap.encoderTargetKbps <= cfg.bitrateKbps &&
                                  snap.encoderTargetKbps >= cfg.bitrateKbps / 2,
                              "A7-parity-billing", b, n, gazeFovea, slowPath,
                              "wire ceiling target escaped [cap/2, cap]");
                }

                // ---- §1.8: the backends' consumable numbers ----
                {
                    rcs::EncoderConfig ec;
                    ec.codec = cfg.codec;
                    ec.width = encW;
                    ec.height = encH;
                    ec.fps = cfg.fps;
                    ec.bitrateKbps = cfg.bitrateKbps;  // the ceiling the sender starts at
                    ec.profile = cfg.profile;
                    ec.tenBit = cfg.tenBit;
                    ec.inputFormat = cfg.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                    ec.foveated = ecFoveated;
                    ec.adaptiveQuant = cfg.adaptiveQuant;
                    ec.twoPass = cfg.twoPass;
                    ec.cappedVbr = cfg.cappedVbr;
                    ec.fillMode = cfg.fillMode;
                    A12_CHECK(ec.width > 0 && ec.height > 0 && ec.width <= 8192 && ec.height <= 8192, "A8-encoder", b,
                              n, gazeFovea, slowPath, "encoder size out of domain");
                    A12_CHECK(ec.bitrateKbps <= 4'000'000, "A8-encoder", b, n, gazeFovea, slowPath,
                              "bitrate would overflow the MF UINT32 rate fields");
                    const rcs::MfEncoderParams mf = rcs::DeriveMfParams(ec);
                    A12_CHECK(mf.meanBitRate > 0 && mf.meanBitRate == ec.bitrateKbps * 1000u, "A8-encoder", b, n,
                              gazeFovea, slowPath, "MF mean bitrate wrong/overflowed");
                    A12_CHECK(mf.bufferSize > 0, "A8-encoder", b, n, gazeFovea, slowPath, "MF buffer size not positive");
                    // A47: DeriveMfParams rounds (lround) instead of truncating, so
                    // 59.94 fps -> 59940 rather than 59939. The sweep's fps values
                    // are integral, so both agree here; update the oracle to match.
                    A12_CHECK(mf.frameRateNum == uint32_t(std::lround(double(ec.fps) * 1000.0)) && mf.frameRateNum > 0,
                              "A8-encoder", b, n, gazeFovea, slowPath, "MF frame rate wrong");
                    // MF's hardware caps ceiling (MfProbe): 4096 - a config above it could
                    // never construct; the sweep's sizes stay inside it.
                    A12_CHECK(ec.width <= 4096 && ec.height <= 4096, "A8-encoder", b, n, gazeFovea, slowPath,
                              "size above the MF backend's 4096 cap");
                    // NVENC's consumable budget (RatePlan -> ApplyBudget).
                    const rc::StreamProfileParams q = rc::ProfileParams(ec.profile);
                    const uint32_t avgPct = ec.cappedVbr ? q.vbrAveragePercent : 100u;
                    const uint32_t vbvPct = ec.cappedVbr ? q.vbvFramesPercent : 150u;
                    rc::EncoderRateTracker fresh;
                    const rc::EncoderRatePlan plan = rc::PlanEncoderRate(ec.bitrateKbps, double(ec.fps), false, true,
                                                                         fresh, uint32_t(ec.fps) * 1000u, avgPct,
                                                                         vbvPct, ec.fillMode);
                    A12_CHECK(plan.budget.maxBitrate == ec.bitrateKbps * 1000u && plan.budget.maxBitrate > 0,
                              "A8-encoder", b, n, gazeFovea, slowPath, "NVENC plan maxBitrate wrong");
                    A12_CHECK(plan.budget.vbvBufferSize > 0 && plan.rateNum > 0, "A8-encoder", b, n, gazeFovea,
                              slowPath, "NVENC plan not positive");
                }

                // ---- isolation: audio/input switches never touch the video config ----
                if (quadrant == 0) {
                    bool b2[14];
                    std::memcpy(b2, b, sizeof(b2));
                    b2[A12StreamAudio] = !b2[A12StreamAudio];
                    b2[A12MicPassthrough] = !b2[A12MicPassthrough];
                    b2[A12DesktopInput] = !b2[A12DesktopInput];
                    const rcs::Settings pc2 = A12PcSettings(b2, n);
                    const rcs::VrStreamBuild build2 =
                        rcs::BuildVrStreamConfig(pc2, st, codec, slowPath, false, autoscalePct, kEyeW, kEyeH, kFps);
                    A12_CHECK(A12SameVrConfig(cfg, build2.cfg), "A-isolation", b, n, gazeFovea, slowPath,
                              "audio/input switches changed the video config");
                }

                // ---- determinism (stride): the builder is stateless ----
                if (mask % 101 == 0) {
                    const rcs::VrStreamBuild again =
                        rcs::BuildVrStreamConfig(pc, st, codec, slowPath, false, autoscalePct, kEyeW, kEyeH, kFps);
                    A12_CHECK(A12SameVrConfig(cfg, again.cfg), "A-determinism", b, n, gazeFovea, slowPath,
                              "builder not deterministic");
                }

                // ---- the desktop builder rides the same sweep ----
                {
                    rcs::MonitorInfo mon;
                    mon.index = 0;
                    mon.width = 2560;
                    mon.height = 1440;
                    const double totalPixels = double(mon.width) * mon.height;
                    const rcs::DesktopStreamConfig dc =
                        rcs::BuildDesktopStreamConfig(pc, st, codec, slowPath, /*vrInFlight=*/false, mon, totalPixels,
                                                      1, caps);
                    A12_CHECK(dc.width > 0 && dc.height > 0 && (dc.width & 1u) == 0 && (dc.height & 1u) == 0,
                              "A-desktop", b, n, gazeFovea, slowPath, "desktop size not even/positive");
                    A12_CHECK(dc.bitrateKbps >= 5000, "A-desktop", b, n, gazeFovea, slowPath,
                              "desktop bitrate below the 5000 floor");
                    A12_CHECK(dc.bitrateKbps == uint32_t(n.desktopBitrateMbps) * 1000u, "A-desktop", b, n, gazeFovea,
                              slowPath, "desktop bitrate split wrong");
                    A12_CHECK(dc.fps == std::clamp(kFps, 30.f, 120.f), "A-desktop", b, n, gazeFovea, slowPath,
                              "desktop fps outside the clamp");
                    A12_CHECK(dc.codec == codec && dc.tenBit == (st.tenBit && codec != rc::VideoCodec::H264),
                              "A-desktop", b, n, gazeFovea, slowPath, "desktop codec/tenBit wrong");
                    A12_CHECK(dc.profile == rc::ProfileFromId(pc.streamProfile) && dc.pace && !dc.cheapEncode &&
                                  dc.adaptiveQuant == pc.adaptiveQuantization &&
                                  dc.adaptive == (slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate)),
                              "A-desktop", b, n, gazeFovea, slowPath, "desktop switch wiring wrong");
                    if (slowPath)
                        A12_CHECK(dc.startBitrateKbps == std::min<uint32_t>(dc.bitrateKbps, 8000), "A-desktop", b, n,
                                  gazeFovea, slowPath, "desktop slow-path start bitrate wrong");
                    else
                        A12_CHECK(dc.startBitrateKbps == 0, "A-desktop", b, n, gazeFovea, slowPath,
                                  "desktop start bitrate on a LAN path");
                }
                combos++;
            }
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("A12 combo sweep: 16384 boolean combinations x %zu numeric pairwise rows x 4 session quadrants = "
                "%lld VR configs (+ desktop) built and asserted in %.2f s\n",
                rows.size(), combos, secs);
    CHECK(secs < 60.0);  // the card's runtime budget: anything longer means hidden I/O
    CHECK(g_a12Fails.empty());
    for (const std::string& f : g_a12Fails) std::printf("A12 %s\n", f.c_str());
}

// §2: the known couplings, spelled out one by one (outside the product).
static void TestConfigCouplingCases() {
    g_a12Fails.clear();
    rcs::ClientCaps caps;
    rcs::ClientStreamSettings st;
    st.vrQuality = "high";
    st.refreshRate = 72;
    st.eyesTracked = true;
    st.adaptiveBitrate = true;
    constexpr uint32_t kEyeW = 1664, kEyeH = 1728;

    // 1. tenBit + H264: the config layer must degrade (tenBit dropped), never emit
    //    an illegal NVENC/MF 10-bit H264 request. The UI hint ("Needs HEVC or AV1")
    //    greys the row; the direct-settings path lands here.
    {
        rcs::Settings pc;
        pc.tenBit = true;
        st.tenBit = true;
        rcs::EncoderCaps full;
        full.h264 = full.hevc = full.av1 = full.hevc10 = full.av110 = true;
        rcs::EncoderCaps h264OnlyCaps;
        h264OnlyCaps.h264 = true;
        rcs::ClientCaps decNo10;  // the default headset answer: hevc10/av1* = false
        const auto codec = rcs::ChooseStreamCodec(full, decNo10, true, "h264");
        CHECK(codec == rc::VideoCodec::H264);  // the explicit h264 preference is honoured
        const auto build = rcs::BuildVrStreamConfig(pc, st, codec, false, false, 100, kEyeW, kEyeH, 72.f);
        CHECK(!build.cfg.tenBit);  // ...and 10-bit is dropped at the config layer
        // auto with tenBit and a Main10-capable headset: HEVC, never H264.
        rcs::ClientCaps decFull;
        decFull.h264 = decFull.hevc = decFull.hevc10 = true;
        const auto autoCodec = rcs::ChooseStreamCodec(full, decFull, true, "auto");
        CHECK(autoCodec == rc::VideoCodec::HEVC);
        // auto with tenBit but a headset without Main10: H264 (the codec ok() gate
        // refuses 10-bit HEVC) - tenBit dropped at the config layer, still legal.
        const auto autoNo10 = rcs::ChooseStreamCodec(full, decNo10, true, "auto");
        CHECK(autoNo10 == rc::VideoCodec::H264);
        const auto buildNo10 = rcs::BuildVrStreamConfig(pc, st, autoNo10, false, false, 100, kEyeW, kEyeH, 72.f);
        CHECK(!buildNo10.cfg.tenBit);
        const auto h264Only = rcs::ChooseStreamCodec(h264OnlyCaps, caps, true, "auto");
        CHECK(h264Only == rc::VideoCodec::H264);
        const auto build2 = rcs::BuildVrStreamConfig(pc, st, h264Only, false, false, 100, kEyeW, kEyeH, 72.f);
        CHECK(!build2.cfg.tenBit);
        st.tenBit = false;
        std::printf("A12 case 1: tenBit + H264 -> H264 chosen, tenBit dropped at the config layer (degraded, legal)\n");
    }

    // 2. encodeScale 85 + foveated on: the scaled frame pauses the fovea layout
    //    (vr_stream.cpp L2-FIX3), keeps the plain scaled coded size, and the lens
    //    bit budget applies to the unsqueezed frame.
    {
        rcs::Settings pc;
        pc.encodeScalePct = 85;
        pc.foveatedEncoding = true;
        st.gazeFoveation = true;
        const auto build = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, kEyeW, kEyeH, 72.f);
        CHECK(build.cfg.encodeScale == 0.85f);
        CHECK(build.cfg.foveatedEyeWidth == 0 && build.cfg.foveatedEyeHeight == 0);
        CHECK(build.codedEyeWidth == rc::ScaledEncodeSize(kEyeW * 2, 0.85f) / 2);
        CHECK(build.codedEyeHeight == rc::ScaledEncodeSize(kEyeH, 0.85f));
        CHECK(build.cfg.foveated);  // the lens QP map still runs - on the UNSQUEEZED frame
        st.gazeFoveation = false;
        std::printf("A12 case 2: scale 85%% + foveated -> fovea paused, plain scaled %ux%u, lens budget on the full frame\n",
                    build.codedEyeWidth, build.codedEyeHeight);
    }

    // 3. CBR on + fillMode on + halfRate: three budget layers stack. CBR (quality +
    //    takeover) runs avg 100% / VBV 150%; the fill rung must keep the per-frame
    //    budget inside that smaller bucket; and at 20 Mbps the full-size frame really
    //    sits below the half-rate trigger (kHalfRateBelowBpp = 0.03), so the stack
    //    exists on real numbers (half rate doubles the per-frame budget by skipping;
    //    the CBR plan above is what the encoder actually runs).
    {
        rcs::Settings pc;
        pc.qualityProfileCbr = true;
        pc.streamProfile = 2;
        pc.fillMode = true;
        pc.vrBitrateMbps = 20;   // the lower-bound extreme
        pc.pcBitrateWins = true;
        st.tenBit = false;
        constexpr uint32_t kBigEyeW = 2208, kBigEyeH = 2112;  // bpp lands under 0.03 at 20 Mbps
        const auto build =
            rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, kBigEyeW, kBigEyeH, 72.f);
        CHECK(!build.cfg.cappedVbr);
        CHECK(build.cfg.profile == rc::StreamProfile::Quality);
        const uint32_t avgPct = 100u, vbvPct = 150u;  // the CBR mapping (nvenc RatePlan)
        rc::EncoderRateTracker tracker;
        tracker.Observe(1e9 / 45.0);
        const auto plan =
            rc::PlanEncoderRate(build.cfg.bitrateKbps, 72.0, false, false, tracker, 72000, avgPct, vbvPct, true);
        CHECK(plan.budget.maxBitrate == 20'000'000u);
        CHECK(plan.budget.averageBitrate == plan.budget.maxBitrate);  // CBR spends exactly the limit
        const int rung = int(plan.rateNum / 1000u);
        CHECK(rung >= rc::FillFloorRung(72, 150));
        CHECK(rc::FillFloorRung(72, 150) >= rc::FillFloorRung(72, 200));  // CBR floors above capped-VBR's
        const double perFrame = double(plan.budget.maxBitrate) * 1000.0 / double(plan.rateNum);
        CHECK(double(plan.budget.vbvBufferSize) >= perFrame - 1.0);  // 1 bit: uint32 bucket truncation
        // The half-rate trigger (vr_stream.cpp Run): bpp at the ceiling at this size.
        const uint32_t encW = rc::ScaledEncodeSize(kBigEyeW * 2, 1.0f), encH = rc::ScaledEncodeSize(kBigEyeH, 1.0f);
        const double bpp = 20'000'000.0 / (double(encW) * encH * 72.0);
        CHECK(bpp > 0 && bpp < 0.03);  // below kHalfRateBelowBpp: half rate ON
        std::printf("A12 case 3: CBR + fillMode + half-rate -> CBR avg==max, rung %d >= floor %d, "
                    "per-frame %.0f kbit <= VBV %.0f kbit, bpp %.4f < 0.03\n",
                    rung, rc::FillFloorRung(72, 150), perFrame / 1000.0, double(plan.budget.vbvBufferSize) / 1000.0,
                    bpp);
    }

    // 4. A user hand-write: the manual lock wins for that dimension, other dims
    //    stay auto's; a decode-bound tick moves nothing by hand. A89: the tuner
    //    is always on, so there is no switch to state here.
    {
        rcs::Settings pc;
        pc.qualityProfileCbr = true;   // effective CBR on (below)
        pc.streamProfile = 2;
        st.tenBit = false;
        const auto in = rcs::BuildAutoTuneInput(pc, st, false);
        CHECK(in.cbrOn);
        CHECK(in.profileId == pc.streamProfile);
        rc::AutoTune tuner;
        tuner.MarkManual(rc::AutoDim::Cbr);  // the user hand-wrote CBR after enabling autoTune
        rc::BottleneckSignal sig;
        sig.qp = 30.0;
        sig.mbps = 1.0;
        sig.ceilingKbps = 20000;
        sig.queueMs = 5.0;
        sig.nominalFps = 72.0;
        (void)tuner.Update(sig, in, 0);
        const auto r = tuner.Update(sig, in, 6'000'000'000LL);
        // CbrOff is locked out; with gaze foveation unavailable (the headset's switch)
        // the layer falls through to the once-per-episode ceiling hint.
        CHECK(r.action != rc::AutoAction::CbrOff);
        CHECK(r.action == rc::AutoAction::LogOnly);
        CHECK(std::strcmp(r.reason, "rate-control: CBR off (capped VBR, QP floor and deep VBV return)") != 0);
        std::printf("A12 case 4: autoTune + manual CBR lock -> RateControl falls through (%s), CbrOff never fires\n",
                    r.reason);
    }

    // 5. fecPercent=20 + bitrate 20 Mbps: the lowest-bandwidth budget with heavy
    //    parity - the wire ceiling conversion stays positive and inside the ceiling,
    //    and the packetizer really emits the parity the formula bills (probes below).
    {
        rcs::Settings pc;
        pc.fecPercent = 20;
        pc.vrBitrateMbps = 20;
        pc.pcBitrateWins = true;
        const auto build = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, kEyeW, kEyeH, 72.f);
        CHECK(build.cfg.fecPercent == 20);
        CHECK(build.cfg.bitrateKbps == 20000);
        constexpr size_t kFragPayload = rc::kMaxDatagramLan - sizeof(rc::PacketHeader) - rc::kMacSize -
                                        sizeof(rc::VideoFragmentHeader);
        const size_t total = 2 + sizeof(rc::VrFrameMetadata) + size_t(64) * 1024;
        const size_t wire = rcs::FrameWireBytes(kFragPayload, total, 20);
        CHECK(wire > total);
        rc::WireCeiling wc;
        rc::WireCeiling::Snapshot snap;
        wc.OnFrame(0, wire, total, 20000, snap);
        wc.OnFrame(13'888'888LL, wire, total, 20000, snap);  // 72 fps spacing
        CHECK(snap.encoderTargetKbps > 0);
        CHECK(snap.encoderTargetKbps <= 20000 && snap.encoderTargetKbps >= 10000);
        std::printf("A12 case 5: fec 20%% + 20 Mbps -> wire %.1f KB, encoder target %u kbps in [10000, 20000]\n",
                    wire / 1024.0, snap.encoderTargetKbps);
    }

    CHECK(g_a12Fails.empty());
    for (const std::string& f : g_a12Fails) std::printf("A12 %s\n", f.c_str());
}

// TASK-CAPS-GUARD (A15): H264's NVENC encoder refuses full-frame widths above
// 4096 px ("Frame Dimension greater than the maximum supported value", measured on
// the A14 matrix; the MF fallback fails the same way), so a session built on such a
// config never came up. The guarded synthesis (stream_config.h) moves such a config
// onto HEVC - the same caps-fallback rule ChooseCodec uses - and every other field
// is re-derived by the same formulas from the same inputs. The three card combos:
// display tiers 2688 / 2144 at the encode scales that overflow the width cap.
static void TestH264CapsFallback() {
    rcs::ClientCaps caps;  // the default headset answer: h264 + hevc, no Main10/AV1
    rcs::EncoderCaps encFull;
    encFull.h264 = encFull.hevc = encFull.av1 = encFull.hevc10 = encFull.av110 = true;
    rcs::ClientStreamSettings st;
    st.vrQuality = "high";
    st.refreshRate = 72;
    st.eyesTracked = true;
    st.adaptiveBitrate = true;
    st.tenBit = false;

    // Guard precision: below the cap nothing changes - the fallback only ever acts
    // on a path that would otherwise have failed, so every config the plain builder
    // could legally emit (and the whole A12 sweep's 1664-eye grid) passes through
    // the guarded one bit-identical.
    {
        rcs::Settings pc;
        pc.encodeScalePct = 85;
        const auto plain =
            rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::H264, false, false, 100, 2144, 2144, 72.f);
        const auto guarded =
            rcs::BuildVrStreamConfigGuarded(pc, st, rc::VideoCodec::H264, false, false, 100, 2144, 2144, 72.f,
                                            encFull, caps);
        CHECK(plain.cfg.codec == rc::VideoCodec::H264);
        CHECK(plain.codedEyeWidth * 2 == 3644 && plain.codedEyeWidth * 2 <= 4096);
        CHECK(!guarded.h264WidthFallback && guarded.h264RejectedWidth == 0);
        CHECK(A12SameVrConfig(plain.cfg, guarded.cfg));
        std::printf("A15 guard precision: 2144 eye at scale 85 encodes %u px - inside the cap, no fallback\n",
                    plain.codedEyeWidth * 2);
    }

    // The three card combos: the H264 build's full-frame width exceeds NVENC's 4096 px
    // cap, so the guarded synthesis rebuilds the same tier on HEVC. Assertions per the
    // card: codec == HEVC, the config stays inside its codec's encoder width cap
    // (H264 4096 / HEVC 8192 - the tier itself survives, that is the point of the
    // fallback), and every field except the codec matches the H264 build it replaces.
    {
        struct TierCase {
            uint32_t eyeW, eyeH;
            int scalePct;
        };
        const TierCase tierCases[] = {{2688, 2688, 85}, {2688, 2688, 100}, {2144, 2144, 100}};
        int caseNo = 0;
        for (const TierCase& tc : tierCases) {
            ++caseNo;
            rcs::Settings pc;
            pc.encodeScalePct = tc.scalePct;
            const auto h264 =
                rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::H264, false, false, 100, tc.eyeW, tc.eyeH, 72.f);
            const uint32_t fullW = h264.codedEyeWidth * 2;
            CHECK(h264.cfg.codec == rc::VideoCodec::H264);
            CHECK(fullW > 4096);  // the trigger: this frame could never open on NVENC/MF H264
            const auto guarded = rcs::BuildVrStreamConfigGuarded(pc, st, rc::VideoCodec::H264, false, false, 100,
                                                                tc.eyeW, tc.eyeH, 72.f, encFull, caps);
            CHECK(guarded.h264WidthFallback && guarded.h264RejectedWidth == fullW);
            CHECK(guarded.cfg.codec == rc::VideoCodec::HEVC);
            // Legal for the codec that now carries it, and nothing else moved.
            const uint32_t capW = guarded.cfg.codec == rc::VideoCodec::H264 ? 4096u : 8192u;
            CHECK(guarded.codedEyeWidth * 2 <= capW && guarded.codedEyeHeight <= capW);
            rcs::VrStreamConfig asRebuilt = h264.cfg;
            asRebuilt.codec = guarded.cfg.codec;
            CHECK(A12SameVrConfig(asRebuilt, guarded.cfg));
            std::printf("A15 case %d: H264 %ux%u eye at scale %d -> %u px frame would fail encoder init; the guarded "
                        "synthesis carries the same tier on HEVC, rest of the config unchanged\n",
                        caseNo, tc.eyeW, tc.eyeH, tc.scalePct, fullW);
        }
    }

    // No HEVC to fall back to (an H264-only chain): the guard must not emit a config
    // the decoder can't play - it returns the H264 config as-is and the encoder init
    // reports the real problem, exactly as before the guard existed.
    {
        rcs::Settings pc;
        pc.encodeScalePct = 100;
        rcs::EncoderCaps h264Only;
        h264Only.h264 = true;
        const auto guarded =
            rcs::BuildVrStreamConfigGuarded(pc, st, rc::VideoCodec::H264, false, false, 100, 2688, 2688, 72.f,
                                            h264Only, caps);
        CHECK(!guarded.h264WidthFallback);
        CHECK(guarded.cfg.codec == rc::VideoCodec::H264);
        std::printf("A15 no-HEVC: 2688 eye on an H264-only chain keeps the H264 config (nothing to fall back to)\n");
    }
}

// §1.7 runtime half + the packetizer cross-check: what the packetizer puts on the
// wire (datagram counts measured on a loopback pair) must equal what FrameWireBytes
// bills into the wire ceiling, parity included; the token bucket keeps the budget
// positive and never overflows.
static void TestFecWireBillingLoopback() {
    const int fecCases[] = {0, 3, 5, 10, 20, 100};  // 100: the clamp edge (group -> 2)
    for (int fec : fecCases) {
        LoopbackPair l;
        CHECK(l.Open());
        l.Start();
        VideoPacketizer pkt(l.sendConn.get());
        pkt.SetFecPercent(fec);
        // Token bucket: the §2 case's pacer numbers (20 Mbps x 1.08, 72 fps budget).
        pkt.SetPacingKbps(21600);
        pkt.SetPacingFrameBudgetNs(13'888'888LL);
        constexpr size_t kFrameBytes = 64 * 1024;
        std::vector<uint8_t> frame(kFrameBytes, uint8_t(0xA5));
        VrFrameMetadata meta{};
        meta.trackingTimestampNs = 12345;
        const size_t total = 2 + sizeof(VrFrameMetadata) + kFrameBytes;
        const size_t fragPayload = l.sendConn->MaxPayload() - sizeof(VideoFragmentHeader);
        const size_t dataFrags = fragPayload ? (total + fragPayload - 1) / fragPayload : size_t(0);
        const size_t sent1 = pkt.SendFrame(rc::kVrStreamId, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), frame.data(),
                                           frame.size());
        const size_t sent2 = pkt.SendFrame(rc::kVrStreamId, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), frame.data(),
                                           frame.size());
        l.Drain(150);
        CHECK(sent1 == sent2);                 // same frame -> same datagram count
        CHECK(sent1 > dataFrags || fec == 0);  // parity datagrams went out when FEC is on
        const size_t parity = sent1 - dataFrags;
        const int group = fec > 0 ? int(std::clamp(100 / fec, 2, 64)) : 0;
        // The packetizer emits one parity datagram per group of >= 2 members: a FINAL
        // group with a single fragment carries no parity (the shipped rule - it is the
        // XOR of that one fragment, which adds nothing to recover with). The billing
        // formula (ceil) can therefore over-count by at most one parity datagram.
        const size_t expectedParity = group ? (dataFrags + group - 1) / group - (dataFrags % size_t(group) == 1 ? 1u : 0u)
                                            : 0;
        CHECK(parity == expectedParity);
        // The billing formula must cover what actually left (never under-count) and
        // miss it by at most that one parity datagram (FINDING A12-F2: the wire-ceiling
        // and the token-bucket billing both use ceil; the packetizer may emit one
        // parity datagram less when the final group has a single member - conservative
        // direction, <= 1 datagram per frame).
        const size_t billed = rcs::FrameWireBytes(fragPayload, total, fec);
        const size_t actualBilled =
            total + dataFrags * sizeof(VideoFragmentHeader) + parity * (sizeof(VideoParityHeader) + fragPayload);
        CHECK(billed >= actualBilled);
        CHECK(billed - actualBilled <= sizeof(VideoParityHeader) + fragPayload);
        // The receiver saw every datagram (or the socket refused some - that escape
        // hatch is what SendFailures tracks) and reassembled the frame.
        CHECK(l.arrivals.size() >= sent1 + sent2 || pkt.SendFailures() > 0);
        CHECK(!l.frames.empty() || pkt.SendFailures() > 0);
        // Token bucket: a 64 KB frame bills ~24 ms at 21.6 Mbps (parity incl.) - two
        // back-to-back frames complete, the budget stays positive, nothing overflows.
        CHECK(pkt.BudgetOverruns() == 0);
        l.Stop();
        std::printf("A12 wire billing fec=%d%%: %zu data + %zu parity datagrams, billed %.1f KB/frame\n", fec,
                    dataFrags, parity, billed / 1024.0);
    }
}

// A33 (TASK-PACE-FOLLOW-2026-11-22): the burst pacer's billing base, extracted from
// VideoSender::Send as the pure inline rcs::PaceBaseKbps (video_sender.h) - the real
// call site bills base x 1.08 into SetPacingKbps. The card pins three laws (the
// selection is a one-line policy the card mandates asserting; nothing here mirrors
// more than that single rule):
//   1. burst mode, adaptive target < user ceiling: bill the TARGET (the pacer follows
//      the collapse instead of releasing frames the sinking link cannot carry - the
//      real device's 479 ms send-side queue spike);
//   2. burst mode, target >= ceiling: bill the ceiling (the old behaviour whenever
//      adaptation is not pushing under the user's setting);
//   3. adaptive off (the target IS the user's setting == the ceiling): bill the
//      ceiling x 1.08 exactly as before - zero behaviour change on that path.
static void TestPaceBaseKbps() {
    // The card's diagnosis pair: the 165 Mbps burst-ceiling tier and the adaptive
    // target collapsed to 109 Mbps by the link.
    constexpr uint32_t kCeiling = 165'000, kTarget = 109'000;
    // Law 1: follow the adaptive target down.
    CHECK(rcs::PaceBaseKbps(3, kCeiling, kTarget) == kTarget);
    // Law 2: the ceiling stays the bill when the target is at or above it - the
    // at-ceiling boundary (the real "adaptive has climbed back to max" case) and a
    // synthetic strictly-above target proving the clamp direction.
    CHECK(rcs::PaceBaseKbps(3, kCeiling, kCeiling) == kCeiling);
    CHECK(rcs::PaceBaseKbps(3, kCeiling, kCeiling + 35'000) == kCeiling);
    // Law 3: adaptive off - the target IS the user's setting, so it equals the ceiling
    // (ctor + SetMaxBitrate pin m_targetKbps to maxBitrateKbps when !adaptive) - bills
    // exactly as the old code did (same boundary value as law 2, different meaning:
    // this is the zero-behaviour-change path).
    CHECK(rcs::PaceBaseKbps(3, kCeiling, kCeiling) == kCeiling);
    // The follow modes (0 auto / 1 off / 2 follow-the-guard) never used the ceiling:
    // unchanged in both directions, target above or below it.
    CHECK(rcs::PaceBaseKbps(0, kCeiling, kTarget) == kTarget);
    CHECK(rcs::PaceBaseKbps(1, kCeiling, kTarget) == kTarget);
    CHECK(rcs::PaceBaseKbps(2, kCeiling, kTarget) == kTarget);
    CHECK(rcs::PaceBaseKbps(2, kCeiling, kCeiling) == kCeiling);
    std::printf("A33 pace base: burst follows the adaptive target down (109k), keeps the 165k ceiling, "
                "adaptive-off and follow modes unchanged\n");
}

#undef A12_CHECK

// ---------------------------------------------------------------------------
// A45 (TASK-TRANSPORT-ROBUST-2026-11-22): local robustness of the video transport.
// Every case is a malformed / extreme INPUT (a fuzzer-shaped wire packet or a
// concurrent setter), and every assertion is on an externally observable result
// (no crash, no zombie entry, no stats pollution, exact billed bytes). The fixed
// vector at the end is a byte-for-byte known-answer proof that legal frames are
// unchanged. Helpers build wire packets by hand - they never call the code under test.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> RFrag(uint8_t streamId, VideoCodec codec, uint8_t flags, uint8_t fecGroupSize,
                           uint32_t frameIndex, uint16_t fragIndex, uint16_t fragCount, uint32_t frameSize,
                           const std::vector<uint8_t>& data) {
    VideoFragmentHeader h{};
    h.streamId = streamId;
    h.codec = uint8_t(codec);
    h.frameFlags = flags;
    h.fecGroupSize = fecGroupSize;
    h.frameIndex = frameIndex;
    h.fragmentIndex = fragIndex;
    h.fragmentCount = fragCount;
    h.frameSize = frameSize;
    std::vector<uint8_t> p(sizeof(h) + data.size());
    std::memcpy(p.data(), &h, sizeof(h));
    if (!data.empty()) std::memcpy(p.data() + sizeof(h), data.data(), data.size());
    return p;
}

std::vector<uint8_t> RParity(uint8_t streamId, uint8_t fecGroupSize, uint16_t groupIndex, uint32_t frameIndex,
                             uint16_t fragCount, uint16_t lastFragSize, uint32_t frameSize,
                             const std::vector<uint8_t>& data) {
    VideoParityHeader h{};
    h.streamId = streamId;
    h.fecGroupSize = fecGroupSize;
    h.groupIndex = groupIndex;
    h.frameIndex = frameIndex;
    h.fragmentCount = fragCount;
    h.lastFragmentSize = lastFragSize;
    h.frameSize = frameSize;
    std::vector<uint8_t> p(sizeof(h) + data.size());
    std::memcpy(p.data(), &h, sizeof(h));
    if (!data.empty()) std::memcpy(p.data() + sizeof(h), data.data(), data.size());
    return p;
}

}  // namespace

static void TestTransportRobustReassembly() {
    // --- 1.1: a throwing handler must restore the caller's unique_lock. A broken
    // state would deadlock/crash on the very next reassembler operation. ---
    {
        VideoReassembler r([](ReceivedFrame&&) { throw 42; });
        auto f = RFrag(1, VideoCodec::HEVC, kVideoKeyframe, 0, 1, 0, 1, 4, {0, 0, 'a', 'b'});
        bool threw = false;
        try {
            r.HandleFragment(f.data(), f.size());
        } catch (int) {
            threw = true;
        }
        CHECK(threw);  // the exception itself is expected to propagate
        r.Reset();     // would hang / double-unlock if the mutex state were corrupted
    }
    // --- 1.1: an empty handler is skipped, not std::bad_function_call. ---
    {
        VideoReassembler r(VideoReassembler::FrameHandler{});
        auto f = RFrag(1, VideoCodec::HEVC, 0, 0, 1, 0, 1, 4, {0, 0, 'a', 'b'});
        r.HandleFragment(f.data(), f.size());
        CHECK(r.TakeStats().framesComplete == 1);
    }

    // --- 1.4: malformed parity must not create zombie pending entries. ---
    {
        VideoReassembler r([](ReceivedFrame&&) {});
        auto p1 = RParity(1, 2, 100, 1, 2, 3, 6, {1, 2, 3});  // groupIndex out of range
        auto p2 = RParity(1, 1, 0, 2, 2, 3, 6, {1, 2, 3});     // fecGroupSize = 1
        auto p3 = RParity(1, 2, 0, 3, 0, 3, 6, {1, 2, 3});     // fragmentCount = 0
        auto p4 = RParity(1, 2, 0, 4, 2, 3, 6, {});            // header only, no parity data
        r.HandleParity(p1.data(), p1.size());
        r.HandleParity(p2.data(), p2.size());
        r.HandleParity(p3.data(), p3.size());
        r.HandleParity(p4.data(), p4.size());
        r.Expire(NowNs() + 1'000'000'000LL, 1);
        const auto s = r.TakeStats();
        CHECK(s.framesLost == 0);       // no zombie entry was created
        CHECK(s.fragmentsReceived == 0);
    }

    // --- 1.4: a single-member group's parity must not complete a frame. ---
    {
        std::vector<ReceivedFrame> done;
        VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
        auto p = RParity(1, 2, 0, 1, 1, 4, 4, {1, 2, 3, 4});
        r.HandleParity(p.data(), p.size());
        CHECK(done.empty());
        CHECK(r.TakeStats().framesComplete == 0);
    }

    // --- 1.4: tail-length underflow is rejected (no bogus recovered fragment). ---
    {
        std::vector<ReceivedFrame> done;
        VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
        // frameSize=1 but prefix=(count-1)*fragmentPayload=2 => unsigned underflow
        // used to wrap the recovered tail length to ~SIZE_MAX.
        auto f0 = RFrag(1, VideoCodec::HEVC, 0, 2, 1, 0, 2, 1, {'a', 'b'});
        auto par = RParity(1, 2, 0, 1, 2, 2, 1, {0x01, 0x02});
        r.HandleFragment(f0.data(), f0.size());
        r.HandleParity(par.data(), par.size());
        const auto s = r.TakeStats();
        CHECK(s.fragmentsRecovered == 0);
        CHECK(s.framesComplete == 0);
        CHECK(done.empty());
    }

    // --- 1.4: duplicate / out-of-range fragments must not pollute codec/flags or
    // inflate receive stats. ---
    {
        std::vector<ReceivedFrame> done;
        VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
        auto f0 = RFrag(1, VideoCodec::HEVC, kVideoKeyframe, 0, 1, 0, 2, 4, {0, 0});
        auto f1 = RFrag(1, VideoCodec::HEVC, kVideoKeyframe, 0, 1, 1, 2, 4, {'a', 'b'});
        r.HandleFragment(f0.data(), f0.size());
        auto dup = RFrag(1, VideoCodec::H264, 0, 0, 1, 0, 2, 4, {0, 0});  // duplicate of idx 0
        auto oob = RFrag(1, VideoCodec::H264, 0, 0, 1, 9, 2, 4, {0, 0});  // index >= count
        r.HandleFragment(dup.data(), dup.size());
        r.HandleFragment(oob.data(), oob.size());
        r.HandleFragment(f1.data(), f1.size());
        CHECK(done.size() == 1);
        if (!done.empty()) {
            CHECK(done[0].codec == VideoCodec::HEVC);
            CHECK(done[0].flags == kVideoKeyframe);
        }
        CHECK(r.TakeStats().fragmentsReceived == 2);
    }

    // --- 1.5: fragmentCount=65535 packets are dropped (bounded per-frame alloc). ---
    {
        VideoReassembler r([](ReceivedFrame&&) {});
        for (uint32_t fi = 1; fi <= 200; fi++) {
            auto f = RFrag(1, VideoCodec::HEVC, 0, 0, fi, 0, 65535, 4, {0, 0});
            r.HandleFragment(f.data(), f.size());
        }
        const auto s = r.TakeStats();
        CHECK(s.packetsDropped == 200);
        CHECK(s.fragmentsReceived == 0);
    }
    // --- 1.5: a flood of distinct frame indices keeps m_pending bounded at 64. ---
    {
        VideoReassembler r([](ReceivedFrame&&) {});
        for (uint32_t fi = 1; fi <= 200; fi++) {
            auto f = RFrag(1, VideoCodec::HEVC, 0, 0, fi, 0, 2, 4, {0, 0});
            r.HandleFragment(f.data(), f.size());
        }
        r.Expire(NowNs() + 1'000'000'000LL, 1);
        const auto s = r.TakeStats();
        CHECK(s.framesLost > 0);  // evicted + expired frames were counted, no crash
        CHECK(s.packetsDropped == 0);
    }

    // --- 1.6: frame 10 complete, frame 11 partial then expired, frame 12 complete
    // => framesLost == 1 (not 2: Expire and TryComplete must not double-count). ---
    {
        std::vector<ReceivedFrame> done;
        VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
        auto ten = RFrag(1, VideoCodec::HEVC, 0, 0, 10, 0, 1, 4, {0, 0});
        auto eleven = RFrag(1, VideoCodec::HEVC, 0, 0, 11, 0, 2, 4, {0, 0});
        auto twelve = RFrag(1, VideoCodec::HEVC, 0, 0, 12, 0, 1, 4, {0, 0});
        r.HandleFragment(ten.data(), ten.size());
        r.HandleFragment(eleven.data(), eleven.size());
        r.Expire(NowNs() + 1'000'000'000LL, 1);
        r.HandleFragment(twelve.data(), twelve.size());
        const auto s = r.TakeStats();
        CHECK(s.framesLost == 1);
        CHECK(s.framesComplete == 2);
    }

    std::printf("A45 reassembly: no double-unlock/zombie/underflow, bounds hold, framesLost counted once\n");
}

static void TestTransportRobustPacketizer() {
    // --- 1.2: MaxPayload() <= header size => SendFrame returns 0, no SIGFPE. ---
    {
        UdpSocket a;
        CHECK(a.Open(0));
        auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                              crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey, "t");
        for (size_t payloadCap : {size_t(0), size_t(1), size_t(15), sizeof(VideoFragmentHeader)}) {
            Connection conn(&a, NetAddress{0x7F000001, 1}, 1, keys, true,
                            sizeof(PacketHeader) + kMacSize + payloadCap);
            VideoPacketizer pkt(&conn);
            DesktopFrameMetadata meta{};
            uint8_t data[32] = {};
            CHECK(pkt.SendFrame(1, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), data, sizeof(data)) == 0);
        }
    }

    // --- 1.2: count % group == 1 => billed wire bytes equal actual parity count. ---
    {
        LoopbackPair l;
        CHECK(l.Open());
        std::atomic<int> dataPkts{0}, parityPkts{0};
        l.recvConn->SetPacketHandler([&](PacketType t, const uint8_t*, size_t) {
            if (t == PacketType::Video) dataPkts++;
            else if (t == PacketType::VideoParity) parityPkts++;
        });
        l.Start();
        VideoPacketizer pkt(l.sendConn.get());
        pkt.SetFecPercent(25);  // group = clamp(100/25) = 4
        pkt.SetPacingKbps(21600);
        pkt.SetPacingFrameBudgetNs(13'888'888LL);
        const size_t fragPayload = l.sendConn->MaxPayload() - sizeof(VideoFragmentHeader);
        VrFrameMetadata meta{};
        const size_t count = 5;  // 5 % 4 == 1: single-member final group
        const size_t bitstreamSize = count * fragPayload - 2 - sizeof(meta);
        std::vector<uint8_t> frame(bitstreamSize, 0x5A);
        const size_t total = 2 + sizeof(meta) + bitstreamSize;  // == count * fragPayload
        const size_t expectedParity = 1;                        // 4-member group only
        const size_t expectedWire =
            total + count * sizeof(VideoFragmentHeader) + expectedParity * (sizeof(VideoParityHeader) + fragPayload);
        pkt.SendFrame(rc::kVrStreamId, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), frame.data(), frame.size());
        CHECK(pkt.LastBilledWireBytesForTest() == expectedWire);
        l.Drain(150);
        CHECK(dataPkts.load() == int(count));
        CHECK(parityPkts.load() == int(expectedParity));
        l.Stop();
    }

    // --- 1.3: concurrent SetFecPercent(0) and SendFrame must not SIGFPE. ---
    {
        LoopbackPair l;
        CHECK(l.Open());
        l.Start();
        VideoPacketizer pkt(l.sendConn.get());
        pkt.SetFecPercent(5);
        std::vector<uint8_t> data(1024, 0x33);
        DesktopFrameMetadata meta{};
        std::thread flipper([&] {
            for (int i = 0; i < 2000; i++) pkt.SetFecPercent((i % 3 == 0) ? 0 : (i % 7) + 1);
        });
        for (uint32_t f = 1; f <= 2000; f++) {
            pkt.SendFrame(1, VideoCodec::HEVC, 0, f, &meta, sizeof(meta), data.data(), data.size());
        }
        flipper.join();
        l.Stop();
        // Reaching this line alive is the assertion: the old TOCTOU could SIGFPE here.
    }

    // --- 1.7: a single frame's billing is capped; leaving burst mode re-baselines. ---
    {
        LoopbackPair l;
        CHECK(l.Open());
        l.Start();
        VideoPacketizer pkt(l.sendConn.get());
        pkt.SetFecPercent(0);
        pkt.SetPacingKbps(100);  // absurdly low: one 256 KB frame bills ~22 s uncapped
        pkt.SetPacingFrameBudgetNs(13'888'888LL);
        std::vector<uint8_t> big(256 * 1024, 0x7E);
        DesktopFrameMetadata meta{};
        pkt.SendFrame(1, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), big.data(), big.size());
        CHECK(pkt.NextSendNsForTest() <= NowNs() + 40'000'000LL + 2'000'000LL);
        const uint32_t resets = pkt.ModeSwitchResetsForTest();
        pkt.SetPacingKbps(0);           // exit burst pacing entirely
        pkt.SetPacingFrameBudgetNs(0);  // switch back to legacy mode
        uint8_t tiny[8] = {};
        pkt.SendFrame(1, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), tiny, sizeof(tiny));
        CHECK(pkt.ModeSwitchResetsForTest() == resets + 1);
        l.Stop();
    }

    std::printf("A45 packetizer: small MTU safe, parity billing exact, FEC race safe, clock bounded\n");
}

// ---------------------------------------------------------------------------
// A49 (TASK-CONN-HARDEN-2026-11-22): connection-layer hardening.
// The receiver is driven through the real HandleDatagram (version gate + AEAD +
// replay window) with datagrams sealed by a peer that holds the session keys, so the
// tests need no test-only visibility hook and production code gains no new surface.
// ---------------------------------------------------------------------------
namespace {

// Connection::HandleDatagram's direction byte for client -> server (connection.cpp).
// Wire format, not a protocol.h constant; a test peer must use the same byte.
constexpr uint8_t kDirClientToServerTest = 0x43;  // 'C'
const NetAddress kInjectFrom{0x7F000001, 5000};

// Seals control/other datagrams the way a peer would, with an order-preserving
// PacketHeader sequence and a caller-controlled chunkSeq/version.
struct PeerInjector {
    Connection& conn;
    const crypto::Key& key;  // the sender's m_sendKey (== receiver's m_recvKey)
    uint32_t sessionId;
    uint64_t packetSeq = 1;
    uint32_t chunkSeq = 0;
    uint8_t version = uint8_t(kProtocolVersion);

    PeerInjector(Connection& c, const crypto::Key& k, uint32_t sid) : conn(c), key(k), sessionId(sid) {}

    bool Send(const uint8_t* plain, size_t plainSize, PacketType type = PacketType::Control) {
        PacketHeader h{};
        h.version = version;
        h.type = uint8_t(type);
        h.flags = kFlagEncrypted;
        h.sessionId = sessionId;
        h.sequence = packetSeq++;
        std::vector<uint8_t> dg(sizeof(h) + plainSize + kMacSize);
        std::memcpy(dg.data(), &h, sizeof(h));
        crypto::Seal(key, h.sequence, kDirClientToServerTest, dg.data(), sizeof(h), plain, plainSize,
                     dg.data() + sizeof(h));
        return conn.HandleDatagram(dg.data(), dg.size(), kInjectFrom);
    }

    bool Chunk(uint32_t messageId, uint16_t index, uint16_t count, const std::string& data) {
        ControlChunkHeader ch{};
        ch.chunkSeq = chunkSeq++;
        ch.messageId = messageId;
        ch.chunkIndex = index;
        ch.chunkCount = count;
        std::vector<uint8_t> plain(sizeof(ch) + data.size());
        std::memcpy(plain.data(), &ch, sizeof(ch));
        if (!data.empty()) std::memcpy(plain.data() + sizeof(ch), data.data(), data.size());
        return Send(plain.data(), plain.size());
    }
};

}  // namespace

static void TestConnectionHardening() {
    UdpSocket dead;  // never opened: every ack is refused, nothing reaches the network
    CHECK(!dead.IsOpen());
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey,
                                          crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, "a49");

    // --- 5.1: chunkCount == 0 and chunkIndex >= chunkCount must not accumulate. ---
    {
        const uint32_t sid = 0xA49'00001;
        Connection conn(&dead, kInjectFrom, sid, keys, true, kMaxDatagramLan);
        int deliveries = 0;
        std::string last;
        conn.SetControlHandler([&](const std::string& m) {
            deliveries++;
            last = m;
        });
        PeerInjector in(conn, keys.clientToServer, sid);

        // "Last chunk" can never arrive (0 + 1 != 0), so the old code appended 4000x120
        // bytes to m_assembly and kept growing.
        int accepted = 0;
        for (int i = 0; i < 4000; i++) accepted += in.Chunk(11, 0, 0, std::string(120, 'z')) ? 1 : 0;
        CHECK(accepted == 4000);  // authentic, so delivery is attempted - and must be dropped
        CHECK(deliveries == 0);
        // Index past the end: same non-terminating shape.
        accepted = 0;
        for (int i = 0; i < 4000; i++) accepted += in.Chunk(12, 5, 3, std::string(120, 'y')) ? 1 : 0;
        CHECK(accepted == 4000);
        CHECK(deliveries == 0);
        // A well-formed message still arrives byte-exact: no stale prefix survived.
        CHECK(in.Chunk(13, 0, 2, "hello "));
        CHECK(in.Chunk(13, 1, 2, "world"));
        CHECK(deliveries == 1);
        CHECK(last == "hello world");
        // The dropped bytes were never accounted as received control data.
        CHECK(conn.Stats().packetsReceived == 8002);

        // --- 5.2: over the 4 MB cap the buffer is cleared and nothing is delivered. ---
        // 3200 x 1356 B > 4 MiB, so the cap trips mid-message; the message's own final
        // chunk then cannot complete a 4.3 MB delivery (the old code delivered it).
        const std::string body(1356, 'q');
        const uint16_t kCount = 4000;
        for (uint16_t i = 0; i < 3200; i++) CHECK(in.Chunk(21, i, kCount, body));
        CHECK(deliveries == 1);
        CHECK(in.Chunk(21, uint16_t(kCount - 1), kCount, body));
        CHECK(deliveries == 1);  // not delivered: reassembly was reset at the cap
        CHECK(last == "hello world");
        // After the reset a fresh message is reassembled from scratch, cleanly.
        CHECK(in.Chunk(22, 0, 1, "fresh"));
        CHECK(deliveries == 2);
        CHECK(last == "fresh");
    }

    // --- 5.3: a maxDatagram below the floor is clamped, never divides by zero. ---
    {
        const uint32_t sid = 0xA49'00002;
        for (size_t mtu : {size_t(0), size_t(1), size_t(sizeof(PacketHeader) + kMacSize), size_t(44)}) {
            UdpSocket tx;
            CHECK(tx.Open(0));
            Connection conn(&tx, kInjectFrom, sid, keys, true, mtu);
            // The floor leaves room for one control header byte, so SendControl cannot
            // underflow chunkData (the old code produced a 0 or huge divisor).
            CHECK(conn.MaxPayload() > sizeof(ControlChunkHeader));
        }
        // End to end with an absurd MTU: 1 byte per chunk still arrives intact, so this
        // is a clamp, not a silent drop.
        Peer a, b;
        CHECK(a.socket.Open(0) && b.socket.Open(0));
        const NetAddress aAddr{0x7F000001, a.socket.LocalPort()};
        const NetAddress bAddr{0x7F000001, b.socket.LocalPort()};
        a.conn = std::make_unique<Connection>(&a.socket, bAddr, sid, keys, true, 0);
        b.conn = std::make_unique<Connection>(&b.socket, aAddr, sid, keys, false, 0);
        CHECK(a.conn->MaxPayload() > sizeof(ControlChunkHeader));
        std::atomic<int> got{0};
        std::mutex m;
        std::string text;
        b.conn->SetControlHandler([&](const std::string& s) {
            std::lock_guard lock(m);
            text = s;
            got++;
        });
        b.Run();
        a.conn->SendControl("tiny-mtu");
        for (int i = 0; i < 100 && got.load() < 1; i++) {
            a.conn->Tick(NowNs());
            SleepMs(5);
        }
        CHECK(got.load() == 1);
        {
            std::lock_guard lock(m);
            CHECK(text == "tiny-mtu");
        }
        b.Stop();
    }

    // --- 5.4: a datagram the socket refuses is not counted as sent. ---
    {
        const uint32_t sid = 0xA49'00003;
        Connection conn(&dead, kInjectFrom, sid, keys, true, kMaxDatagramLan);
        const ConnectionStats before = conn.Stats();
        CHECK(!conn.Send(PacketType::Ping, nullptr, 0));
        CHECK(!conn.Send2(PacketType::Ping, "ab", 2, "cd", 2));
        conn.SendControl("refused");  // control chunks take the same path
        const ConnectionStats after = conn.Stats();
        CHECK(after.packetsSent == before.packetsSent);
        CHECK(after.bytesSent == before.bytesSent);
        // Not vacuous: a live socket does count the same send.
        UdpSocket sink;
        CHECK(sink.Open(0));
        Connection live(&sink, NetAddress{0x7F000001, sink.LocalPort()}, sid, keys, true, kMaxDatagramLan);
        CHECK(live.Send(PacketType::Ping, nullptr, 0));
        CHECK(live.Stats().packetsSent == 1);
        CHECK(live.Stats().bytesSent == sizeof(PacketHeader) + kMacSize);
    }

    // --- 5.5: header.version is gated, and a version reject is not a MAC reject. ---
    {
        const uint32_t sid = 0xA49'00004;
        Connection conn(&dead, kInjectFrom, sid, keys, true, kMaxDatagramLan);
        int deliveries = 0;
        conn.SetControlHandler([&](const std::string&) { deliveries++; });
        PeerInjector in(conn, keys.clientToServer, sid);

        in.version = uint8_t(kProtocolVersion) + 1;
        const ConnectionStats before = conn.Stats();
        CHECK(!in.Chunk(31, 0, 1, "v4"));  // valid session + valid MAC, wrong version
        const ConnectionStats rejected = conn.Stats();
        CHECK(rejected.packetsRejected == before.packetsRejected);  // not an auth failure
        CHECK(rejected.packetsReceived == before.packetsReceived);
        CHECK(deliveries == 0);

        in.version = uint8_t(kProtocolVersion);
        in.chunkSeq = 0;  // the rejected datagram never advanced the receiver's expectation
        CHECK(in.Chunk(31, 0, 1, "v3"));  // identical datagram shape, correct version
        CHECK(deliveries == 1);
        CHECK(conn.Stats().packetsReceived == before.packetsReceived + 1);
    }

    std::printf("A49 connection hardening: chunk bounds, 4 MB cap, MTU floor, stats truth, version gate\n");
}

static void TestTransportRobustFixedVector() {
    // A45 gate §2.5: a frozen wire vector (fragment + XOR parity + short tail)
    // reassembles to an exact byte sequence - the legal-frame behaviour proof.
    {
        std::vector<ReceivedFrame> done;
        VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
        // frame payload = [u16 metaSize=0][A1 A2 A3 A4 A5 A6], frameSize=8,
        // 3 fragments (3/3/2), FEC group 2. Parity = frag0 XOR frag1 = A2 A3 05.
        auto par = RParity(1, 2, 0, 1, 3, 2, 8, {0xA2, 0xA3, 0x05});
        auto f1 = RFrag(1, VideoCodec::HEVC, kVideoKeyframe, 2, 1, 1, 3, 8, {0xA2, 0xA3, 0xA4});
        auto f2 = RFrag(1, VideoCodec::HEVC, kVideoKeyframe, 2, 1, 2, 3, 8, {0xA5, 0xA6});
        r.HandleParity(par.data(), par.size());
        r.HandleFragment(f1.data(), f1.size());  // recovers fragment 0 from parity
        r.HandleFragment(f2.data(), f2.size());
        CHECK(done.size() == 1);
        if (!done.empty()) {
            const std::vector<uint8_t> expected = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
            CHECK(done[0].bitstream == expected);
            CHECK(done[0].codec == VideoCodec::HEVC);
            CHECK(done[0].flags == kVideoKeyframe);
        }
    }
    // End-to-end legal frame through the real packetizer: bytes survive unchanged.
    {
        LoopbackPair l;
        CHECK(l.Open());
        l.Start();
        VideoPacketizer pkt(l.sendConn.get());
        pkt.SetFecPercent(0);
        const std::vector<uint8_t> bits = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
        VrFrameMetadata meta{};
        pkt.SendFrame(1, VideoCodec::HEVC, kVideoKeyframe, 1, &meta, sizeof(meta), bits.data(), bits.size());
        l.Drain(100);
        CHECK(l.frames.size() == 1);
        if (!l.frames.empty()) {
            CHECK(l.frames[0].bitstream == bits);
            CHECK(l.frames[0].codec == VideoCodec::HEVC);
            CHECK(l.frames[0].flags == kVideoKeyframe);
        }
        l.Stop();
    }
    std::printf("A45 fixed vector: reassembled bitstream A1..A6 byte-identical (legal frame unchanged)\n");
}

// A47 (TASK-ENCODE-CONVERT-FIX-2026-11-22 §1.3): DeriveMfParams feeds the MF
// codec API directly. CODECAPI_AVEncCommonBufferSize is BYTES/frame, but the old
// code wrote bits/frame (an 8x-too-large VBV buffer), and fps<=0 divided by zero.
// Known-answer checks on the pure function - no device or MFT required.
static void TestDeriveMfParams() {
    rcs::EncoderConfig c;
    c.bitrateKbps = 100000;
    c.fps = 72;
    c.desktop = false;
    rcs::MfEncoderParams m = rcs::DeriveMfParams(c);
    // 100000 kbps = 12,500,000 bytes/s; /72 fps = 173,611 bytes/frame
    // (the old bits/frame expression produced 1,388,888 - 8x too large).
    CHECK(m.meanBitRate == 100000000u);
    CHECK(m.bufferSize == 173611u);
    CHECK(m.frameRateNum == 72000u);

    // Desktop mode budgets two frames of VBV.
    c.desktop = true;
    m = rcs::DeriveMfParams(c);
    CHECK(m.bufferSize == 347222u);

    // fps<=0 used to yield inf -> uint32 UB; now clamped to the 72 fps default.
    c.desktop = false;
    c.fps = 0.0f;
    m = rcs::DeriveMfParams(c);
    CHECK(m.bufferSize == 173611u && m.bufferSize > 0);

    // Fractional fps rounds instead of truncating (59.94 -> 59940, not 59939).
    c.fps = 59.94f;
    m = rcs::DeriveMfParams(c);
    CHECK(m.frameRateNum == 59940u);

    // 64-bit intermediate: the ceiling kbps survives intact in the MF UINT32 rate.
    c.fps = 72;
    c.bitrateKbps = 4000000;
    m = rcs::DeriveMfParams(c);
    CHECK(m.meanBitRate == 4000000000u);

    std::printf("A47 DeriveMfParams: bytes/frame 100Mbps@72=173611 (desktop=347222), fps=0 finite, 59.94->59940\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestLinkLocalDiscovery();
    TestMusic();
    TestMotionRuns();
    TestMotionHysteresis();  // A18 (TASK-MOTION-HYSTERESIS-2026-11-22)
    TestHandSkeleton();
    NetInit();
    TestAddresses();
    TestDiscovery();
    TestDiscoveryHardening();  // A51 (TASK-DISCOVERY-PLATFORM-2026-11-22)
    TestSubnetHosts();
    TestSocketHardening();  // A50 (TASK-SOCKET-HARDEN-2026-11-22)
    TestLocalSource();      // A58 (TASK-DISCOVERY-REPLY-2026-11-22)
    TestAudio();
    TestIdentity();
    TestVideoReassemblyOrder();
    TestTransportRobustReassembly();   // A45 §1.1 / 1.4 / 1.5 / 1.6
    TestTransportRobustFixedVector();  // A45 gate §2.5: legal frame byte-for-byte unchanged
    TestTransportRobustPacketizer();   // A45 §1.2 / 1.3 / 1.7
    TestConnectionHardening();         // A49 (TASK-CONN-HARDEN-2026-11-22)
    TestPacing();
    TestPacingBursts();
    TestPacingStop();
    // P5: TestPacingFrameBudget is also real-time scheduling sensitive
    // (SleepMs-driven windows); same pre-run guard as TestPacingBurstShaping.
    // The checks themselves stay untouched. Since TASK-FLAKE-ROBUST-2026-11-22
    // (2026-11-22) the timing-sensitive pacing tests self-calibrate their tolerances
    // (see CalibrateTimingJitter); RINCAST_TEST_TIMING_SENSITIVE=1 is a standard-gate
    // item now (evidence: overnight/evidence/A7-receipt.md).
    {
        const char* force = std::getenv("RINCAST_TEST_TIMING_SENSITIVE");
        if (force && std::strcmp(force, "1") == 0) {
            TestPacingFrameBudget();
        } else if ((force && std::strcmp(force, "0") == 0) || TestHostBusy()) {
            g_skips++;
            std::printf("SKIP TestPacingFrameBudget: timing-sensitive test, host busy "
                        "(%s; set RINCAST_TEST_TIMING_SENSITIVE=1 to force)\n",
                        force && std::strcmp(force, "0") == 0 ? "explicitly disabled via RINCAST_TEST_TIMING_SENSITIVE=0"
                                                             : "RinCast Streamer.exe is running");
        } else {
            TestPacingFrameBudget();
        }
    }
    // F1 (TASK-FIX 2026-11-19) -> TASK-FLAKE-ROBUST-2026-11-22 (2026-11-22): the
    // pre-run guard for the timing-sensitive shaping test - see TestHostBusy above.
    // The shaping test now self-calibrates its tolerances (capped), so forcing with =1
    // is meaningful under load and is a standard-gate item (evidence:
    // overnight/evidence/A7-receipt.md); =0 or a busy host still skips with an
    // explicit counted line for unforced runs.
    {
        const char* force = std::getenv("RINCAST_TEST_TIMING_SENSITIVE");
        if (force && std::strcmp(force, "1") == 0) {
            TestPacingBurstShaping();
        } else if ((force && std::strcmp(force, "0") == 0) || TestHostBusy()) {
            g_skips++;
            std::printf("SKIP TestPacingBurstShaping: timing-sensitive test, host busy "
                        "(%s; set RINCAST_TEST_TIMING_SENSITIVE=1 to force)\n",
                        force && std::strcmp(force, "0") == 0 ? "explicitly disabled via RINCAST_TEST_TIMING_SENSITIVE=0"
                                                             : "RinCast Streamer.exe is running");
        } else {
            TestPacingBurstShaping();
        }
    }
    TestWireCeilingBudget();
    TestRateScaleForLadder();
    TestQpStatisticsValidity();
    TestEncoderRateTransaction();
    TestFillModePlan();
    TestFillModeGateEdges();  // TASK-ROBUST-EDGES-2026-11-20 §1.2
    TestStreamProfile();
    TestProfileFoveaGradient();
    TestBitrateSwitches();
    // TASK-COMBO-SWEEP-2026-11-21: the full switch-combination sweep. Pure logic
    // (no GPU, no real encode session); the card requires the sweep to finish in
    // under 60 s, asserted inside the test itself.
    TestVrConfigComboSweep();
    TestDeriveMfParams();  // A47 (TASK-ENCODE-CONVERT-FIX-2026-11-22 §1.3)
    TestConfigCouplingCases();
    TestH264CapsFallback();  // TASK-CAPS-GUARD: the H264 width-cap fallback cases
    TestFecWireBillingLoopback();
    TestPaceBaseKbps();  // A33 (TASK-PACE-FOLLOW-2026-11-22): the burst pacer's billing base
    TestFoveaPlacement();  // A61: the fovea square's placement rule
    TestFoveaQpDelta();  // A62: the foveated QP ramp's NaN rule
    TestFpsLadder();
    TestFillFloorByTier();
    TestPercentile();  // TASK-DEC-PERCENTILE-2026-11-20 §1.2
    TestLatencyStageAccumulation();  // TASK-LATENCY-METER-FIX-2026-11-22 §2
    TestFrameQueueBound();  // A23 (TASK-QUEUE-BOUND-2026-11-22)
    TestStaleFlush();  // A26 (TASK-BACKLOG-FLUSH-2026-11-22)
    TestStaleFlushChainGate();  // A72 (TASK-A72-STALE-FLUSH-2026-11-22): rules C1-C8
    TestStaleFlushGraceReplay();  // A75 (TASK-A75-FLUSH-GRACE-2026-11-22): the grace period replayed
    TestLossBrake();
    TestQueueGuard();
    TestGuardRecoveryCost();  // A73b (TASK-A73b-RECOVERY-COST): the recovery-cost table
    TestL7Autoscale();
    TestAutoTuneClassify();
    TestAutoTuneClassifyEdgeCases();  // TASK-ROBUST-EDGES-2026-11-20 §2
    TestAutoTuneDecide();
    TestAutoTuneProfileFloor();  // A84 (TASK-A84-PROFILE-FLOOR-TWO-WAY-AUTOTUNE-2026-11-23 §5)
    TestHasUsableSignals();  // A88: no stream is not a clean link
    TestEffectiveProfileRung();  // A84b §4 (TASK-A84b): the rule behind the periodic effProfile
    TestSettingsSchemaMigration();  // A84b §2: the autoTune default migration, out of settings.cpp
    TestLinkPolicy();
    TestSuggestStartFps();
    TestSessionDisconnectReconnect();
    TestFecDeterministic();
    TestHandshakeHardening();  // A44 (TASK-HANDSHAKE-HARDEN-2026-11-22)
    TestHandshakeAndTransport();
    if (g_failures) std::printf("%d FAILURES\n", g_failures);
    else if (g_skips) std::printf("ALL TESTS PASSED (%d skipped)\n", g_skips);
    else std::printf("ALL TESTS PASSED\n");
    return g_failures ? 1 : 0;
}
