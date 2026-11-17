#pragma once

// ROUND19 T3 (原 L5-L7-AUTOSCALE, frozen design): the decode-side autoscale ladder.
//
// The headset's kStats carries its decode cost; the PC knows its own dropped-frame
// average and the handshake's decode-fps hint. When decode pressure holds for long
// enough, the encoded frame size steps DOWN one rung of a fixed ladder — and only
// down. Nothing here ever raises the scale: only the user's manual reset does.
//
// Signals (all pre-existing, no protocol keys added):
//   - kStats decodeMs: the headset's per-frame decode average;
//   - the PC's own dropped-frame average (video_sender.h droppedFps);
//   - maxFpsAtMaxRes (handshake) shapes the decode budget, computed by the owner:
//     it is the headset's own estimate of what it can decode at large sizes.
// A missing signal is never evidence: a negative decodeMs or drop average counts as
// "no pressure this tick", so an old headset or a stats gap can never fire the ladder.
//
// Hard constraints from the task card: the pressure must hold continuously for
// `overNs` (5 s) before a step; a step at least `cooldownNs` (30 s) must separate
// steps — the encoder rebuild on each rung is a real cost (the 513f150 rebuild-storm
// precedent) — and with the switch off nothing happens at all.

#include <cstdint>

namespace rc {

class L7Autoscale {
public:
    static constexpr int kLevels = 4;
    static constexpr int kLevelPct[kLevels] = {100, 85, 67, 50};

    struct Config {
        double decodeBudgetMs = 20.0;  // per-frame decode cost the headset sustains
        double droppedFps = 3.0;       // PC-side dropped-frame average that also counts
        int64_t overNs = 5'000'000'000LL;
        int64_t cooldownNs = 30'000'000'000LL;
    };

    struct Result {
        int level = 0;     // the level after this tick
        bool changed = false;  // true exactly on the tick that stepped down
    };

    void Configure(const Config& c) { m_cfg = c; }
    int Level() const { return m_level; }
    int Pct() const { return kLevelPct[m_level]; }

    // The user's manual reset is the only way back up (and the only way to clear the
    // cooldown, so a fresh episode can step again after a full 5 s of pressure).
    void Reset() {
        m_level = 0;
        m_over = false;
        m_overSinceNs = 0;
        m_lastChangeNs = 0;
    }

    Result Update(double decodeMs, double droppedFps, int64_t nowNs) {
        Result r;
        r.level = m_level;
        const bool decodeOver = decodeMs > m_cfg.decodeBudgetMs;
        const bool dropsOver = droppedFps > m_cfg.droppedFps;
        if (!decodeOver && !dropsOver) {
            m_over = false;  // the 5 s must be CONTINUOUS pressure
            return r;
        }
        if (!m_over) {
            m_over = true;
            m_overSinceNs = nowNs;
        }
        if (nowNs - m_overSinceNs < m_cfg.overNs) return r;
        if (m_lastChangeNs && nowNs - m_lastChangeNs < m_cfg.cooldownNs) return r;
        if (m_level >= kLevels - 1) return r;  // bottom of the ladder, stays
        m_level++;
        m_lastChangeNs = nowNs;
        m_over = false;
        r.changed = true;
        r.level = m_level;
        return r;
    }

private:
    Config m_cfg{};
    int m_level = 0;             // index into kLevelPct; 0 = 100 %
    bool m_over = false;         // currently above a threshold
    int64_t m_overSinceNs = 0;   // when the current over-streak began
    int64_t m_lastChangeNs = 0;  // 0 = no step taken yet
};

}  // namespace rc
