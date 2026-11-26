// Finds how a frame's contents moved since the frame before, on the GPU: one motion vector
// per 32 x 32 pixel block, by block matching. The headset uses them to generate a frame when
// the next real one is late.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rc/motion_field.h"

namespace rcs {

inline float DecodeMotionHalf(uint16_t bits) {
    const int exponent = (bits >> 10) & 31;
    if (exponent == 31) {
        uint32_t out = (uint32_t(bits & 0x8000) << 16) | 0x7f800000u | (uint32_t(bits & 1023) << 13);
        if (bits & 1023) out |= 0x00400000u;
        return std::bit_cast<float>(out);
    }
    const float magnitude = exponent == 0 ? (bits & 1023) / 16777216.f : std::ldexp(float(1024 + (bits & 1023)), exponent - 25);
    return (bits & 0x8000) ? -magnitude : magnitude;
}

class MotionEstimator {
   public:
    static constexpr uint32_t kBlockSize = 32;
    static constexpr int kReadbackSlots = 4;

    bool Init(ID3D11Device* device, uint32_t width, uint32_t height);
    // The next frame has nothing to be compared with.
    void Reset() {
        m_hasPrevious = false;
        for (int i = 0; i < kReadbackSlots; i++) {
            m_readPending[i] = false;
            m_readTiming[i] = {};
            m_readQuery[i].Reset();
        }
        m_readWrite = m_readNext = 0;
        m_overwrittenReadbacks = 0;
    }
    // Drops only the previous-frame reference on an unexpected bridge skip while keeping
    // in-flight async GPU readbacks intact for frames already handed to the encoder.
    void ResetPrevious() { m_hasPrevious = false; }

    uint32_t GridWidth() const { return m_gridWidth; }
    uint32_t GridHeight() const { return m_gridHeight; }

    // Matches `source` (width x height, gamma-encoded colour) against the previous call's.
    // headTurn rotates a direction seen from this frame's head into the previous frame's
    // (row by row); fovTan is per eye: left, right, up, down. The result leaves out what that
    // turn explains. False if there's no previous frame to compare with yet.
    bool Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, const float headTurn[9], const float fovTan[2][4]);

    // The newest field: x, y in pixels per frame, row by row. Waits for the GPU.
    bool Read(ID3D11DeviceContext* ctx, std::vector<float>& out);

    // Queues a field in FIFO order without waiting. If all slots are pending, the oldest is dropped.
    bool QueueRead(ID3D11DeviceContext* ctx, const rc::MotionReadbackTiming& timing);
    bool TryRead(ID3D11DeviceContext* ctx, std::vector<float>& out, rc::MotionReadbackTiming& timing, bool& slotConsumed);
    bool HasPendingRead() const {
        for (bool pending : m_readPending)
            if (pending) return true;
        return false;
    }
    uint32_t OverwrittenReadbacks() const { return m_overwrittenReadbacks; }

#ifdef RINCAST_TEST_EXPOSE_MOTION_STAGES
    // Test-only stage readbacks keep boundary assertions on real shader output.
    bool ReadCoarse(ID3D11DeviceContext* ctx, std::vector<float>& out);
    bool ReadRefined(ID3D11DeviceContext* ctx, std::vector<float>& out);
    bool RefineForTest(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* previous,
                       ID3D11ShaderResourceView* coarse, const float headTurn[9], const float fovTan[2][4]);
#endif

   private:
    struct Level {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };
    bool MakeLevel(DXGI_FORMAT format, uint32_t w, uint32_t h, Level& level);
    void Dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT srvCount,
                  ID3D11UnorderedAccessView* uav, UINT x, UINT y);
    void Decode(const D3D11_MAPPED_SUBRESOURCE& map, std::vector<float>& out);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_lumaCs, m_downCs, m_coarseCs, m_refineCs, m_medianCs;
    Level m_quarter[2], m_sixteenth[2];  // this frame's and the last, swapping
    Level m_coarse, m_refined, m_field;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging[kReadbackSlots];
    Microsoft::WRL::ComPtr<ID3D11Query> m_readQuery[kReadbackSlots];
    bool m_readPending[kReadbackSlots]{};
    rc::MotionReadbackTiming m_readTiming[kReadbackSlots]{};
    int m_readWrite = 0;
    int m_readNext = 0;
    uint32_t m_overwrittenReadbacks = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_gridWidth = 0, m_gridHeight = 0, m_quarterW = 0, m_quarterH = 0, m_sixteenthW = 0, m_sixteenthH = 0;
    int m_current = 0;
    bool m_hasPrevious = false;
};

}  // namespace rcs
