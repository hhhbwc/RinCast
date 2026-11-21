// TASK-DEC-PERCENTILE-2026-11-20 §1.1: the decode-time percentile sampler behind the
// diag line's dec p50/p95 (VD-OBSERVE-DATA §7: that field carried a -1.0/-1.0 placeholder
// the whole run). Observation only: pure aggregation, no allocation, no clock, no I/O.
// Push runs on the decoder's output thread, Reset/Count/Percentile on the GL thread's
// 1 Hz stats tick - a small mutex keeps the reader's snapshot consistent.
#pragma once
#include <algorithm>
#include <cmath>
#include <mutex>

namespace rc {

// 采样器：定容量环形缓冲，push 满后覆盖最旧。
struct DecodeMsSamples {
    static constexpr int kCapacity = 512;  // 72fps x 5s 窗 = 360 样本，512 留裕量

    void Push(float ms) {
        // ms<=0 或非有限值忽略（沿用 fps_ladder.h EncoderRateTracker::Observe 的 isfinite 先例）。
        if (!std::isfinite(ms) || ms <= 0) return;
        std::lock_guard lock(m_mutex);
        m_buf[m_next] = ms;
        if (++m_next == kCapacity) m_next = 0;
        if (m_count < kCapacity) ++m_count;
    }

    void Reset() {
        std::lock_guard lock(m_mutex);
        m_next = 0;
        m_count = 0;
    }

    int Count() const {
        std::lock_guard lock(m_mutex);
        return m_count;
    }

    // 最近一次 Reset 后全部样本的分位；无样本返回 -1.0f（保旧哨兵语义）。
    // nearest-rank：rank = ceil(p/100 * n)（1 基），确定性、不插值。
    // p 不在 (0,100] 时返回 -1.0f（NaN 也落在这里）。
    float Percentile(float p) const {
        if (!(p > 0.f && p <= 100.f)) return -1.0f;
        std::lock_guard lock(m_mutex);
        if (m_count == 0) return -1.0f;
        // 无分配：栈上定容快照 + 就地选择（nth_element）。
        float sorted[kCapacity];
        for (int i = 0; i < m_count; ++i) sorted[i] = m_buf[i];
        const int rank = int(std::ceil((double(p) / 100.0) * double(m_count)));  // 1 基
        std::nth_element(sorted, sorted + (rank - 1), sorted + m_count);
        return sorted[rank - 1];
    }

  private:
    mutable std::mutex m_mutex;
    float m_buf[kCapacity] = {};
    int m_next = 0;   // 环形写位
    int m_count = 0;  // 最近一次 Reset 后的样本数，封顶 kCapacity
};

}  // namespace rc
