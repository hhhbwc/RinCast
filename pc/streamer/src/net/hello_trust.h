// The one place that decides whether a Hello from the network may open a stream. Kept free of
// sockets, settings, JSON and Windows so the rule can be exercised directly
// (tools/hello-trust-test/main.cpp) instead of only through a live handshake. Reading the fields
// out of a Hello lives next door, in ../json_field.h.
#pragma once

#include <cstddef>
#include <string_view>

namespace rcs {

// Two hex keys are the same key. HexEncode writes lower case, but a settings file written by an
// older build (or by hand) may hold upper case, and that must not cost the headset its trust.
// An empty key never matches anything, including another empty one: "no identity" is not an
// identity, and every caller here would otherwise have to remember that on its own.
inline bool SameKey(std::string_view a, std::string_view b) {
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
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
        if (!d.publicKey.empty() && SameKey(d.publicKey, q.deviceKey)) return true;
    }
    // "Allow" applies to the headset that asked, not to anyone repeating its ID: an approval
    // counts only against a proof carried by this very Hello.
    return q.approvalLive && !q.deviceKey.empty() && SameKey(q.approvalKey, q.deviceKey);
}

// Whether this Hello may become the identity the pending approval is bound to.
//
// A second identity that answers to the same device ID must not take the binding over while the
// PC is waiting for (or has just granted) the answer: the click blessed the headset that was on
// screen, and only the key tells the two apart - the name and model are whatever the sender
// claims. Once the binding is stale (the prompt went unanswered long enough) it may be replaced,
// so a headset that reinstalled itself and got a new identity is not locked out for good.
inline bool MayBindApproval(std::string_view bound, std::string_view provenKey, bool awaitingOrGranted) {
    if (provenKey.empty()) return false;  // no proof, nothing to bind
    if (bound.empty() || SameKey(bound, provenKey)) return true;
    return !awaitingOrGranted;
}

// What to remember about who is asking, so the PC's "Allow" binds to that identity. A Hello
// without a proof never overwrites it: the packet that writes this value is the same packet that
// is later compared against it, so a stranger could otherwise bind a live approval to itself by
// sending an unproven Hello first and a proven one second.
inline std::string_view NextApprovalKey(std::string_view current, std::string_view provenKey) {
    return provenKey.empty() ? current : provenKey;
}

}  // namespace rcs
