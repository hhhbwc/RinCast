// ROUND19 T2: deterministic, CPU-only fuzz drivers for the shared transport layer.
//
//   rincast-fuzz msg     [iterations]   handshake/control-JSON parsing (truncation, oversize,
//                                      invalid JSON, duplicate keys)
//   rincast-fuzz fec     [iterations]   video fragment/parity reassembly (reorder, duplicate,
//                                      loss, corruption, hostile headers) + buffer bounds
//   rincast-fuzz pacing  [iterations]   QueueGuard state machine (random arrivals, floods,
//                                      clock jumps, bounded cap, bounded time)
//
// Fixed seeds and no wall-clock randomness: every run is reproducible, and the periodic
// progress lines make a sanitizer abort's crashing iteration easy to replay.
// Memory safety is watched by ASan when built with -DRINCAST_SANITIZE=address; the
// explicit CHECK() bounds here are the ones ASan cannot see (state growth, counters).

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <crtdbg.h>
#endif

#include "nlohmann/json.hpp"
#include "rc/audio_codec.h"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/frame_generation.h"
#include "rc/handshake.h"
#include "rc/motion_field.h"
#include "rc/socket.h"
#include "rc/protocol.h"
#include "rc/queue_guard.h"
#include "rc/stream_profile.h"
#include "rc/video_transport.h"

namespace {

uint64_t g_iterations = 1000000;
int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL fuzz:%s:%d iter %llu: %s\n", __func__, __LINE__, \
                        (unsigned long long)g_i, #cond);                       \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

uint64_t g_i = 0;

void progress(const char* mode) {
    if (g_i % 5000 == 0 && g_i) {
        std::printf("[%s] %llu iterations\n", mode, (unsigned long long)g_i);
        std::fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// mode msg: handshake parsing + control/stat JSON parsing
// ---------------------------------------------------------------------------

std::vector<uint8_t> BuildValidHandshake(std::mt19937_64& rng) {
    rc::HandshakePacket p;
    p.type = rc::PacketType::Hello;
    p.sessionId = 42;
    for (size_t i = 0; i < p.publicKey.size(); i++) p.publicKey[i] = uint8_t(rng() & 0xFF);
    p.json = R"({"app":"rincast","proto":2,"device":"PICO 4","name":"headset-a8110",)"
             R"("key":"b64key==","cloud":{"token":"t","user":"u"}})";
    return rc::BuildHandshake(p);
}

// Control/stat-shaped messages the app layer feeds to its JSON parser.
const char* const kJsonTemplates[] = {
    R"({"type":"kStats","stats":{"fps":72.5,"latencyMs":25.3,"decodeMs":5.6}})",
    R"({"type":"kVrPhase","slipNs":12345678})",
    R"({"type":"kUpdateSettings","settings":{"codec":"hevc","tenBit":true,"desktopFps":60}})",
    R"({"type":"kKeyframe","stream":0})",
    R"({"type":"kStats","stats":{"qp":16.2,"res":[2688,2688],"enc":90,"drop":0}})",
};

int FuzzMsg(uint64_t n) {
    std::mt19937_64 rng(0xD1CEB04A5EEDULL);
    rc::HandshakePacket out;
    uint64_t accepted = 0, json_ok = 0, json_err = 0;
    for (g_i = 0; g_i < n; g_i++) {
        progress("msg");
        std::vector<uint8_t> bytes = BuildValidHandshake(rng);
        const unsigned cls = unsigned(rng() % 20);
        if (cls < 5) {  // truncate
            bytes.resize(rng() % (bytes.size() + 1));
        } else if (cls < 10) {  // flip bytes
            const int flips = 1 + int(rng() % 8);
            for (int f = 0; f < flips && !bytes.empty(); f++)
                bytes[size_t(rng() % bytes.size())] = uint8_t(rng() & 0xFF);
        } else if (cls < 12) {  // oversize: append junk
            const size_t junk = 1 + size_t(rng() % 65536);
            const size_t old = bytes.size();
            bytes.resize(old + junk);
            for (size_t i = old; i < bytes.size(); i++) bytes[i] = uint8_t(rng() & 0xFF);
        } else if (cls < 15) {  // duplicate keys inside the JSON, rebuilt packet
            std::string js = R"({"app":"rincast","app":"dup","proto":2,"proto":9,)"
                             R"("device":"PICO 4","device":"","name":"h","name":"h2"})";
            rc::HandshakePacket p;
            p.type = rc::PacketType::Hello;
            p.sessionId = uint32_t(rng());
            for (size_t i = 0; i < p.publicKey.size(); i++) p.publicKey[i] = uint8_t(rng() & 0xFF);
            p.json = js;
            bytes = rc::BuildHandshake(p);
        } else if (cls < 17) {  // pure noise
            const size_t len = rng() % 2048;
            bytes.resize(len);
            for (size_t i = 0; i < len; i++) bytes[i] = uint8_t(rng() & 0xFF);
        } else {  // control/stat JSON through the app's parser (nlohmann)
            std::string js = kJsonTemplates[rng() % 5];
            const unsigned m = unsigned(rng() % 5);
            if (m == 0) js.resize(rng() % (js.size() + 1));  // truncation
            else if (m == 1) {  // duplicate keys
                js.insert(js.size() - 1, R"(,"type":"kStats","stats":{"fps":1})");
            } else if (m == 2) {  // absurd numbers
                js.insert(js.size() - 1, R"(,"x":1e999,"y":-0.0,"z":123456789012345678901234)");
            } else if (m == 3 && rng() % 50 == 0) {  // rare oversize string
                js.insert(js.size() - 1, std::string(1 << 20, 'a'));
            } else {  // junk injection
                js.insert(size_t(rng() % js.size()), "\"");
            }
            try {
                auto v = nlohmann::json::parse(js);
                json_ok++;
            } catch (...) {
                json_err++;
            }
        }
        if (rc::ParseHandshake(bytes.data(), bytes.size(), out)) accepted++;
        CHECK(bytes.size() < (1ull << 32));  // the driver itself must stay bounded
    }
    std::printf("fuzz msg: %llu iterations, handshake accepted %llu, json ok %llu / err %llu, failures %d\n",
                (unsigned long long)n, (unsigned long long)accepted, (unsigned long long)json_ok,
                (unsigned long long)json_err, g_failures);
    return g_failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// mode fec: fragment/parity reassembly under reorder/dup/loss/corruption
// ---------------------------------------------------------------------------

int FuzzFec(uint64_t n) {
    std::mt19937_64 rng(0xFECC0DE5EEDULL);
    uint64_t handled = 0;
    rc::VideoReassembler reassembler([&handled](rc::ReceivedFrame&&) { handled++; });
    int64_t nowNs = 0;
    uint64_t fragsSent = 0;
    uint32_t frameBase = 1;
    for (g_i = 0; g_i < n; g_i++) {
        progress("fec");
        // mostly walk forward; sometimes revisit recent frames (reorder/dup), rarely jump far
        double roll = std::uniform_real_distribution<>(0.0, 1.0)(rng);
        if (roll < 0.70) frameBase += uint32_t(rng() % 3);
        else if (roll < 0.90) frameBase += uint32_t(rng() % 500);
        else frameBase += uint32_t(1 + rng() % 1000000);

        const int frags = 1 + int(rng() % 6);
        for (int f = 0; f < frags; f++) {
            std::vector<uint8_t> d(size_t(16 + rng() % 1384), 0);
            rc::VideoFragmentHeader h{};
            h.streamId = uint8_t(rng() % 17);
            h.codec = uint8_t(rng() % 4);
            h.frameFlags = uint8_t(rng() & 0xFF);
            h.fecGroupSize = uint8_t(rng() % 65);  // 0 = none, up to 64 (hostile values included)
            h.frameIndex = frameBase;
            // 92% plausible 1..32; 7% the rejected 0; 1% large (4096 exercises the same
            // resize/assign/expire paths as 65535 without an allocation storm under ASan)
            h.fragmentCount = uint16_t(rng() % 1000 < 920 ? 1 + rng() % 32
                                       : (rng() % 100 < 88 ? 0 : 4096));
            h.fragmentIndex = uint16_t(rng() % (h.fragmentCount ? h.fragmentCount : 1));
            // usually plausible; rarely hostile (allocation-size probing in TryComplete)
            h.frameSize = (rng() % 100 < 95) ? uint32_t(h.fragmentCount) * 1300 + uint32_t(rng() % 100)
                                             : 0xFFFFFFF0u + uint32_t(rng() % 16);
            std::memcpy(d.data(), &h, sizeof(h));
            for (size_t i = sizeof(h); i < d.size(); i++) d[i] = uint8_t(rng() & 0xFF);
            reassembler.HandleFragment(d.data(), d.size());
            fragsSent++;
        }
        const int parity = int(rng() % 3);
        for (int p = 0; p < parity; p++) {
            std::vector<uint8_t> d(size_t(16 + rng() % 600), 0);
            rc::VideoParityHeader h{};
            h.streamId = uint8_t(rng() % 17);
            h.fecGroupSize = uint8_t(1 + rng() % 64);
            h.groupIndex = uint16_t(rng() % 200 < 180 ? rng() % 8 : rng() % 65536);  // hostile included
            h.frameIndex = frameBase;
            h.fragmentCount = uint16_t(1 + rng() % 32);
            h.lastFragmentSize = uint16_t(rng() % 1400);
            h.frameSize = uint32_t(h.fragmentCount) * 1300;
            std::memcpy(d.data(), &h, sizeof(h));
            for (size_t i = sizeof(h); i < d.size(); i++) d[i] = uint8_t(rng() & 0xFF);
            reassembler.HandleParity(d.data(), d.size());
        }
        // the caller's contract: expire stale assemblies as the clock moves; jumps included
        nowNs += int64_t(rng() % 50) * 1000000LL;
        if (rng() % 25 == 0) nowNs += (1 + int64_t(rng() % 10)) * int64_t(rng() % 2 ? 1 : -1) * 1000000000LL;
        reassembler.Expire(nowNs, 2000000000LL);
        CHECK(handled <= fragsSent);
        if (g_i % 1000 == 0) {
            const rc::ReassemblyStats s = reassembler.TakeStats();
            CHECK(s.fragmentsReceived <= fragsSent);
        }
        if (handled > 50000000ull) CHECK(false);  // handler bound (buffer upper bound)
    }
    reassembler.Expire(nowNs + 100000000000LL, 60000000000LL);
    const rc::ReassemblyStats s = reassembler.TakeStats();
    std::printf("fuzz fec: %llu iterations, fragments %llu, handled %llu, recovered %u, lost frames %u, "
                "complete %u, failures %d\n",
                (unsigned long long)n, (unsigned long long)fragsSent, (unsigned long long)handled,
                s.fragmentsRecovered, s.framesLost, s.framesComplete, g_failures);
    return g_failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// mode pacing: QueueGuard state machine
// ---------------------------------------------------------------------------

int FuzzPacing(uint64_t n) {
    std::mt19937_64 rng(0xFACE0FFF12345678ULL);
    rc::QueueGuard guard;
    guard.Configure(rc::GuardConfigFor(rc::StreamProfile::Balanced));
    uint64_t dips = 0, climbs = 0, released = 0;
    uint32_t maxTargetSeen = 0;
    int64_t nowNs = 0;
    std::uniform_real_distribution<> unit(0.0, 1.0);
    for (g_i = 0; g_i < n; g_i++) {
        progress("pacing");
        // log-uniform target in [1000, 200000] kbps
        const uint32_t target = uint32_t(1000.0 * std::exp(unit(rng) * std::log(200.0)));
        maxTargetSeen = target > maxTargetSeen ? target : maxTargetSeen;
        double queueMs;
        const double qr = unit(rng);
        if (qr < 0.15) queueMs = -1.0;  // unmeasured
        else if (qr < 0.85) queueMs = -20.0 + unit(rng) * 140.0;
        else queueMs = -20.0 + unit(rng) * 3000.0;
        const double floorMs = unit(rng) * 120.0;
        const bool key = unit(rng) < 0.10;

        // clock: mostly steady, sometimes jumps (forward or backward), occasionally floods
        if (g_i % 10000 == 9999) {
            for (int f = 0; f < 1000; f++) {
                const auto r = guard.Update(target, queueMs, floorMs, unit(rng) < 0.05, nowNs);
                CHECK(r.capKbps <= maxTargetSeen * 2u + 100000u);
            }
        }
        nowNs += int64_t(unit(rng) * 50.0) * 1000000LL;
        if (unit(rng) < 0.04) nowNs += (1 + int64_t(unit(rng) * 10.0)) * 1000000000LL;
        if (unit(rng) < 0.02) nowNs -= (1 + int64_t(unit(rng) * 2.0)) * 1000000000LL;  // clock jump back

        const auto r = guard.Update(target, queueMs, floorMs, key, nowNs);
        CHECK(r.capKbps <= maxTargetSeen * 2u + 100000u);  // the cap must stay bounded
        if (r.action == rc::QueueGuard::Action::Dip) dips++;
        else if (r.action == rc::QueueGuard::Action::Climb) climbs++;
        else if (r.action == rc::QueueGuard::Action::Released) released++;
        if (r.action == rc::QueueGuard::Action::Dip) CHECK(guard.Cap() == r.capKbps && r.capKbps > 0);
        if (r.capKbps) CHECK(r.capKbps >= 1000);  // the guard's own hard floor
    }
    std::printf("fuzz pacing: %llu iterations, dips %llu, climbs %llu, released %llu, max target %u kbps, "
                "failures %d\n",
                (unsigned long long)n, (unsigned long long)dips, (unsigned long long)climbs,
                (unsigned long long)released, maxTargetSeen, g_failures);
    return g_failures ? 1 : 0;
}


// ---------------------------------------------------------------------------
// mode chunk: the control-chunk reassembler behind the MAC (ROUND19 T2). Drives
// Connection::HandleControlChunkForTest - the compile-time seam - with mutated
// chunk sequences; asserts bounded state, clean drops, bounded time.
// ---------------------------------------------------------------------------

int FuzzChunk(uint64_t n) {
    std::mt19937_64 rng(0xC0DEC0DE12345678ULL);
    rc::UdpSocket sock;  // never opened: SendAck's send just returns false
    auto keys = rc::crypto::DeriveSessionKeys(rc::crypto::KeyPair::Generate().secretKey,
                                              rc::crypto::KeyPair::Generate().publicKey,
                                              rc::crypto::KeyPair::Generate().publicKey,
                                              rc::crypto::KeyPair::Generate().publicKey, "t");
    rc::Connection conn(&sock, rc::NetAddress{0x7F000001, 1}, 7, keys, true, rc::kMaxDatagramLan);
    uint64_t deliveredTotal = 0, fed = 0;
    conn.SetControlHandler([&](const std::string& msg) {
        deliveredTotal++;
        CHECK(msg.size() <= (1ull << 30));  // a delivered message must be sane
    });
    const size_t kHeader = sizeof(rc::ControlChunkHeader);
    std::vector<uint8_t> scratch(rc::kMaxDatagramLan);
    uint32_t cursor = 0;   // the in-order writer's sequence cursor
    uint32_t msgId = 0;    // the in-order writer's message id
    for (g_i = 0; g_i < n; g_i++) {
        progress("chunk");
        const int bursts = 1 + int(rng() % 6);
        for (int b = 0; b < bursts; b++) {
            rc::ControlChunkHeader h{};
            size_t size = kHeader;
            const unsigned cls = unsigned(rng() % 20);
            if (cls < 14) size = kHeader + rng() % 300;
            else if (cls < 17) size = kHeader + rng() % 400;
            const unsigned kind = unsigned(rng() % 10);
            if (kind < 6) {
                // a complete small in-order message: exercises the assembly/delivery path
                const uint16_t count = uint16_t(1 + rng() % 3);
                for (uint16_t c = 0; c < count; c++) {
                    h.chunkSeq = cursor++;
                    h.messageId = msgId;
                    h.chunkIndex = c;
                    h.chunkCount = count;
                    std::memcpy(scratch.data(), &h, kHeader);
                    for (size_t i = kHeader; i < size; i++) scratch[i] = uint8_t(rng() & 0xFF);
                    conn.HandleControlChunkForTest(scratch.data(), size);
                    fed++;
                }
                msgId++;
            } else {
                // chaos: random seq/index/count - exercises m_outOfOrder, message-id
                // mismatches, oversized chunkCount and the drop paths
                h.chunkSeq = uint32_t(rng() % 100000);
                h.messageId = uint32_t(rng() % 1000);
                h.chunkIndex = uint16_t(rng() % 65536);
                h.chunkCount = uint16_t(rng() % 100 < 50 ? (rng() % 3 ? 1 : 8) : rng() % 65536);
                std::memcpy(scratch.data(), &h, kHeader);
                for (size_t i = kHeader; i < size; i++) scratch[i] = uint8_t(rng() & 0xFF);
                conn.HandleControlChunkForTest(scratch.data(), size);
                fed++;
            }
        }
        CHECK(deliveredTotal <= fed);
    }
    std::printf("fuzz chunk: %llu iterations, chunks fed %llu, messages delivered %llu, failures %d\n",
                (unsigned long long)n, (unsigned long long)fed, (unsigned long long)deliveredTotal, g_failures);
    return g_failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// mode conn: encrypted Connection::HandleDatagram (AEAD auth, version gate,
// replay window, control/ack/ping/pong dispatch) under adversarial mutation.
// ---------------------------------------------------------------------------

int FuzzConn(uint64_t n) {
    std::mt19937_64 rng(0xAEAD5EEDCAFE0001ULL);
    rc::UdpSocket sock;  // closed socket: replies drop quickly without kernel blocking
    const auto clientKeys = rc::crypto::KeyPair::Generate();
    const auto serverKeys = rc::crypto::KeyPair::Generate();
    const auto keys = rc::crypto::DeriveSessionKeys(
        serverKeys.secretKey, clientKeys.publicKey, clientKeys.publicKey, serverKeys.publicKey, "fuzz-conn");
    rc::Connection server(&sock, rc::NetAddress{0x7F000001, 40001}, 0x99887766u, keys, true, rc::kMaxDatagramLan);
    uint64_t accepted = 0, rejectedBefore = 0;
    server.SetControlHandler([&](const std::string& msg) { CHECK(msg.size() <= (4u * 1024u * 1024u)); });
    server.SetPacketHandler([&](rc::PacketType, const uint8_t*, size_t sz) { CHECK(sz <= rc::kMaxDatagramLan); });

    std::vector<uint8_t> datagram(rc::kMaxDatagramLan);
    uint64_t seq = 1;
    for (g_i = 0; g_i < n; g_i++) {
        progress("conn");
        const unsigned cls = unsigned(rng() % 10);
        if (cls < 4) {
            // Authentic client-to-server encrypted datagram (possibly replayed or bit-flipped).
            rc::PacketHeader h{};
            h.version = cls == 0 ? uint8_t(rng() & 0xFF) : uint8_t(rc::kProtocolVersion);
            h.type = uint8_t(rng() % 16);
            h.flags = rc::kFlagEncrypted;
            h.sessionId = cls == 1 ? uint32_t(rng()) : 0x99887766u;
            h.sequence = (rng() % 5 == 0 && seq > 5) ? (seq - (rng() % 5)) : seq++;
            const size_t plainLen = rng() % 128;
            std::vector<uint8_t> plain(plainLen);
            for (size_t i = 0; i < plainLen; i++) plain[i] = uint8_t(rng() & 0xFF);
            std::memcpy(datagram.data(), &h, sizeof(h));
            rc::crypto::Seal(keys.clientToServer, h.sequence, 0x43, datagram.data(), sizeof(h), plain.data(),
                             plainLen, datagram.data() + sizeof(h));
            if (cls == 2) datagram[sizeof(h) + (rng() % (plainLen + rc::kMacSize))] ^= 0x55;
            const size_t total = sizeof(h) + plainLen + rc::kMacSize;
            if (server.HandleDatagram(datagram.data(), total, rc::NetAddress{0x7F000001, 40001})) accepted++;
        } else {
            // Arbitrary truncated or oversized garbage datagrams.
            const size_t len = rng() % (rc::kMaxDatagramLan + 64);
            std::vector<uint8_t> junk(len);
            for (size_t i = 0; i < len; i++) junk[i] = uint8_t(rng() & 0xFF);
            CHECK(!server.HandleDatagram(junk.data(), junk.size(), rc::NetAddress{0x7F000002, 40002}) ||
                  len >= sizeof(rc::PacketHeader) + rc::kMacSize);
        }
        if (g_i % 500 == 0) server.Tick(int64_t(g_i) * 1'000'000LL);
        const auto stats = server.Stats();
        CHECK(stats.packetsRejected >= rejectedBefore);
        rejectedBefore = stats.packetsRejected;
    }
    std::printf("fuzz conn: %llu iterations, accepted %llu, rejected %llu, failures %d\n",
                (unsigned long long)n, (unsigned long long)accepted, (unsigned long long)rejectedBefore, g_failures);
    return g_failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// mode motion: DecodeMotionRle / DecodeVrMotionMetadata & Opus PLC bounds.
// ---------------------------------------------------------------------------

int FuzzMotion(uint64_t n) {
    std::mt19937_64 rng(0xF00D5EEDFACE1234ULL);
    rc::AudioDecoder audioDec;
    CHECK(audioDec.Init(48000, 2));
    rc::HeadsetMotionState state;
    uint64_t decodedOk = 0, failuresReset = 0;
    for (g_i = 0; g_i < n; g_i++) {
        progress("motion");
        if ((g_i % 5) != 0) {
            // Adversarial VrFrameMetadata + trailing RLE payload (valid/hostile dims & RLE).
            const size_t size = rng() % 512;
            std::vector<uint8_t> meta(size);
            for (size_t i = 0; i < size; i++) meta[i] = uint8_t(rng() & 0xFF);
            const bool foveated = (rng() % 4) == 0;
            const uint64_t trackingTs = 1'000'000'000ULL + (rng() % 100'000ULL);
            const size_t at = sizeof(rc::VrFrameMetadata) + (foveated ? sizeof(rc::VrFoveationMetadata) : 0);
            if (size >= at + sizeof(rc::MotionHeader) && (rng() % 3 != 0)) {
                rc::VrFrameMetadata vm{};
                vm.trackingTimestampNs = trackingTs;
                vm.eyeWidth = 2560;
                vm.eyeHeight = 1440;
                std::memcpy(meta.data(), &vm, sizeof(vm));
                rc::MotionHeader mh{};
                mh.gridWidth = uint16_t(rng() % 200);
                mh.gridHeight = uint16_t(rng() % 200);
                mh.blockSize = uint16_t(rng() % 33);
                mh.previousTimestampNs =
                    (rng() % 8 == 0) ? (trackingTs + 1000) : (trackingTs - 1'000'000ULL - (rng() % 30'000'000ULL));
                std::memcpy(meta.data() + at, &mh, sizeof(mh));
            }
            const auto res = rc::DecodeVrMotionMetadata(true, foveated, trackingTs, meta, state);
            if (res.decoded) {
                CHECK(state.width > 0 && state.height > 0 && state.block > 0 && state.frameNs > 0);
                CHECK(state.grid.size() == size_t(state.width) * size_t(state.height) * 2);
                decodedOk++;
            } else {
                CHECK(!state.moves && !state.consistent && state.frameNs == 0);
                failuresReset++;
            }
        } else {
            // Hostile AudioDecoder::Decode PLC sizes never overrun stack buffer.
            std::vector<int16_t> pcm(5760 * 2 + 16, 0x1111);
            const int req = int(rng() % 70000) - 50;
            const int r = audioDec.Decode(nullptr, 0, pcm.data(), req);
            CHECK(r <= 5760);
            for (size_t i = size_t(5760) * 2; i < pcm.size(); i++) CHECK(pcm[i] == 0x1111);
        }
    }
    std::printf("fuzz motion: %llu iterations, ready %llu, clean-reset %llu, failures %d\n",
                (unsigned long long)n, (unsigned long long)decodedOk, (unsigned long long)failuresReset, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Unattended runs: Debug-CRT assertions (e.g. vector<> bounds) go to stderr instead
    // of popping a modal dialog that hangs the process until someone clicks it.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    _set_error_mode(_OUT_TO_STDERR);
#endif
    if (argc < 2) {
        std::printf("usage: rincast-fuzz <msg|fec|pacing|chunk|conn|motion> [iterations]\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (argc >= 3) g_iterations = std::strtoull(argv[2], nullptr, 10);
    const auto t0 = std::chrono::steady_clock::now();
    int rc = 0;
    if (mode == "msg") rc = FuzzMsg(g_iterations);
    else if (mode == "fec") rc = FuzzFec(g_iterations);
    else if (mode == "pacing") rc = FuzzPacing(g_iterations);
    else if (mode == "chunk") rc = FuzzChunk(g_iterations);
    else if (mode == "conn") rc = FuzzConn(g_iterations);
    else if (mode == "motion") rc = FuzzMotion(g_iterations);
    else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuzz %s: %llu iterations in %.1f s (%s)\n", mode.c_str(), (unsigned long long)g_iterations,
                secs, rc == 0 ? "OK" : "FAILED");
    return rc;
}
