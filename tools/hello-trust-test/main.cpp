// hello-trust-test: pins down who a Hello may open a stream for, and what may be read out of one
// (rcs::HelloIsTrusted / StrField / ClampField, in pc/streamer/src/net/hello_trust.h). None of it
// touches a socket, so it can be checked without a headset - which is the point: A56 found two
// paths where a LAN device could take over a trusted headset's slot by repeating its device ID,
// and neither of them is visible from a build log.
//
//   hello-trust-test      exit 0 = every case held
#include <cstdio>
#include <string>
#include <vector>

#include "net/hello_trust.h"
#include "nlohmann/json.hpp"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

struct Entry {
    std::string id;
    std::string publicKey;
};

const std::string kId = "9f1c0a7e-3b41-4d2e-8a55-0c1d2e3f4a5b";
const std::string kKeyA(64, 'a');  // the headset this PC pinned
const std::string kKeyB(64, 'b');  // someone else on the same LAN

bool Trusted(bool requireApproval, const std::string& id, const std::string& key, bool approvalLive,
             const std::string& approvalKey, const std::vector<Entry>& devices) {
    rcs::HelloTrustQuery q;
    q.requireApproval = requireApproval;
    q.approvalLive = approvalLive;
    q.deviceId = id;
    q.deviceKey = key;
    q.approvalKey = approvalKey;
    return rcs::HelloIsTrusted(q, devices);
}

}  // namespace

int main() {
    const std::vector<Entry> pinned{{kId, kKeyA}};
    const std::vector<Entry> legacy{{kId, ""}};  // a settings file written before identities
    const std::vector<Entry> none;

    // "Ask before a new headset connects" off: anyone local is let in, as before.
    Expect(Trusted(false, kId, "", false, "", none), "approval off: an unproven Hello is trusted");

    // The pinned headset, and only it.
    Expect(Trusted(true, kId, kKeyA, false, "", pinned), "pinned: the headset gets in");
    Expect(!Trusted(true, kId, kKeyB, false, "", pinned), "pinned: another identity is not the headset");
    Expect(!Trusted(true, kId, "", false, "", pinned),
           "pinned: repeating the ID without a proof is not enough");
    Expect(!Trusted(true, "another-id", kKeyA, false, "", pinned), "pinned: the key alone is not enough");

    // A56 F1: an entry without a stored key must not authenticate anyone, or the first stranger
    // that repeats the ID takes the slot and App::OnSessionStarted pins its key for good.
    Expect(!Trusted(true, kId, "", false, "", legacy), "no pinned key: an unproven Hello is refused");
    Expect(!Trusted(true, kId, kKeyB, false, "", legacy), "no pinned key: a stranger's proof is refused");

    // Approving once is what upgrades such an entry - and that identity is the one let in.
    Expect(Trusted(true, kId, kKeyB, true, kKeyB, legacy), "approved once: that identity gets in");

    // A56 F2: an approval counts only against a proof carried by this very Hello, so an unproven
    // Hello cannot bind it to itself.
    Expect(Trusted(true, kId, kKeyA, true, kKeyA, none), "approval: the asking identity gets in");
    Expect(!Trusted(true, kId, "", true, "", none), "approval: an empty binding matches nothing");
    Expect(!Trusted(true, kId, "", true, kKeyA, none), "approval: no proof, no entry");
    Expect(!Trusted(true, kId, kKeyB, true, kKeyA, none), "approval: another identity is not the approved one");

    // NextApprovalKey: a Hello without a proof never rewrites the binding. The packet that writes
    // this value is the same one compared against it, so rewriting it would be self-service.
    Expect(rcs::NextApprovalKey("", kKeyA) == kKeyA, "binding: a proven Hello writes it");
    Expect(rcs::NextApprovalKey(kKeyA, "") == kKeyA, "binding: an unproven Hello leaves it alone");
    Expect(rcs::NextApprovalKey(kKeyA, kKeyB) == kKeyB, "binding: a new proven asker replaces it");
    Expect(rcs::NextApprovalKey("", "") == "", "binding: nothing to remember");

    // ClampField: bounded, no control characters, and never cut inside a UTF-8 character
    // (json::dump() throws on invalid UTF-8 and SendReject has no try/catch).
    const std::string sixtyFour(64, 'x');
    Expect(rcs::ClampField(sixtyFour + "tail", 64) == sixtyFour, "clamp: the field stops at its limit");
    Expect(rcs::ClampField("ab\ncd\tef", 64) == "ab cd ef", "clamp: control characters become spaces");
    const std::string eAcute = "\xC3\xA9";  // one 2-byte character
    Expect(rcs::ClampField(eAcute, 1).empty(), "clamp: a cut inside a character drops it");
    Expect(rcs::ClampField(eAcute, 2) == eAcute, "clamp: a cut on a boundary keeps it");
    std::string cjk;
    for (int i = 0; i < 30; ++i) cjk += "\xE7\x94\xB5";  // 30 x 3-byte characters
    const std::string cut = rcs::ClampField(cjk, 64);
    Expect(cut.size() == 63, "clamp: 64 bytes of 3-byte characters ends at 63");
    bool dumps = true;
    try {
        (void)nlohmann::json{{"name", cut}}.dump();
    } catch (...) {
        dumps = false;
    }
    Expect(dumps, "clamp: the result survives json::dump()");

    // StrField: a field of the wrong JSON type reads as absent instead of throwing. This runs in
    // the receive thread before any authorization, where an uncaught throw is std::terminate -
    // one crafted LAN packet would kill the streamer.
    {
        const auto j = nlohmann::json::parse(
            R"({"deviceId":42,"deviceName":"ok","model":null,"appVersion":["a","b"],"nonce":true})");
        bool threw = false;
        try {
            Expect(rcs::StrField(j, "deviceId").empty(), "fields: a number reads as absent");
            Expect(rcs::StrField(j, "deviceName") == "ok", "fields: a string comes through");
            Expect(rcs::StrField(j, "model").empty(), "fields: null reads as absent");
            Expect(rcs::StrField(j, "appVersion").empty(), "fields: an array reads as absent");
            Expect(rcs::StrField(j, "nonce").empty(), "fields: a bool reads as absent");
            Expect(rcs::StrField(j, "deviceKey").empty(), "fields: a missing key reads as absent");
        } catch (...) {
            threw = true;
        }
        Expect(!threw, "fields: nothing throws out of the receive thread");
    }

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
