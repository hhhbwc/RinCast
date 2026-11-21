#include "rc/video_transport.h"

#include <algorithm>
#include <cstring>

#include "rc/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is Win10 1803+; older SDKs lack it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace rc {

namespace {
inline bool FrameNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

// Inside this window to the due time, send right away instead of waiting.
// The high-resolution cancelable wait wakes with ~1.0 ms accuracy on this machine
// (measured: a 0.5 ms period timer ticks at 1.0 ms), so the threshold stays small:
// a batch emits ~(wake error + threshold) x pacing rate, ~22 KB at 0.2 ms.
constexpr int64_t kPaceWaitThresholdNs = 200'000;
// At most this much of a late send clock is repaid; beyond it the debt is dropped
constexpr int64_t kPaceMaxDebtNs = 600'000;
// (better to send less than to repay a stall as one burst).
constexpr int64_t kPaceIdleResetNs = 50'000'000;
}  // namespace

VideoPacketizer::VideoPacketizer(Connection* connection) : m_connection(connection) {
#ifdef _WIN32
    // High-resolution cancelable wait: a high-resolution timer (~0.5 ms real accuracy)
    // plus the stop event, waited on together, so shutdown need not wait out a packet.
    m_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!m_paceTimer) m_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
}

VideoPacketizer::~VideoPacketizer() {
#ifdef _WIN32
    if (m_paceTimer) CloseHandle(static_cast<HANDLE>(m_paceTimer));
    if (m_stopEvent) CloseHandle(static_cast<HANDLE>(m_stopEvent));
    m_paceTimer = nullptr;
    m_stopEvent = nullptr;
#endif
}

void VideoPacketizer::RequestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
#ifdef _WIN32
    if (m_stopEvent) SetEvent(static_cast<HANDLE>(m_stopEvent));
#endif
}

bool VideoPacketizer::WaitDue(int64_t dueNs, bool precise) {
    if (m_stopRequested.load(std::memory_order_relaxed)) return false;
    // Legacy mode keeps the 0.2 ms "due now" window (fragments inside it leave together).
    // Precise mode (ROUND4 R4.1 shaping) waits down to ~0 so fragments are placed at the
    // computed sub-ms interval instead of clumping into a micro-burst after each timer wake.
    const int64_t threshold = precise ? 0 : kPaceWaitThresholdNs;
    if (dueNs - NowNs() <= threshold) return true;  // due now, send at once
#ifdef _WIN32
    if (precise) {
        // The high-resolution waitable timer still quantizes to ~1 ms on this machine, which
        // is coarser than a fragment interval (~50-100 us). Sleep the bulk of a long wait via
        // the timer (so shutdown stays responsive and CPU is not burned on long gaps), then
        // spin the final tail for precise placement. Short waits spin outright.
        constexpr int64_t kSpinTailNs = 400'000;
        const int64_t remain = dueNs - NowNs();
        if (remain > kSpinTailNs + 1'000'000 && m_paceTimer && m_stopEvent) {
            LARGE_INTEGER li;
            li.QuadPart = -std::max<int64_t>(1, (remain - kSpinTailNs) / 100);
            if (SetWaitableTimerEx(static_cast<HANDLE>(m_paceTimer), &li, 0, nullptr, nullptr, nullptr, 0)) {
                const HANDLE handles[2] = {static_cast<HANDLE>(m_paceTimer), static_cast<HANDLE>(m_stopEvent)};
                const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0 + 1) return false;
            }
        }
        while (!m_stopRequested.load(std::memory_order_relaxed)) {
            if (dueNs - NowNs() <= 0) break;
            YieldProcessor();
        }
        return !m_stopRequested.load(std::memory_order_relaxed);
    }
    if (m_paceTimer && m_stopEvent) {
        const int64_t remain = dueNs - NowNs();
        if (remain <= kPaceWaitThresholdNs) return true;
        LARGE_INTEGER li;
        li.QuadPart = -std::max<int64_t>(1, remain / 100);  // negative = relative, in 100 ns units
        if (SetWaitableTimerEx(static_cast<HANDLE>(m_paceTimer), &li, 0, nullptr, nullptr, nullptr, 0)) {
            const HANDLE handles[2] = {static_cast<HANDLE>(m_paceTimer), static_cast<HANDLE>(m_stopEvent)};
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 + 1) return false;  // the stop event came first
            return true;
        }
    }
#endif
    // Fallback when the timer is unavailable: coarse sleeps (still an order of magnitude
    const int64_t remain = dueNs - NowNs();
    if (remain > 1'000'000) SleepMs(int(remain / 1'000'000));
    return !m_stopRequested.load(std::memory_order_relaxed);
}

size_t VideoPacketizer::SendFrame(uint8_t streamId, VideoCodec codec, uint8_t frameFlags, uint32_t frameIndex,
                                  const void* metadata, uint16_t metadataSize, const uint8_t* bitstream,
                                  size_t bitstreamSize) {
    m_frame.resize(2 + metadataSize + bitstreamSize);
    std::memcpy(m_frame.data(), &metadataSize, 2);
    if (metadataSize) std::memcpy(m_frame.data() + 2, metadata, metadataSize);
    if (bitstreamSize) std::memcpy(m_frame.data() + 2 + metadataSize, bitstream, bitstreamSize);

    const size_t fragPayload = m_connection->MaxPayload() - sizeof(VideoFragmentHeader);
    const size_t total = m_frame.size();
    const size_t count = (total + fragPayload - 1) / fragPayload;
    if (count > 0xFFFF) {
        RC_LOGE("Video frame too large (%zu bytes)", total);
        return 0;
    }

    uint8_t group = 0;
    if (m_fecPercent > 0) group = uint8_t(std::clamp(100 / m_fecPercent, 2, 64));

    VideoFragmentHeader h{};
    h.streamId = streamId;
    h.codec = uint8_t(codec);
    h.frameFlags = frameFlags;
    h.fecGroupSize = group;
    h.frameIndex = frameIndex;
    h.fragmentCount = uint16_t(count);
    h.frameSize = uint32_t(total);

    std::vector<uint8_t> parity;
    if (group) parity.assign(fragPayload, 0);
    size_t sent = 0;
    // A datagram the socket refuses (send buffer full, SO_SNDTIMEO expired) never reaches
    // the path at all: the headset counts it as loss, while the sender's stats and the
    // congestion guard see nothing - the one loss source neither of them can explain.
    // Count it and say so now and then, so a local stall leaves a trace.
    const auto send = [this, streamId](PacketType type, const void* header, size_t headerSize, const uint8_t* data,
                                       size_t len) {
        if (m_connection->Send2(type, header, headerSize, data, len)) return;
        m_sendFailures.fetch_add(1, std::memory_order_relaxed);
        const int64_t now = NowNs();
        if (now - m_lastSendFailLogNs > 5'000'000'000LL) {
            m_lastSendFailLogNs = now;
            RC_LOGW("Stream %u: %u datagrams refused by the socket so far (the path never saw them)", streamId,
                    m_sendFailures.load());
        }
    };
    const size_t lastSize = total - (count - 1) * fragPayload;
    const uint32_t kbps = m_pacingKbps.load(std::memory_order_relaxed);
    const int64_t budgetNs = m_frameBudgetNs.load(std::memory_order_relaxed);
    const bool paced = kbps != 0;
    // Every byte (headers and FEC parity included) takes wire time: this is that count.
    double nsPerByte = paced ? 8e6 / double(kbps) : 0.0;
    int64_t frameStartNs = 0;
    // Token-bucket burst mode (frame budget armed, A10): the frame's datagrams leave
    // BACK-TO-BACK - nothing inside the frame waits or spreads them - and the budget is
    // kept BETWEEN frames: the frame bills its wire bytes at the average ceiling
    // (m_pacingKbps, the user's setting x 1.08) and the NEXT frame may not start before
    // that billing elapses. A frame that arrives under budget starts at once (no credit
    // accumulates across idle, and none is needed); an over-budget one delays the next
    // frame as a WHOLE - its natural frame-size/rate interval - never as a stretched-out
    // frame. Sustained over-ceiling output is the encoder target's job (wire ceiling,
    // adaptive bitrate, loss brake); the send layer only keeps the mean and never
    // manufactures intra-frame delay. Frames are never dropped here: the wait sits at
    // the frame boundary, in front of the first fragment, and a shutdown cancels it.
    // The legacy clock (budget 0 - the desktop stream and remote paths) still spreads
    // fragments at kbps below, byte-for-byte as before.
    bool burstTokens = false;
    if (budgetNs > 0 && paced) {
        burstTokens = true;
        const size_t nParity = group ? (count + group - 1) / group : 0;
        const size_t wireBytes = total + count * sizeof(VideoFragmentHeader) +
                                 nParity * (sizeof(VideoParityHeader) + fragPayload);
        // The previous frame's billing may still be running: wait it out once, here at
        // the frame boundary (RequestStop cancels it for shutdown), then send whole.
        if (m_nextSendNs > NowNs() && !WaitDue(m_nextSendNs, false)) {
            RC_LOGD("Video pacing: stopping before frame %u (token-bucket wait)", frameIndex);
            return 0;
        }
        frameStartNs = NowNs();
        m_nextSendNs = frameStartNs + int64_t(double(wireBytes) * nsPerByte);
    } else if (paced) {
        const int64_t now = NowNs();
        if (m_nextSendNs == 0 || m_nextSendNs < now - kPaceIdleResetNs) m_nextSendNs = now;
    }

    for (size_t i = 0; i < count; i++) {
        const size_t offset = i * fragPayload;
        const size_t len = std::min(fragPayload, total - offset);
        h.fragmentIndex = uint16_t(i);
        if (paced && !burstTokens) {
            if (!WaitDue(m_nextSendNs, false)) {
                RC_LOGD("Video pacing: stopping mid-frame (%zu of %zu datagrams sent)", sent, count);
                return sent;
            }
            // A big lag drops the extra debt (no burst after a stall); a small lag keeps it, so
            // the pacing rate is not eaten by the wake error.
            const int64_t now = NowNs();
            if (m_nextSendNs < now - kPaceMaxDebtNs) m_nextSendNs = now - kPaceMaxDebtNs;
        }
        send(PacketType::Video, &h, sizeof(h), m_frame.data() + offset, len);
        sent++;
        if (paced && !burstTokens) m_nextSendNs += int64_t(double(sizeof(h) + len) * nsPerByte);
        if (group) {
            for (size_t b = 0; b < len; b++) parity[b] ^= m_frame[offset + b];
            const bool groupEnd = ((i + 1) % group == 0) || (i + 1 == count);
            if (groupEnd) {
                VideoParityHeader ph{};
                ph.streamId = streamId;
                ph.fecGroupSize = group;
                ph.groupIndex = uint16_t(i / group);
                ph.frameIndex = frameIndex;
                ph.fragmentCount = uint16_t(count);
                ph.lastFragmentSize = uint16_t(lastSize);
                ph.frameSize = uint32_t(total);
                // Only send parity when the group has more than one member.
                const size_t groupMembers = i - size_t(ph.groupIndex) * group + 1;
                if (groupMembers > 1) {
                    // Parity packets cost the same wire time as data ones and go out at the same pace.
                    if (paced && !burstTokens) {
                        if (!WaitDue(m_nextSendNs, false)) {
                            RC_LOGD("Video pacing: stopping mid-frame (%zu of %zu datagrams sent)", sent, count);
                            return sent;
                        }
                        const int64_t now = NowNs();
                        if (m_nextSendNs < now - kPaceMaxDebtNs) m_nextSendNs = now - kPaceMaxDebtNs;
                    }
                    send(PacketType::VideoParity, &ph, sizeof(ph), parity.data(), fragPayload);
                    sent++;
                    if (paced && !burstTokens) m_nextSendNs += int64_t(double(sizeof(ph) + fragPayload) * nsPerByte);
                }
                std::fill(parity.begin(), parity.end(), uint8_t(0));
            }
        }
    }
    if (frameStartNs) {
        // The token-bucket billing set at the frame boundary stands (the next frame's
        // earliest start); only the burst DURATION is judged against the budget here:
        // overruns mean the socket/path backed the burst up, not that the bucket is spent.
        const int64_t took = NowNs() - frameStartNs;
        const int64_t budget = m_frameBudgetNs.load(std::memory_order_relaxed);
        if (took > budget + 1'000'000LL) {
            m_budgetOverruns.fetch_add(1, std::memory_order_relaxed);
            RC_LOGD("Video pacing: frame %u burst took %.1f ms for a %lld ms budget", frameIndex,
                    double(took) / 1e6, (long long)(budget / 1000000));
        }
    }
    return sent;
}

void VideoReassembler::Reset() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    m_hasComplete = false;
}

ReassemblyStats VideoReassembler::TakeStats() {
    std::lock_guard lock(m_mutex);
    ReassemblyStats s = m_stats;
    m_stats = {};
    return s;
}

void VideoReassembler::HandleFragment(const uint8_t* payload, size_t size) {
    if (size < sizeof(VideoFragmentHeader)) return;
    VideoFragmentHeader h;
    std::memcpy(&h, payload, sizeof(h));
    const uint8_t* data = payload + sizeof(h);
    const size_t len = size - sizeof(h);
    if (h.fragmentCount == 0 || h.fragmentIndex >= h.fragmentCount) return;

    std::unique_lock lock(m_mutex);
    if (m_hasComplete && !FrameNewer(h.frameIndex, m_lastComplete)) return;  // stale
    m_stats.fragmentsReceived++;
    m_stats.bytesReceived += size;

    auto& a = m_pending[h.frameIndex];
    if (a.fragmentCount == 0) {
        a.streamId = h.streamId;
        a.fragmentCount = h.fragmentCount;
        a.fecGroupSize = h.fecGroupSize;
        a.frameSize = h.frameSize;
        a.fragments.resize(h.fragmentCount);
        a.present.assign(h.fragmentCount, false);
        a.firstNs = NowNs();
    }
    // Not only for a new assembly: a parity packet may have created it (reordering).
    a.codec = VideoCodec(h.codec);
    a.flags = h.frameFlags;
    // The assembly may predate this fragment, created by a parity packet whose header
    // claimed a different count: a fragment outside it is garbage, never a place to
    // write into. (ROUND19 T2, found by the fuzzer: vector<bool> out of range.)
    if (h.fragmentIndex >= a.fragmentCount) return;
    if (h.fragmentIndex == 0 || h.fragmentIndex + 1 < h.fragmentCount) a.fragmentPayload = std::max(a.fragmentPayload, len);
    if (a.present[h.fragmentIndex]) return;
    a.fragments[h.fragmentIndex].assign(data, data + len);
    a.present[h.fragmentIndex] = true;
    a.received++;
    if (a.fecGroupSize) TryRecover(h.frameIndex, a, uint16_t(h.fragmentIndex / a.fecGroupSize));
    TryComplete(h.frameIndex);
}

void VideoReassembler::HandleParity(const uint8_t* payload, size_t size) {
    if (size < sizeof(VideoParityHeader)) return;
    VideoParityHeader h;
    std::memcpy(&h, payload, sizeof(h));
    std::unique_lock lock(m_mutex);
    if (m_hasComplete && !FrameNewer(h.frameIndex, m_lastComplete)) return;
    auto& a = m_pending[h.frameIndex];
    if (a.fragmentCount == 0) {
        a.streamId = h.streamId;
        a.fragmentCount = h.fragmentCount;
        a.fecGroupSize = h.fecGroupSize;
        a.frameSize = h.frameSize;
        a.fragments.resize(h.fragmentCount);
        a.present.assign(h.fragmentCount, false);
        a.firstNs = NowNs();
    }
    a.fragmentPayload = std::max(a.fragmentPayload, size - sizeof(h));
    a.parity[h.groupIndex].assign(payload + sizeof(h), payload + size);
    TryRecover(h.frameIndex, a, h.groupIndex);
    TryComplete(h.frameIndex);
}

void VideoReassembler::TryRecover(uint32_t frameIndex, Assembly& a, uint16_t group) {
    auto it = a.parity.find(group);
    if (it == a.parity.end() || !a.fecGroupSize) return;
    const size_t first = size_t(group) * a.fecGroupSize;
    const size_t last = std::min<size_t>(first + a.fecGroupSize, a.fragmentCount);
    int missing = -1;
    for (size_t i = first; i < last; i++) {
        if (!a.present[i]) {
            if (missing >= 0) return;  // more than one missing: can't recover
            missing = int(i);
        }
    }
    if (missing < 0) return;
    std::vector<uint8_t> rec = it->second;
    for (size_t i = first; i < last; i++) {
        if (int(i) == missing) continue;
        const auto& f = a.fragments[i];
        for (size_t b = 0; b < f.size() && b < rec.size(); b++) rec[b] ^= f[b];
    }
    size_t len = a.fragmentPayload;
    if (size_t(missing) + 1 == a.fragmentCount) {
        len = a.frameSize - (size_t(a.fragmentCount) - 1) * a.fragmentPayload;
    }
    rec.resize(std::min(len, rec.size()));
    a.fragments[missing] = std::move(rec);
    a.present[missing] = true;
    a.received++;
    m_stats.fragmentsRecovered++;
    (void)frameIndex;
}

void VideoReassembler::TryComplete(uint32_t frameIndex) {
    auto it = m_pending.find(frameIndex);
    if (it == m_pending.end()) return;
    auto& a = it->second;
    if (a.received != a.fragmentCount || a.fragmentCount == 0) return;
    // Codec/flags are only known from data fragments; a frame fully rebuilt from parity
    // alone (single fragment groups never have parity) can't happen, so this is safe.

    std::vector<uint8_t> frame;
    // Cap the reservation hint at what insert() can actually add: a header with a lying
    // frameSize must not turn into a multi-GB reservation. (ROUND19 T2, found by the fuzzer.)
    frame.reserve(std::min<uint64_t>(a.frameSize, uint64_t(a.fragmentCount) * a.fragmentPayload));
    for (auto& f : a.fragments) frame.insert(frame.end(), f.begin(), f.end());
    if (frame.size() < 2) {
        m_pending.erase(it);
        return;
    }
    uint16_t metaSize;
    std::memcpy(&metaSize, frame.data(), 2);
    if (2u + metaSize > frame.size()) {
        m_pending.erase(it);
        return;
    }

    ReceivedFrame out;
    out.streamId = a.streamId;
    out.codec = a.codec;
    out.flags = a.flags;
    out.frameIndex = frameIndex;
    out.metadata.assign(frame.begin() + 2, frame.begin() + 2 + metaSize);
    out.bitstream.assign(frame.begin() + 2 + metaSize, frame.end());
    out.firstFragmentNs = a.firstNs;
    out.completeNs = NowNs();
    m_stats.spreadMsSum += NsToMs(out.completeNs - out.firstFragmentNs);

    // Everything older than this frame is now useless; count unfinished ones as lost.
    for (auto p = m_pending.begin(); p != m_pending.end();) {
        if (!FrameNewer(p->first, frameIndex) && p->first != frameIndex) {
            m_stats.framesLost++;
            m_stats.fragmentsLost += p->second.fragmentCount - p->second.received;
            m_lostSinceExpire = true;
            p = m_pending.erase(p);
        } else {
            ++p;
        }
    }
    m_pending.erase(frameIndex);
    // Detect gaps in frame numbering (whole frames lost without a single fragment).
    if (m_hasComplete && frameIndex - m_lastComplete > 1) {
        m_stats.framesLost += frameIndex - m_lastComplete - 1;
        m_lostSinceExpire = true;
    }
    m_lastComplete = frameIndex;
    m_hasComplete = true;
    m_stats.framesComplete++;

    auto handler = m_handler;
    m_mutex.unlock();
    handler(std::move(out));
    m_mutex.lock();
}

bool VideoReassembler::Expire(int64_t nowNs, int64_t timeoutNs) {
    std::lock_guard lock(m_mutex);
    for (auto p = m_pending.begin(); p != m_pending.end();) {
        if (nowNs - p->second.firstNs > timeoutNs) {
            m_stats.framesLost++;
            m_stats.fragmentsLost += p->second.fragmentCount - p->second.received;
            m_lostSinceExpire = true;
            p = m_pending.erase(p);
        } else {
            ++p;
        }
    }
    const bool lost = m_lostSinceExpire;
    m_lostSinceExpire = false;
    return lost;
}

}  // namespace rc
