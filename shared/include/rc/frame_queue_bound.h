// A23 (TASK-QUEUE-BOUND-2026-11-22): the decode queue bound.
//
// The user measured the AB9 stream: when the queue in front of the frames overloads,
// frames queue for hundreds of milliseconds and the latency spikes (total 593 ms with
// queue 151 ms; 410 ms with queue 479 ms). The card's trade: the cost of an overload
// becomes a momentary freeze (a keyframe quickly resumes the picture) instead of half
// a second of queued latency. Frames that queued longer than kQueueDropMs between their
// arrival (receivedNs) and the decode submission point are dropped, not decoded, and
// the headset asks the PC for a keyframe - throttled, so an overload doesn't turn into
// a request storm.
//
// Pure decision logic in its own header so core_tests can pin the threshold, its
// boundary and the throttle without a decoder or a stream.
#pragma once

#include <cstdint>

namespace rc {

// A frame that queued longer than this (arrival -> decode submission) is dropped
// instead of decoded. The boundary is explicit: exactly kQueueDropMs passes ("超过"
// in the card is strictly greater than).
constexpr int64_t kQueueDropMs = 100;

// While frames keep dropping, a keyframe request goes out on the first drop and
// then every kQueueDropKeyframeEvery-th drop (1, 6, 11 ...). A23 asked only on
// 5, 10, 15 ...; with the streak reset whenever a frame passed the gate, a drop
// of 1-4 frames interleaved with passing frames never asked at all and broke the
// reference chain permanently under the 100 ms cap (A46).
constexpr uint32_t kQueueDropKeyframeEvery = 5;

// True when the frame's queue time from arrival to the decode submission point
// exceeds the bound. `receivedNs` is the frame's arrival time (the ReceivedFrame's
// completeNs, the receivedNs the decoder books for it). A missing timestamp
// (receivedNs <= 0) and a negative age (nowNs < receivedNs, a clock anomaly) keep
// the frame: the age is unknowable, and dropping on it could freeze a stream the
// gate was never meant to touch.
//
// A46: an IDR keyframe is the recovery frame itself - it is much bigger than a P
// frame, so encoding and queueing take longer. Dropping it at the queue gate is
// what makes a broken reference chain unrecoverable: the rebuilt IDR is thrown
// away every time while the small P frames after it pass and keep the chain
// broken. isKeyframe (default false keeps every existing caller compatible) makes
// the gate never discard a keyframe on the age rule alone.
inline bool FrameQueueExpired(int64_t nowNs, int64_t receivedNs, bool isKeyframe = false) {
    if (isKeyframe) return false;
    if (receivedNs <= 0) return false;
    return nowNs - receivedNs > kQueueDropMs * 1'000'000;  // ms -> ns
}

// True when this drop is the one that carries the keyframe request. `dropsSinceReset`
// counts consecutive drops (1-based; the caller resets it whenever a frame passes the
// gate). A46: the first drop of a streak asks immediately (1), then every
// kQueueDropKeyframeEvery-th (6, 11, 16 ...). Waiting until the 5th let a 1-4 drop
// streak that ended in a passing frame ask nothing at all. A 0 argument never asks
// (defensive; the caller never passes it).
inline bool QueueDropRequestKeyframe(uint32_t dropsSinceReset) {
    return dropsSinceReset != 0 &&
           (dropsSinceReset == 1 || dropsSinceReset % kQueueDropKeyframeEvery == 1);
}

}  // namespace rc
