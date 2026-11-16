#pragma once

// R4.2 (D-026): the user's bitrate setting is a WIRE ceiling - it is what the link
// has to carry, fragment headers and FEC parity included. The encoder works in
// encoded bytes, so the target it is given must be the wire budget converted into
// encoded bytes:
//
//     encoderTarget = wireCeiling / overhead
//
// where overhead = wireBytes / encodedBytes is the structural factor of the
// transport (one fragment header per datagram plus one parity datagram per FEC
// group, ~1.07 at 5% FEC). Without the conversion the link carries ~7% more than
// the user asked for: measured 106.2 Mbps of wire against a 100 Mbps setting
// (glm-toolkit/out/r51-e3-nofb-p2-wire.csv).
//
// Two properties are baked in here, both measured (D-026):
//
//   * The factor is STRUCTURAL, not content-dependent (it depends on the frame size
//     and the FEC group size, not on the picture). So it can be applied
//     PROACTIVELY: the target does not wait for a breach and does not depend on the
//     headset's feedback cadence. The first version derived the cap from a breach
//     counter that only the feedback path could feed, so on a link that reports
//     slowly the encoder was never told about the ceiling at all (measured:
//     appliedKbps stayed at 100000).
//
//   * The target must be published with HYSTERESIS. Republishing it every frame
//     reconfigured NVENC ~10x/s, and every reconfigure resets the rate control:
//     the 1 s wire peak got WORSE (127.9 vs 118.8 Mbps on the same command). The
//     cap now only moves when it moves by at least 1/50 of the ceiling.
//
// Pure arithmetic plus a small ring of entries: no clock, no locks, no I/O. The
// sender calls it from the send thread only.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace rc {

class WireCeiling {
public:
    struct Snapshot {
        uint32_t encoderTargetKbps = 0;  // the target the encoder should use now
        uint64_t sumWire = 0;            // wire bytes in the trailing second
        uint64_t sumEncoded = 0;         // encoded bytes in the trailing second
        size_t entries = 0;              // frames in the trailing second
        double overhead = 1.0;           // sumWire / sumEncoded
        uint32_t breaches = 0;           // frames that saw the trailing second over quota
        bool targetChanged = false;      // the encoder has to be re-told
    };

    // One frame's accounting. `wireCeilingKbps` == 0 disables the controller (the
    // follow modes have no wire ceiling). Returns true when the encoder target
    // changed and has to be pushed.
    bool OnFrame(int64_t nowNs, size_t wireBytes, size_t encodedBytes, uint32_t wireCeilingKbps,
                 Snapshot& out) {
        out = Snapshot{};
        if (!wireCeilingKbps) {
            Reset();
            return false;
        }
        m_window.push_back({nowNs, wireBytes, encodedBytes});
        m_sumWire += wireBytes;
        m_sumEnc += encodedBytes;
        while (!m_window.empty() && nowNs - m_window.front().ns >= kWindowNs) {
            m_sumWire -= m_window.front().wire;
            m_sumEnc -= m_window.front().encoded;
            m_window.pop_front();
        }
        out.entries = m_window.size();
        out.sumWire = m_sumWire;
        out.sumEncoded = m_sumEnc;
        out.overhead = m_sumEnc ? double(m_sumWire) / double(m_sumEnc) : 1.0;
        // The quota is the user's ceiling with the 2% tolerance the acceptance
        // criteria allow: kbps * 1000 / 8 * 1.02 bytes per second.
        const uint64_t quota = uint64_t(wireCeilingKbps) * 1275 / 10;
        if (m_sumWire > quota) m_breaches++;
        out.breaches = m_breaches;

        uint32_t candidate = 0;
        if (m_sumEnc > 0 && out.overhead > 1.0) {
            const uint64_t cap = uint64_t(double(wireCeilingKbps) / out.overhead);
            candidate = uint32_t(std::min<uint64_t>(std::max<uint64_t>(cap, wireCeilingKbps / 2),
                                                    wireCeilingKbps));
        }
        out.encoderTargetKbps = m_target;
        // Hysteresis: a target that wobbles by a percent reconfigured the encoder
        // ten times a second and made the peak worse (D-026).
        const uint32_t band = std::max<uint32_t>(1000, wireCeilingKbps / 50);
        if (std::abs(int(candidate) - int(m_target)) >= int(band)) {
            m_target = candidate;
            out.encoderTargetKbps = m_target;
            out.targetChanged = true;
            return true;
        }
        return false;
    }

    void Reset() {
        m_window.clear();
        m_sumWire = 0;
        m_sumEnc = 0;
        m_breaches = 0;
        m_target = 0;
    }

    size_t Entries() const { return m_window.size(); }
    double Overhead() const { return m_sumEnc ? double(m_sumWire) / double(m_sumEnc) : 1.0; }

    // The quota in bytes per second, for callers that want to compare against it.
    static uint64_t QuotaBytes(uint32_t wireCeilingKbps) { return uint64_t(wireCeilingKbps) * 1275 / 10; }

private:
    struct Entry {
        int64_t ns;
        size_t wire;
        size_t encoded;
    };
    static constexpr int64_t kWindowNs = 1'000'000'000LL;

    std::deque<Entry> m_window;
    uint64_t m_sumWire = 0;
    uint64_t m_sumEnc = 0;
    uint32_t m_breaches = 0;
    uint32_t m_target = 0;
};

}  // namespace rc
