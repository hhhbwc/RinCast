// The one place that decides whether a Hello from the network may open a stream, and where its
// fields are read out. Kept free of sockets, settings and Windows so the rules can be exercised
// directly (tools/hello-trust-test/main.cpp) instead of only through a live handshake.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace rcs {

// Reads one string field out of a Hello's JSON. nlohmann's value() throws when the stored type is
// not the one asked for, and this runs in the receive thread before any authorization, where an
// uncaught throw is std::terminate: one crafted LAN packet would kill the streamer. A field of the
// wrong type is treated as absent. (Same class as the A51 discovery fix.)
inline std::string StrField(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

struct HelloTrustQuery {
    bool requireApproval = true;   // Settings::requireApproval
    bool approvalLive = false;     // an "Allow" on the PC is still inside its window
    std::string_view deviceId;     // what the Hello claims
    std::string_view deviceKey;    // identity it proved (hex); empty when the Hello had no proof
    std::string_view approvalKey;  // identity that asked when the PC showed the prompt
};

// `devices` is Settings::trustedDevices: anything whose elements have .id and .publicKey
// comparable with string_view.
template <class Devices>
bool HelloIsTrusted(const HelloTrustQuery& q, const Devices& devices) {
    if (!q.requireApproval) return true;
    for (const auto& d : devices) {
        if (d.id != q.deviceId) continue;
        // An entry without a pinned key cannot authenticate anyone. Honouring it would let the
        // first stranger that repeats the ID take the slot - and App::OnSessionStarted would then
        // pin that stranger's key for good, so the real headset could never get back in. Such an
        // entry is upgraded by approving the headset once, which is what returning false here
        // leads to. (Every headset proves an identity since first launch, so this costs the
        // legitimate one a single click and nothing else.)
        if (!d.publicKey.empty() && d.publicKey == q.deviceKey) return true;
    }
    // "Allow" applies to the headset that asked, not to anyone repeating its ID: an approval
    // counts only against a proof carried by this very Hello.
    return q.approvalLive && !q.deviceKey.empty() && q.approvalKey == q.deviceKey;
}

// What to remember about who is asking, so the PC's "Allow" binds to that identity. A Hello
// without a proof never overwrites it: the packet that writes this value is the same packet that
// is later compared against it, so a stranger could otherwise bind a live approval to itself by
// sending an unproven Hello first and a proven one second.
inline std::string_view NextApprovalKey(std::string_view current, std::string_view provenKey) {
    return provenKey.empty() ? current : provenKey;
}

// Hello fields are strings straight off the network and they end up in the log, in the web UI and
// in a Reject sent back, so bound them and keep control characters out of the log.
//
// The cut must land on a UTF-8 character boundary: json::dump() throws on invalid UTF-8 (its
// default error handler is strict) and SendReject has no try/catch, so a byte-wise cut would turn
// a long name into a process-killing exception.
inline std::string ClampField(std::string_view in, size_t max) {
    size_t n = in.size() < max ? in.size() : max;
    while (n > 0 && n < in.size() && (static_cast<unsigned char>(in[n]) & 0xC0) == 0x80) --n;
    std::string out(in.substr(0, n));
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) c = ' ';
    }
    return out;
}

}  // namespace rcs
