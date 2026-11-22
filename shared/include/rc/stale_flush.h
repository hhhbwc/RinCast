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
//
// A75 (TASK-A75-FLUSH-GRACE-2026-11-22) bounds that intent. The test above was "the
// display moment has passed", with no slack: the user's streamer.log 23:09-23:11 run
// (lines 2058-2129) shows a healthy 60 fps stream - 0.4-1.6 Mbps against a 74-90 Mbps
// ceiling, submit 1.3-6.1 ms, no loss reported - losing runs of 8 / 14 / 16 frames whose
// measured lateness in the same lines is 0 / 1 / 8 ms. A frame less than a frame period
// late is shown at the next vsync, not "stale no matter how fast it goes out": dropping
// it whole is what the user saw as "stutters". The judgment is now "past the display
// moment by a named grace period" (kFlushGraceNs below), derived from the two measured
// groups and placed in the one judgment point both the gate and the send loop share.
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

// A46 circuit breaker: a prediction/clock offset can make every frame satisfy the
// expired test, which would drop 100% of frames forever (black screen). After this
// many consecutive drops the sender forces one frame through and resets the streak.
constexpr uint32_t kStaleFlushBreakerDrops = 30;
inline bool StaleFlushBreaker(uint32_t consecutiveDrops) {
    return consecutiveDrops >= kStaleFlushBreakerDrops;
}

// A75: the grace period, written as a multiple of the frame period so the number is
// arguable rather than a taste.
//
//   kFlushFramePeriodNs = 1 s / 60 = 16'666'666 ns.
//       The evidence run encodes at 60 fps (streamer.log's encoder lines 1859-2195 all
//       read "60 fps"), so one frame period is 16.7 ms - the distance between "missed the
//       display moment" and "shown one vsync late".
//   kFlushGraceNs = 2 periods = 33'333'332 ns.
//       One period buys back the vsync the frame genuinely still makes (the comment this
//       function used to carry: a frame at its own moment "may just make the next vsync").
//       The second period is for the noise on top of that: the log's lateness readings
//       come from two prediction sources (the headset's exact target moment, or the
//       submit + 40 ms predict-ahead estimate - vr_stream.cpp:565-582), and an estimate is
//       off by roughly a frame period on its own.
//
// Both measured groups pin the value, and it is between them with room on each side:
//   1. the healthy run must stop dropping: the worst lateness streamer.log:2058-2129 reports
//      for the run the card quotes is 8 ms (line 2115; 0 ms and 1 ms at 2062-2106). 8 ms is
//      under half a frame period at 60 fps and under a whole one at every rate above it
//      (72/90/120 fps), so 2 periods cover the group with 4.2x to spare and keep the
//      flush's steady state dead. One period would not: the sender's own queue through that
//      window reads 0-3 ms with 0.0% loss while its slow-frames lines cost 16-22 ms to
//      encode, and the log's other healthy bursts show lateness of 9/11/14/15/17/18/19/21/
//      25 ms (lines 53-1427) - one period would still flush every reading from 17 ms up
//      while nothing is backed up.
//   2. the real backlog must still clear: AB9 measured 119 ms and 479 ms past the display
//      moment (stale_flush.h:1-9, the numbers the flush was written for). Those are 3.6x
//      and 14.4x the grace, and a queue that deep is 7 and 29 frame periods long - no
//      backlog of that kind fits inside 2 periods.
// Anything that is neither (F2/C4's AB9-shaped 40 ms overtake) still drops. The number is
// deliberately NOT read from the configured frame rate (card §3.7: one named constant, no
// switch), because both bounds that pin it are absolute measurements - the reported noise
// (8 ms in the quoted run, up to 25 ms in the neighbouring healthy bursts), 119 ms of queue
// - not rate-relative ones. Measured against kFpsLadder
// (24..120 fps, rc/fps_ladder.h) 33.3 ms is 4 frame periods at 120 fps, exactly 2 at the
// 60 fps the evidence was written at, and 0.8 at the 24 fps bottom rung: constraint 1's
// noise cover holds at every rung, and only the "one vsync bought back" reading is exact
// from 60 fps up - at 24 fps a whole-period slip is still flushed, the conservative side of
// the trade and what the pre-A75 rule did there anyway.
constexpr int64_t kFlushFramePeriodNs = 1'000'000'000 / 60;  // 16'666'666 ns at 60 fps
constexpr int64_t kFlushGraceFramePeriods = 2;
constexpr int64_t kFlushGraceNs = kFlushGraceFramePeriods * kFlushFramePeriodNs;  // 33'333'332 ns

// True when the frame is past its display moment by more than the grace period above:
// whatever it costs, the headset shows it stale or skips it. `predictedDisplayNs` is the
// frame's predicted display time in the PC clock (0 = unknown - no tracking yet): an
// unknown moment never expires, so a stream without the prediction keeps sending every
// frame, exactly as before. Strict `<` is kept, now on the grace boundary: a frame at
// `nowNs - kFlushGraceNs` is still inside the grace (it may just make the next vsync);
// the first ns past the grace is what drops. The grace is added to the frame's moment
// rather than taken off `nowNs`, so the expiry comparison stays inside this one function
// (card §4.4: no second one anywhere in the file), and a monotonic clock value plus 33 ms
// cannot overflow int64 ns (that would need a clock about 292 years ahead).
inline bool FrameExpiredForSend(int64_t predictedDisplayNs, int64_t nowNs) {
    return predictedDisplayNs > 0 && predictedDisplayNs + kFlushGraceNs < nowNs;
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
    // A46: `now` can go backwards (clock source reset, sleep/wake, mixed time
    // bases). Without `now < lastNs` the subtraction is negative, the throttle
    // says "not due" until now catches up, and every keyframe request / log is
    // silently swallowed for that whole window. A rollback means due now.
    const auto due = [](int64_t lastNs, int64_t now, int64_t windowNs) {
        return lastNs <= 0 || now < lastNs || now - lastNs >= windowNs;
    };
    return {true,
            due(lastKeyframeReqNs, nowNs, kFlushKeyframeThrottleNs),
            due(lastLogNs, nowNs, kFlushLogThrottleNs)};
}

// A72 (TASK-A72-STALE-FLUSH-2026-11-22): the same question with the other half added.
// StaleFlushDecide only asks "is this frame still in time"; it never asked "what does
// this frame reference", so the frame right after a burst of drops went out with its
// own references already on the discard pile and the headset decoded it against a stale
// picture (the artifact the user sees as "stutters, then smears"). The gate below keeps
// the chain state and is the flush branch's only entry point; StaleFlushDecide stays as
// the expiry-only judgment the A46 offline tests walk.

// The flush's chain state: once a frame is dropped, every descendant of it is
// undecodable until a repair frame (IDR or post-invalidation recovery) gets out.
struct StaleFlushState {
    int64_t lastKeyframeReqNs = 0;  // 300 ms request throttle stamp (0 = never)
    int64_t lastLogNs = 0;          // 1 s log-line throttle stamp (0 = never)
    bool chainBroken = false;       // a frame was dropped, no repair has got out since
    uint32_t dropStreak = 0;        // consecutive drops, the circuit breaker's count
};

struct FlushGate {
    bool send;              // this frame may leave the sender
    bool requestKeyframe;   // ask the encoder for a repair now
    bool resetDebt;         // wipe the pacer's billing with this drop
    bool log;               // carry the once-per-second log line
    bool breaker;           // the forced repair fired (log a WARN)
};

// `isRepair` = this frame is an IDR or a recovery frame: it rebuilds the chain, so
// it is exempt from the expired test. `prev` is the caller's state, mutated in place.
inline FlushGate StaleFlushGate(bool isRepair, int64_t predictedDisplayNs, int64_t nowNs,
                                StaleFlushState& prev) {
    // A46: `now` can go backwards (clock source reset, sleep/wake, mixed time bases),
    // and a stamp ahead of `now` made the subtraction negative, so the throttle said
    // "not due" and silently swallowed every request and log until `now` caught up.
    // A rollback means due now - the same defense StaleFlushDecide keeps.
    const auto due = [](int64_t lastNs, int64_t now, int64_t windowNs) {
        return lastNs <= 0 || now < lastNs || now - lastNs >= windowNs;
    };
    // Rule 1: a repair frame goes out even past its display moment. Holding the send
    // window for one late IDR is the deal - every 300 ms without it is another second
    // of artifacts, so the expired test simply does not apply here.
    if (isRepair) {
        prev.chainBroken = false;
        prev.dropStreak = 0;
        prev.lastKeyframeReqNs = nowNs;  // a repair just went out: none needed yet
        return {true, false, false, false, false};
    }
    FlushGate g{false, false, true, false, false};  // a drop: the debt is forgiven (A26)
    if (prev.chainBroken) {
        // Rule 2: with the chain broken this frame decodes against references that are
        // gone, so it never goes out - freezing on the last good picture beats sending
        // a frame that is guaranteed garbage (the card's trade: more frames dropped
        // while broken, no artifact after the recovery).
        // The request here deliberately ignores the 300 ms throttle: that throttle
        // protects a healthy chain from a recovery storm, and a throttled request only
        // lengthens the break. Valid only while VideoSender::RequestKeyframe() is a
        // single bool - if it ever becomes a counter or a queue, the throttle belongs
        // here again.
        g.requestKeyframe = true;
        prev.lastKeyframeReqNs = nowNs;
    } else {
        // Rule 3: intact chain, so the original expiry test decides, throttles included
        // (A75: that test now carries kFlushGraceNs, the same single judgment point the
        // send loop's log line and StaleFlushDecide use - nothing here has its own slack).
        if (!FrameExpiredForSend(predictedDisplayNs, nowNs)) {
            prev.dropStreak = 0;  // rule 5: a frame that goes out ends the streak
            return {true, false, false, false, false};
        }
        g.requestKeyframe = due(prev.lastKeyframeReqNs, nowNs, kFlushKeyframeThrottleNs);
        if (g.requestKeyframe) prev.lastKeyframeReqNs = nowNs;
        prev.chainBroken = true;  // this drop makes its descendants undecodable
    }
    // Rule 4: the A46 breaker, re-targeted. It used to force the current frame through
    // after a bounded streak - under rule 2 that frame is exactly the broken P frame
    // this gate exists to stop. Force the repair request instead and keep dropping.
    if (++prev.dropStreak >= kStaleFlushBreakerDrops) {
        prev.dropStreak = 0;
        g.requestKeyframe = true;
        g.breaker = true;
    }
    // A72 review: the log stamp is taken here, on the path that actually drops. Stamping
    // it while judging a frame that then goes out would consume the next second's flush
    // line, and the first line of a burst - the one that proves the burst happened -
    // would be silently missing.
    g.log = due(prev.lastLogNs, nowNs, kFlushLogThrottleNs);
    if (g.log) prev.lastLogNs = nowNs;
    return g;
}

}  // namespace rc
