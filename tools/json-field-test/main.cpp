// json-field-test: every read of a JSON field that came off the network goes through
// pc/streamer/src/json_field.h, and none of them may throw. nlohmann's value()/get() throw
// type_error on a wrong-typed field, and these reads run on the receive thread (and in
// ClientSession's constructor, straight out of the handshake), where an uncaught throw is
// std::terminate - one crafted packet would kill the streamer.
//
//   json-field-test      exit 0 = every case held
#include <cfloat>
#include <cstdio>
#include <string>

#include "json_field.h"
#include "nlohmann/json.hpp"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using nlohmann::json;

}  // namespace

int main() {
    const json j = json::parse(R"({
        "text": "hello",
        "num": 5,
        "numFloat": 5.9,
        "numString": "5",
        "huge": 1e300,
        "negative": -3,
        "big": 4294967296,
        "maxUint": 4294967295,
        "yes": true,
        "one": 1,
        "zero": 0,
        "yesString": "true",
        "nothing": null,
        "obj": {"a": 1},
        "arr": [1, "x", 3, null],
        "fov2x2": [[1, 2], [3, 4]]
    })");

    // StrField
    Expect(rcs::StrField(j, "text") == "hello", "string: comes through");
    Expect(rcs::StrField(j, "num").empty(), "string: a number reads as absent");
    Expect(rcs::StrField(j, "nothing").empty(), "string: null reads as absent");
    Expect(rcs::StrField(j, "obj").empty(), "string: an object reads as absent");
    Expect(rcs::StrField(j, "missing").empty(), "string: a missing key reads as absent");

    // IntField
    Expect(rcs::IntField(j, "num", -1) == 5, "int: a number comes through");
    Expect(rcs::IntField(j, "numFloat", -1) == 5, "int: a float truncates");
    Expect(rcs::IntField(j, "numString", -1) == -1, "int: a string reads as the fallback");
    Expect(rcs::IntField(j, "huge", -1) == -1, "int: out of range reads as the fallback");
    Expect(rcs::IntField(j, "negative", 0) == -3, "int: negative numbers come through");
    Expect(rcs::IntField(j, "missing", 7) == 7, "int: a missing key reads as the fallback");

    // UIntField
    Expect(rcs::UIntField(j, "negative", 9u) == 9u, "uint: negative reads as the fallback");
    Expect(rcs::UIntField(j, "maxUint", 9u) == 4294967295u, "uint: the largest uint32 comes through");
    Expect(rcs::UIntField(j, "big", 9u) == 9u, "uint: past uint32 reads as the fallback");

    // FloatField / FloatAt
    Expect(rcs::FloatField(j, "numFloat", 0.f) > 5.89f && rcs::FloatField(j, "numFloat", 0.f) < 5.91f,
           "float: comes through");
    Expect(rcs::FloatField(j, "huge", 1.f) == 1.f, "float: a double past float range reads as the fallback");
    Expect(rcs::FloatField(j, "numString", 2.f) == 2.f, "float: a string reads as the fallback");
    Expect(rcs::FloatAt(j["arr"], 0, -1.f) == 1.f, "float at: a number element comes through");
    Expect(rcs::FloatAt(j["arr"], 1, -1.f) == -1.f, "float at: a string element reads as the fallback");
    Expect(rcs::FloatAt(j["arr"], 3, -1.f) == -1.f, "float at: null reads as the fallback");
    Expect(rcs::FloatAt(j["arr"], 99, -1.f) == -1.f, "float at: past the end reads as the fallback");
    Expect(rcs::FloatAt(j["fov2x2"][0], 3, -1.f) == -1.f, "float at: a short row reads as the fallback");
    Expect(rcs::FloatAt(j["num"], 0, -1.f) == -1.f, "float at: a non-array reads as the fallback");

    // BoolField
    Expect(rcs::BoolField(j, "yes", false), "bool: true comes through");
    Expect(!rcs::BoolField(j, "zero", true), "bool: 0 written as a number reads as false");
    Expect(rcs::BoolField(j, "one", false), "bool: 1 written as a number reads as true");
    Expect(rcs::BoolField(j, "yesString", true), "bool: a string reads as the fallback");
    Expect(!rcs::BoolField(j, "nothing", false), "bool: null reads as the fallback");
    Expect(rcs::BoolField(j, "missing", true), "bool: a missing key reads as the fallback");

    // ObjectField
    Expect(rcs::ObjectField(j, "obj").is_object() && rcs::ObjectField(j, "obj")["a"] == 1,
           "object: comes through");
    Expect(rcs::ObjectField(j, "num").empty(), "object: a number reads as an empty object");
    Expect(rcs::ObjectField(j, "missing").empty(), "object: a missing key reads as an empty object");

    // The whole point: none of the above ever throws, whatever the shape of the input.
    bool threw = false;
    try {
        (void)rcs::StrField(j, "num");
        (void)rcs::IntField(j, "numString", 0);
        (void)rcs::UIntField(j, "negative", 0u);
        (void)rcs::FloatField(j, "huge", 0.f);
        (void)rcs::BoolField(j, "yesString", false);
        (void)rcs::ObjectField(j, "arr").size();
        (void)rcs::FloatAt(j["fov2x2"], 5, 0.f);
        (void)rcs::ClampField(rcs::StrField(j, "text"), 64);
    } catch (...) {
        threw = true;
    }
    Expect(!threw, "no helper throws, whatever the field holds");

    // ClampField bounds what goes on to the log and the web UI.
    Expect(rcs::ClampField(std::string(200, 'x'), 64).size() == 64, "clamp: stops at the limit");
    Expect(rcs::ClampField("a\nb", 64) == "a b", "clamp: a newline cannot forge a log line");

    // A91: the cloud client walks server-supplied "events"/"peers" arrays and
    // keeps named fallbacks on its error strings. value(key, json::array())
    // throws on `{"events":{}}`, and value(key, std::string(fallback)) throws
    // on `{"message":123}` - both on worker threads, both fatal.
    const json cloud = json::parse(R"({"arr":[1,2],"notArr":{},"text":"hello","num":123})");
    Expect(rcs::ArrayField(cloud, "arr").size() == 2, "A91 array: a real array passes through");
    Expect(rcs::ArrayField(cloud, "notArr").empty(), "A91 array: an object reads as empty, not a throw");
    Expect(rcs::ArrayField(cloud, "text").empty(), "A91 array: a string reads as empty");
    Expect(rcs::ArrayField(json::object(), "arr").empty(), "A91 array: a missing key reads as empty");
    Expect(rcs::StrFieldOr(cloud, "text", "fallback") == "hello", "A91 string-or: a real string passes through");
    Expect(rcs::StrFieldOr(cloud, "missing", "fallback") == "fallback", "A91 string-or: missing uses the fallback");
    Expect(rcs::StrFieldOr(cloud, "num", "fallback") == "fallback", "A91 string-or: a number uses the fallback");

    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
