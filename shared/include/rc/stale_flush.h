// A26 (TASK-BACKLOG-FLUSH-2026-11-22): the sender-side stale-frame flush.
//
// The AB9 session measured what a scene change does: the encoder outputs giant
// frames / keyframes, the token bucket bills each one at the wire ceiling, and
// every following frame queues at the frame boundary behind that billing - the
// headset reports the queue growing (119 -> 72 ms) while nothing is dropped
// ("frames are never dropped, the encoder waits"), and the picture trails the
// game for seconds until adaptive bitrate catches up. VD instead throws the
// backlog away and jumps to the new scene.
//
// The flush does the same on the send thread: a frame whose predicted display
// moment has already passed is late no matter how fast it goes out, so it is
// dropped, the pacer's billing is wiped, and a keyframe is requested (throttled)
// so the reference chain rebuilds. Normal frames are never touched: their
// predicted display time sits in the future by design, so in the steady state
// the whole branch is dead code.
//
// Pure decision logic in its own header so core_tests can walk a synthetic
// timeline (pass / drop / debt reset / throttles) without a stream or a pacer.
#pragma once

#include <cstdint>

namespace rc {

// While the flush keeps dropping, a keyframe request goes out at most once per
// this window (card §1.2: every 300 ms at most - a request storm would turn every
// recovery frame into another giant frame and re-trigger the very queue it fixes).
constexpr int64_t kFlushKeyframeThrottleNs = 300'000'000;

// The per-flush log line goes out at most once per this window (card §1.4: 1 s),
// carrying the drop count accumulated since the last line.
constexpr int64_t kFlushLogThrottleNs = 1'000'000'000;

// True when the frame's display moment has passed: whatever it costs, the headset
// shows it stale or skips it. `predictedDisplayNs` is the frame's predicted display
// time in the PC clock (0 = unknown - no tracking yet): an unknown moment never
// expires, so a stream without the prediction keeps sending every frame, exactly
// as before. Strict `<`: a frame predicted for exactly `nowNs` still has its
// moment (it may just make the next vsync).
inline bool FrameExpiredForSend(int64_t predictedDisplayNs, int64_t nowNs) {
    return predictedDisplayNs > 0 && predictedDisplayNs < nowNs;
}

// The flush's billing rule (card §1.3): the backlog's debt is forgiven - the
// token bucket re-baselines to `nowNs` so the next frame may start at once,
// instead of first repaying the giant frames' billing that queued it.
// `previousDebtNs` is deliberately ignored: the wipe is unconditional.
inline int64_t FlushSendDebtNs(int64_t /*previousDebtNs*/, int64_t nowNs) {
    return nowNs;
}

// One decision for the send loop: drop the frame, and which of the throttled
// side effects fire with this drop.
struct FlushDecision {
    bool drop;            // the frame is past its display moment: do not send it
    bool requestKeyframe; // this drop carries the keyframe request (300 ms throttle)
    bool log;             // this drop carries the log line (1 s throttle)
};

// The send loop's per-frame gate, all in one call so the loop stays mechanical:
// a frame predicted for the future passes untouched; an expired one drops, and
// the two throttles decide which side effects this particular drop carries.
// `lastKeyframeReqNs` / `lastLogNs` are the caller's throttle stamps (0 = never).
inline FlushDecision StaleFlushDecide(int64_t predictedDisplayNs, int64_t nowNs,
                                      int64_t lastKeyframeReqNs, int64_t lastLogNs) {
    if (!FrameExpiredForSend(predictedDisplayNs, nowNs)) return {false, false, false};
    const auto due = [](int64_t lastNs, int64_t now, int64_t windowNs) {
        return lastNs <= 0 || now - lastNs >= windowNs;
    };
    return {true,
            due(lastKeyframeReqNs, nowNs, kFlushKeyframeThrottleNs),
            due(lastLogNs, nowNs, kFlushLogThrottleNs)};
}

}  // namespace rc
