#include "rc/discovery.h"

#include <cstring>
#include <utility>

#include "nlohmann/json.hpp"

namespace rc {

using nlohmann::json;

static std::vector<uint8_t> Build(DiscoveryType type, const std::string& body) {
    DiscoveryHeader h{kDiscoveryMagic, kProtocolVersion, uint8_t(type), 0};
    std::vector<uint8_t> out(sizeof(h) + body.size());
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), body.data(), body.size());
    return out;
}

std::vector<uint8_t> BuildProbe(const DiscoveryProbe& probe) {
    json j{{"deviceId", probe.deviceId}, {"deviceName", probe.deviceName}, {"nonce", probe.nonce}};
    return Build(DiscoveryType::Probe, j.dump());
}

std::vector<uint8_t> BuildAnnounce(const DiscoveryAnnounce& a) {
    json j{{"serverId", a.serverId},   {"name", a.name},
           {"version", a.version},     {"os", a.os},
           {"gpu", a.gpu},             {"port", a.streamPort},
           {"accountLinked", a.accountLinked},  // ROUND20 T1 (B1): the account name no longer leaves the PC
           {"busy", a.busy},           {"nonce", a.nonce}};
    return Build(DiscoveryType::Announce, j.dump());
}

uint8_t ParseDiscoveryType(const uint8_t* data, size_t size) {
    if (size < sizeof(DiscoveryHeader)) return 0;
    DiscoveryHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kDiscoveryMagic || h.protocolVersion != kProtocolVersion) return 0;
    return h.type;
}

static bool Body(const uint8_t* data, size_t size, json& out) {
    try {
        out = json::parse(reinterpret_cast<const char*>(data) + sizeof(DiscoveryHeader),
                          reinterpret_cast<const char*>(data) + size);
        return out.is_object();
    } catch (...) {
        return false;
    }
}

bool ParseProbe(const uint8_t* data, size_t size, DiscoveryProbe& out) {
    if (ParseDiscoveryType(data, size) != uint8_t(DiscoveryType::Probe)) return false;
    json j;
    if (!Body(data, size, j)) return false;
    // A51: json::value() throws on a type mismatch, and this runs on a datagram anyone on
    // the LAN can send - {"deviceId":123} used to reach std::terminate and kill the process.
    // Parse into a local so a rejected packet also cannot leave half of `out` overwritten.
    DiscoveryProbe parsed;
    try {
        parsed.deviceId = j.value("deviceId", std::string{});
        parsed.deviceName = j.value("deviceName", std::string{});
        parsed.nonce = j.value("nonce", uint32_t{0});
    } catch (...) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool ParseAnnounce(const uint8_t* data, size_t size, DiscoveryAnnounce& out) {
    if (ParseDiscoveryType(data, size) != uint8_t(DiscoveryType::Announce)) return false;
    json j;
    if (!Body(data, size, j)) return false;
    DiscoveryAnnounce parsed;
    try {
        parsed.serverId = j.value("serverId", std::string{});
        parsed.name = j.value("name", std::string{});
        parsed.version = j.value("version", std::string{});
        parsed.os = j.value("os", std::string{});
        parsed.gpu = j.value("gpu", std::string{});
        // Read the port as int: asking for uint16_t made an out-of-range value throw, and a
        // rejected announce just meant "PC not found" instead of "PC found with a bad port".
        const int port = j.value("port", int(kStreamPort));
        if (port <= 0 || port > 65535) return false;
        parsed.streamPort = uint16_t(port);
        parsed.accountLinked = j.value("accountLinked", false);
        parsed.accountName = j.value("accountName", std::string{});
        parsed.busy = j.value("busy", false);
        parsed.nonce = j.value("nonce", uint32_t{0});
    } catch (...) {
        return false;
    }
    if (parsed.serverId.empty()) return false;
    out = std::move(parsed);
    return true;
}

}  // namespace rc
