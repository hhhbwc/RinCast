// The one place that decides whether a Hello from the network may open a stream. Kept free of
// sockets, settings, JSON and Windows so the rule can be exercised directly
// (tools/hello-trust-test/main.cpp) instead of only through a live handshake. Reading the fields
// out of a Hello lives next door, in ../json_field.h.
#pragma once

#include <string_view>

namespace rcs {

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

}  // namespace rcs
