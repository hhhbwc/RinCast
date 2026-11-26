// Streamer side of the VR bridge: owns the shared memory, the shared eye
// textures and the events that the SteamVR driver / OpenXR runtime use.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../gpu.h"
#include "vr_bridge.h"
#include "rc/tracking_cache.h"

namespace rcs {

class VrBridgeHost {
   public:
    using HapticHandler = std::function<void(const rcvr::HapticEvent&)>;

    bool Start(const rcvr::BridgeNames& names = {});
    void Stop();

    // Applies a headset display configuration. Recreates textures when the
    // resolution or pixel format changes. Returns true if SteamVR needs a restart to apply.
    bool Configure(const rcvr::DisplayConfig& config, bool tenBit = false);
    void SetHeadsetConnected(bool connected);
    void PublishTracking(const rcvr::TrackingSnapshot& snapshot, bool clockValid);
    bool LookupTracking(uint64_t sequence, int64_t& targetDisplayPcNs, bool& clockValid) const;
    void PublishBody(const rcvr::BodySnapshot& snapshot);
    void PublishHand(int hand, const rcvr::HandSnapshot& snapshot);
    void Recenter();
    // Moves the driver's vsync by about half of shiftNs (+ = later), and learns the rate at
    // which the two clocks drift apart, from the headset's reports twice a second.
    void ShiftVsync(int64_t shiftNs);
    // Moves the driver's vsync later by slipNs, a little with every vsync (rc::kVrPhaseSlipRate):
    // the headset has frames waiting and shows them without getting new ones as fast, until
    // none wait, instead of skipping one.
    void SlipVsync(int64_t slipNs);
    // While SteamVR runs on our driver: how long a pose published at `atNs` waits until the
    // driver picks one up for its next frame (1 ms before its vsync), 0 to one period.
    bool PoseWaitNs(int64_t atNs, int64_t& waitNs, int64_t& periodNs) const;

    void SetHapticHandler(HapticHandler h);

    rcvr::Shared* Shared() const { return m_shared; }
    HANDLE FrameEvent() const { return m_frameEvent; }
    ID3D11Device* Device() const { return m_device.Get(); }
    UINT VendorId() const { return m_gpu.vendorId; }
    // Shared texture for a slot (same device as Device()).
    ComPtr<ID3D11Texture2D> SlotTexture(int slot) const {
        std::lock_guard lock(m_mutex);
        return m_textures[slot];
    }
    uint32_t TextureGeneration() const { return m_generation.load(std::memory_order_relaxed); }

    rcvr::ProducerKind ActiveProducer() const;
    std::string ApplicationName() const;
    bool HasConfig() const { return m_hasConfig; }
    // The size SteamVR renders each eye at, from its frames; false while it sends none.
    bool SteamVrEyeSize(uint32_t& width, uint32_t& height) const;
    rcvr::DisplayConfig LastConfig() const;

   private:
    void HeartbeatLoop();
    bool CreateTextures(uint32_t width, uint32_t height, bool tenBit);
    void RecoverFromDeadProducer();
    void ClearTrackingCache();

    HANDLE m_mapping = nullptr;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_trackingEvent = nullptr;
    rcvr::Shared* m_shared = nullptr;

    ComPtr<ID3D11Device> m_device;
    GpuInfo m_gpu;
    ComPtr<ID3D11Texture2D> m_textures[rcvr::kFrameSlots];
    HANDLE m_textureHandles[rcvr::kFrameSlots] = {};
    std::atomic<uint32_t> m_generation{0};
    bool m_hasConfig = false;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    HapticHandler m_onHaptic;
    LONG m_hapticsRead = 0;
    uint32_t m_hapticsWaitTicks = 0;
    int64_t m_hapticsWaitStartNs = 0;
    int64_t m_epochBaseNs = 0;
    std::atomic<double> m_pendingShiftNs{0};  // for the heartbeat loop, which owns the phase
    std::atomic<double> m_driftNsPerS{0};
    std::atomic<double> m_pendingSlipNs{0};  // what is left of a slip

    mutable std::mutex m_trackingCacheMutex;
    rc::TrackingTargetCache m_trackingCache;
};

}  // namespace rcs
