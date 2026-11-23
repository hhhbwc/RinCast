#include "settings.h"

#include <algorithm>
#include <functional>

#include "rc/platform.h"

namespace rcs {

json Settings::ToJson(bool forUi) const {
    json devices = json::array();
    for (const auto& d : trustedDevices)
        devices.push_back({{"id", d.id}, {"name", d.name}, {"model", d.model}, {"firstSeen", d.firstSeen},
                           {"lastSeen", d.lastSeen}, {"publicKey", d.publicKey}});
    json j{
        {"serverId", serverId},
        {"pcName", pcName},
        {"startWithWindows", startWithWindows},
        {"startMinimized", startMinimized},
        {"preventSleep", preventSleep},
        {"requireApproval", requireApproval},
        {"desktopInput", desktopInput},
        {"trustedDevices", devices},
        {"codec", codec},
        {"tenBit", tenBit},
        {"vrBitrateMbps", vrBitrateMbps},
        {"desktopBitrateMbps", desktopBitrateMbps},
        {"adaptiveBitrate", adaptiveBitrate},
        {"streamProfile", streamProfile},
        {"profileControlsVideo", profileControlsVideo},
        {"videoPacingMode", videoPacingMode},
        {"qualityProfileCbr", qualityProfileCbr},
        {"encodeScalePct", encodeScalePct},
        {"l7Autoscale", l7Autoscale},
        {"autoTune", autoTune},
        {"fillMode", fillMode},
        {"pcBitrateWins", pcBitrateWins},
        {"fecPercent", fecPercent},
        {"foveatedEncoding", foveatedEncoding},
        {"adaptiveQuantization", adaptiveQuantization},
        {"twoPassEncoding", twoPassEncoding},
        {"desktopMaxHeight", desktopMaxHeight},
        {"desktopFps", desktopFps},
        {"streamAudio", streamAudio},
        {"micPassthrough", micPassthrough},
        {"switchDefaultAudio", switchDefaultAudio},
        {"playOnPcToo", playOnPcToo},
        {"audioDevices", audioDevices},
        {"openxrRuntime", openxrRuntime},
        {"autoRegisterSteamVrDriver", autoRegisterSteamVrDriver},
        {"cloudUrl", cloudUrl},
        {"allowRemote", allowRemote},
        {"cloudUserName", cloudUserName},
        {"cloudEmail", cloudEmail},
        // A84: the stamp every save carries, so FromJson can tell "this file was
        // written when autoTune still defaulted to off" from "the user chose it".
        // Written as the constant (not a field) so the UI's read-modify-write round
        // trip in app.cpp (merged = ToJson, patch, FromJson) always re-stamps.
        {"settingsSchema", kSettingsSchema},
    };
    if (!forUi) j["cloudRefreshTokenProtected"] = cloudRefreshTokenProtected;
    return j;
}

void Settings::FromJson(const json& j) {
    auto get = [&](const char* key, auto& field) {
        if (j.contains(key) && !j[key].is_null()) {
            try {
                field = j[key].get<std::decay_t<decltype(field)>>();
            } catch (...) {
            }
        }
    };
    get("serverId", serverId);
    get("pcName", pcName);
    get("startWithWindows", startWithWindows);
    get("startMinimized", startMinimized);
    get("preventSleep", preventSleep);
    get("requireApproval", requireApproval);
    get("desktopInput", desktopInput);
    get("codec", codec);
    get("tenBit", tenBit);
    get("vrBitrateMbps", vrBitrateMbps);
    get("desktopBitrateMbps", desktopBitrateMbps);
    get("adaptiveBitrate", adaptiveBitrate);
    get("streamProfile", streamProfile);
    get("profileControlsVideo", profileControlsVideo);
    get("videoPacingMode", videoPacingMode);
    get("qualityProfileCbr", qualityProfileCbr);
    get("encodeScalePct", encodeScalePct);
    get("l7Autoscale", l7Autoscale);
    get("autoTune", autoTune);
    get("fillMode", fillMode);
    get("pcBitrateWins", pcBitrateWins);
    get("fecPercent", fecPercent);
    get("foveatedEncoding", foveatedEncoding);
    get("adaptiveQuantization", adaptiveQuantization);
    get("twoPassEncoding", twoPassEncoding);
    get("desktopMaxHeight", desktopMaxHeight);
    get("desktopFps", desktopFps);
    get("streamAudio", streamAudio);
    get("micPassthrough", micPassthrough);
    get("switchDefaultAudio", switchDefaultAudio);
    get("playOnPcToo", playOnPcToo);
    get("audioDevices", audioDevices);
    get("openxrRuntime", openxrRuntime);
    get("autoRegisterSteamVrDriver", autoRegisterSteamVrDriver);
    get("cloudUrl", cloudUrl);
    get("allowRemote", allowRemote);
    get("cloudRefreshTokenProtected", cloudRefreshTokenProtected);
    get("cloudUserName", cloudUserName);
    get("cloudEmail", cloudEmail);
    // A84 Q-persist (TASK-A84 §4): the ONE-TIME migration behind the flipped
    // autoTune default. A file with no schema stamp was last written by a build
    // whose default was off, and SaveLocked dumps EVERY key - so its
    // "autoTune": false proves nothing about what the user wanted. A stamped file
    // rules exactly as written, so a user who turns the tuner off keeps it off over
    // restarts.
    // A84b §2: the comparison is rc::MigrateAutoTuneOn (shared/include/rc/
    // settings_migration.h), asserted in core_tests; what stays here is only the
    // read, type-checked like every other field in FromJson because this also runs
    // on the app thread (the UI's read-modify-write merge), where a throw from a
    // hand-edited value would be fatal. The read's whole contract is the sentinel:
    // absent key or wrong type leaves rc::kSettingsSchemaUnstamped, so both migrate.
    int savedSchema = rc::kSettingsSchemaUnstamped;
    if (const auto it = j.find("settingsSchema"); it != j.end() && it->is_number())
        savedSchema = it->get<int>();
    if (rc::MigrateAutoTuneOn(savedSchema)) autoTune = true;
    if (j.contains("trustedDevices") && j["trustedDevices"].is_array()) {
        trustedDevices.clear();
        for (const auto& d : j["trustedDevices"]) {
            TrustedDevice td;
            td.id = d.value("id", "");
            td.name = d.value("name", "");
            td.model = d.value("model", "");
            td.firstSeen = d.value("firstSeen", int64_t(0));
            td.lastSeen = d.value("lastSeen", int64_t(0));
            td.publicKey = d.value("publicKey", "");
            if (!td.id.empty()) trustedDevices.push_back(td);
        }
    }
    // Clamp to sane ranges.
    vrBitrateMbps = std::clamp(vrBitrateMbps, 10, 500);
    desktopBitrateMbps = std::clamp(desktopBitrateMbps, 5, 200);
    fecPercent = std::clamp(fecPercent, 0, 25);
    streamProfile = std::clamp(streamProfile, 0, 2);
    // 0 = follow the profile; otherwise clamp to the supported range.
    encodeScalePct = encodeScalePct > 0 ? std::clamp(encodeScalePct, 50, 100) : 0;
    if (codec != "auto" && codec != "h264" && codec != "hevc" && codec != "av1") codec = "auto";
    if (openxrRuntime != "rincast" && openxrRuntime != "steamvr") openxrRuntime = "steamvr";
    if (audioDevices != "auto" && audioDevices != "rincast" && audioDevices != "steam" && audioDevices != "system")
        audioDevices = "auto";
}

SettingsStore& SettingsStore::Get() {
    static SettingsStore store;
    return store;
}

void SettingsStore::Load() {
    std::lock_guard lock(m_mutex);
    std::vector<uint8_t> bytes;
    if (ReadFileBytes(DataDir() / L"settings.json", bytes)) {
        try {
            m_settings.FromJson(json::parse(bytes.begin(), bytes.end()));
        } catch (const std::exception& e) {
            RC_LOGW("settings.json is corrupt (%s), using defaults", e.what());
        }
    }
    bool dirty = false;
    if (m_settings.serverId.empty()) {
        uint8_t id[12];
        rc::RandomBytes(id, sizeof(id));
        m_settings.serverId = rc::HexEncode(id, sizeof(id));
        dirty = true;
    }
    if (m_settings.pcName.empty()) {
        m_settings.pcName = ComputerName();
        dirty = true;
    }
    if (dirty) SaveLocked();
}

Settings SettingsStore::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void SettingsStore::Update(const std::function<void(Settings&)>& mutate) {
    std::lock_guard lock(m_mutex);
    mutate(m_settings);
    SaveLocked();
}

void SettingsStore::SaveLocked() {
    if (!WriteFileAtomic(DataDir() / L"settings.json", m_settings.ToJson(false).dump(2)))
        RC_LOGW("Failed to save settings");
}

}  // namespace rcs
