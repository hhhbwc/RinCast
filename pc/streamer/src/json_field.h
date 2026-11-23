// Reading fields out of JSON that came off the network.
//
// nlohmann's value()/get() throw type_error when the stored type is not the one asked for - and
// these reads happen on the receive thread (and in ClientSession's constructor, straight out of
// the handshake), where an uncaught throw is std::terminate. One crafted packet would kill the
// streamer, which is the same class of bug A51 fixed in discovery and A56 in the Hello.
// Every helper here answers with the fallback instead of throwing.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace rcs {

// A string field; anything else reads as absent. (nlohmann also validates UTF-8 on parse, so a
// string that arrives is already valid.)
inline std::string StrField(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Any number; a string, bool, array or object reads as the fallback. Accepts ints for floats and
// the other way round, because the sender's JSON writer decides that, not us.
inline double NumberField(const nlohmann::json& j, const char* key, double fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const double v = it->get<double>();
    return std::isfinite(v) ? v : fallback;
}

// A number that also fits in a float: 1e300 is a valid JSON number and an infinity as a float,
// which downstream maths would happily propagate.
inline float FloatField(const nlohmann::json& j, const char* key, float fallback) {
    const double v = NumberField(j, key, double(fallback));
    return std::abs(v) <= double(std::numeric_limits<float>::max()) ? static_cast<float>(v) : fallback;
}

inline int IntField(const nlohmann::json& j, const char* key, int fallback) {
    const double v = NumberField(j, key, double(fallback));
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return fallback;
    return static_cast<int>(v);
}

// A92: a 64-bit number for the control channel's nanosecond offsets. double keeps
// only 53 bits of mantissa, but the values here are ~1e9 ns, far below that
// limit; the range check still rejects anything that would wrap on cast.
inline int64_t Int64Field(const nlohmann::json& j, const char* key, int64_t fallback) {
    const double v = NumberField(j, key, double(fallback));
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return fallback;
    return static_cast<int64_t>(v);
}

inline uint32_t UIntField(const nlohmann::json& j, const char* key, uint32_t fallback) {
    const double v = NumberField(j, key, double(fallback));
    if (!(v >= 0.0 && v <= 4294967295.0)) return fallback;
    return static_cast<uint32_t>(v);
}

// A boolean, or a 0/1 written as a number: a headset that writes {"down":1} must not take the PC
// down with it.
inline bool BoolField(const nlohmann::json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    return fallback;
}

// An object field; a missing key or any other type reads as an empty object, so callers can
// iterate without a second check.
inline const nlohmann::json& ObjectField(const nlohmann::json& j, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : kEmpty;
}

// A91: a string field with a caller-supplied fallback. The cloud client reads fields whose
// default is a specific non-empty string (an error message, a marker) - StrField alone would
// silently turn those into "" and change what the log says.
inline std::string StrFieldOr(const nlohmann::json& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

// A91: an array field; a missing key or any other type reads as an empty array. The cloud
// event loop iterates this straight from an HTTP response on a worker thread, so a single
// `"events": {}` would throw out of value() and terminate the process.
inline const nlohmann::json& ArrayField(const nlohmann::json& j, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const auto it = j.find(key);
    return it != j.end() && it->is_array() ? *it : kEmpty;
}

// A number field of an array: element i of `arr` when it is a number, else the fallback. Bounds
// are checked first, so a short or oddly shaped array (an fov of 2x2, say) reads as defaults
// instead of indexing past the end.
inline float FloatAt(const nlohmann::json& arr, size_t i, float fallback) {
    if (!arr.is_array() || i >= arr.size() || !arr[i].is_number()) return fallback;
    const double v = arr[i].get<double>();
    if (!std::isfinite(v) || std::abs(v) > double(std::numeric_limits<float>::max())) return fallback;
    return static_cast<float>(v);
}

// Fields straight off the network end up in the log, in the web UI and in a Reject sent back, so
// bound them and keep control characters out of the log.
//
// The cut must land on a UTF-8 character boundary: json::dump() throws on invalid UTF-8 (its
// default error handler is strict) and the reject path has no try/catch, so a byte-wise cut would
// turn a long name into a process-killing exception.
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
