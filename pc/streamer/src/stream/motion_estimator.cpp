#include "motion_estimator.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "rc/platform.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace rcs {

namespace {

const char* kLumaCs = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<unorm float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float sum = 0;
    for (uint y = 0; y < 4; y++)
        for (uint x = 0; x < 4; x++)
            sum += dot(src.Load(int3(id.xy * 4 + uint2(x, y), 0)).rgb, float3(0.2126, 0.7152, 0.0722));
    dst[id.xy] = sum / 16;
}
)";

const char* kDownCs = R"(
Texture2D<float> src : register(t0);
RWTexture2D<unorm float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float sum = 0;
    for (uint y = 0; y < 4; y++)
        for (uint x = 0; x < 4; x++) sum += src.Load(int3(id.xy * 4 + uint2(x, y), 0));
    dst[id.xy] = sum / 16;
}
)";

// Where a block's contents would have been in the last frame if only the head had turned.
const char* kExpected = R"(
cbuffer Constants : register(b0) {
    float4 turn0, turn1, turn2;
    float4 fov[2];
    float2 size;
};
float2 ExpectedPixels(uint2 block) {
    const float2 c = (float2(block) + 0.5) * 32.0;
    const float eyeW = size.x * 0.5;
    const int eye = c.x >= eyeW ? 1 : 0;
    const float4 f = fov[eye];
    const float u = (c.x - eye * eyeW) / eyeW, v = c.y / size.y;
    const float3 ray = float3(-f.x + u * (f.x + f.y), f.z - v * (f.z + f.w), -1.0);
    const float3 r = float3(dot(turn0.xyz, ray), dot(turn1.xyz, ray), dot(turn2.xyz, ray));
    if (r.z > -0.05) return float2(0, 0);
    const float tx = r.x / -r.z, ty = r.y / -r.z;
    const float un = (tx + f.x) / (f.x + f.y), vn = (f.z - ty) / (f.z + f.w);
    return float2((un - u) * eyeW, (vn - v) * size.y);
}
// The other eye's picture is no match. The current block chooses the eye; a shifted window
// never does. Coordinates are pixels of an image size / scale.
int2 ClampOffset(int2 blockOrigin, int2 corner, int2 offset, int blockW, float scale) {
    const int eyeW = int(size.x * 0.5 / scale);
    const int left = blockOrigin.x < eyeW ? 0 : eyeW;
    offset.x = clamp(offset.x, left - corner.x, left + eyeW - blockW - corner.x);
    offset.y = clamp(offset.y, -corner.y, int(size.y / scale) - blockW - corner.y);
    return offset;
}
bool SameEye(int2 blockOrigin, int2 corner, int2 offset, int blockW, float scale) {
    const int eyeW = int(size.x * 0.5 / scale);
    const int left = blockOrigin.x < eyeW ? 0 : eyeW;
    return corner.x + offset.x >= left && corner.x + offset.x + blockW <= left + eyeW &&
           corner.y + offset.y >= 0 && corner.y + offset.y + blockW <= int(size.y / scale);
}
)";

// Full search on the 1/16 images, around where the head turn alone would put it. A block is
// 2 x 2 pixels here, matched over the 4 x 4 around it. Result: where the block's contents were
// in the last frame, relative to now, in 1/16 pixels.
const char* kCoarseCs = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
RWTexture2D<float2> dst : register(u0);
float CoarseSad(int2 blockOrigin, int2 corner, int2 offset, int2 base) {
    const int eyeW = int(size.x * 0.5 / 16.0);
    const int left = blockOrigin.x < eyeW ? 0 : eyeW;
    const int right = left + eyeW;
    float sad = 0;
    int count = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int2 p = corner + int2(x, y);
            const int2 q = p + offset;
            if (p.x < left || p.x >= right || q.x < left || q.x >= right ||
                p.y < 0 || p.y >= int(size.y / 16.0) || q.y < 0 || q.y >= int(size.y / 16.0)) continue;
            sad += abs(cur.Load(int3(p, 0)) - prev.Load(int3(q, 0)));
            count++;
        }
    }
    // Compare mean error so a smaller boundary window cannot win by collecting less error.
    return count >= 4 ? sad * (16.0 / float(count)) + 0.5 * (abs(offset.x - base.x) + abs(offset.y - base.y)) : 1e9;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(size.x / 32.0) || id.y >= uint(size.y / 32.0)) return;
    float best = 1e9;
    const uint eyeBlocks = uint(size.x / 64.0);
    const int left = id.x < eyeBlocks ? 0 : int(eyeBlocks) * 2;
    const int2 corner = int2(clamp(int(id.x) * 2 - 1, left, left + int(eyeBlocks) * 2 - 4),
                             clamp(int(id.y) * 2 - 1, 0, int(size.y / 16.0) - 4));
    const int2 base = ClampOffset(int2(id.xy) * 2, corner, int2(round(ExpectedPixels(id.xy) / 16.0)), 4, 16.0);
    int2 bestOffset = base;
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -8; dx <= 8; dx++) {
            const int2 o = base + int2(dx, dy);
            float sad = CoarseSad(int2(id.xy) * 2, corner, o, base);
            if (sad < best) {
                best = sad;
                bestOffset = o;
            }
        }
    }
    dst[id.xy] = float2(bestOffset);
}
)";

// On the 1/4 images: tries this block's coarse vector, its neighbours' and none, then searches
// around the best. A block is 8 x 8 pixels here. Result in full pixels of motion: prev -> now.
const char* kRefineCs = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
Texture2D<float2> coarse : register(t2);
RWTexture2D<float2> dst : register(u0);
float Sad(int2 corner, int2 o) {
    float sad = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sad += abs(cur.Load(int3(corner + int2(x, y), 0)) - prev.Load(int3(corner + int2(x + o.x, y + o.y), 0)));
    return sad;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(size.x / 32.0) || id.y >= uint(size.y / 32.0)) return;
    const int2 corner = int2(id.xy) * 8;
    // Flat-area variance gate: avoid matching noise on solid walls, fog or dark skies.
    float mean = 0.0;
    for (int my = 0; my < 8; my++)
        for (int mx = 0; mx < 8; mx++)
            mean += cur.Load(int3(corner + int2(mx, my), 0));
    mean *= (1.0 / 64.0);
    float mad = 0.0;
    for (int vy = 0; vy < 8; vy++)
        for (int vx = 0; vx < 8; vx++)
            mad += abs(cur.Load(int3(corner + int2(vx, vy), 0)) - mean);
    if (mad < 0.012 * 64.0) {
        dst[id.xy] = float2(0.0, 0.0);
        return;
    }
    const int2 base = ClampOffset(int2(id.xy) * 8, corner, int2(round(ExpectedPixels(id.xy) / 4.0)), 8, 4.0);
    const float zero = Sad(corner, base);
    float best = zero;
    int2 bestOffset = base;
    const uint eyeBlocks = uint(size.x / 64.0);
    static const int2 near[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    for (int i = 0; i < 5; i++) {
        const int2 n = int2(id.xy) + near[i];
        if (n.x < 0 || n.y < 0 || n.x >= int(size.x / 32.0) || n.y >= int(size.y / 32.0) ||
            (n.x < int(eyeBlocks)) != (int(id.x) < int(eyeBlocks))) continue;
        const int2 c = int2(coarse.Load(int3(n, 0))) * 4;
        if (!SameEye(int2(id.xy) * 8, corner, c, 8, 4.0)) continue;
        const float sad = Sad(corner, c);
        if (sad < best) {
            best = sad;
            bestOffset = c;
        }
    }
    const int2 centre = bestOffset;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (!SameEye(int2(id.xy) * 8, corner, centre + int2(dx, dy), 8, 4.0)) continue;
            const float sad = Sad(corner, centre + int2(dx, dy));
            if (sad < best) {
                best = sad;
                bestOffset = centre + int2(dx, dy);
            }
        }
    }
    // Moving only counts if it clearly matches better than staying: this ignores noise,
    // compression and flat areas, where a wrong vector would bend the picture for nothing.
    if (best > zero * 0.8 - 0.02) bestOffset = base;
    // More than 32 px of content motion in one frame (2880 px/s) is either a wrong match or
    // more than a held frame can be bent into: keep what the head turn alone explains.
    if (abs(bestOffset.x - base.x) > 8 || abs(bestOffset.y - base.y) > 8) bestOffset = base;
    float2 sub = float2(0.0, 0.0);
    if (bestOffset.x != base.x || bestOffset.y != base.y) {
        if (SameEye(int2(id.xy) * 8, corner, bestOffset + int2(-1, 0), 8, 4.0) &&
            SameEye(int2(id.xy) * 8, corner, bestOffset + int2(1, 0), 8, 4.0)) {
            const float sL = Sad(corner, bestOffset + int2(-1, 0));
            const float sR = Sad(corner, bestOffset + int2(1, 0));
            const float denom = sL - 2.0 * best + sR;
            if (denom > 1e-4) sub.x = clamp(0.5 * (sL - sR) / denom, -0.5, 0.5);
        }
        if (SameEye(int2(id.xy) * 8, corner, bestOffset + int2(0, -1), 8, 4.0) &&
            SameEye(int2(id.xy) * 8, corner, bestOffset + int2(0, 1), 8, 4.0)) {
            const float sU = Sad(corner, bestOffset + int2(0, -1));
            const float sD = Sad(corner, bestOffset + int2(0, 1));
            const float denom = sU - 2.0 * best + sD;
            if (denom > 1e-4) sub.y = clamp(0.5 * (sU - sD) / denom, -0.5, 0.5);
        }
    }
    dst[id.xy] = (float2(base) - (float2(bestOffset) + sub)) * 4.0;
}
)";

// Component-wise median of each 3 x 3: single wrong blocks go away, edges stay.
const char* kMedianCs = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
float Median(float v[9]) {
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 9; j++) {
            if (v[j] < v[i]) {
                const float t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    return v[4];
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint dimX, dimY;
    src.GetDimensions(dimX, dimY);
    if (id.x >= dimX || id.y >= dimY) return;
    float x[9], y[9];
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            int2 n = clamp(int2(id.xy) + int2(i - 1, j - 1), int2(0, 0), int2(int(dimX), int(dimY)) - 1);
            if ((n.x < int(dimX / 2)) != (id.x < dimX / 2)) n.x = id.x;
            const float2 m = src.Load(int3(n, 0));
            x[j * 3 + i] = m.x;
            y[j * 3 + i] = m.y;
        }
    }
    dst[id.xy] = float2(Median(x), Median(y));
}
)";

// 3x3 edge-preserving bilateral filter on the median-cleaned motion field.
const char* kBilateralCs = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint dimX, dimY;
    src.GetDimensions(dimX, dimY);
    if (id.x >= dimX || id.y >= dimY) return;
    const float2 c = src.Load(int3(id.xy, 0));
    if (dot(c, c) < 1e-6) {
        dst[id.xy] = float2(0.0, 0.0);
        return;
    }
    static const float kSpatial[3][3] = {
        {1.0, 2.0, 1.0},
        {2.0, 4.0, 2.0},
        {1.0, 2.0, 1.0},
    };
    float2 sum = float2(0.0, 0.0);
    float wSum = 0.0;
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            int2 n = clamp(int2(id.xy) + int2(i - 1, j - 1), int2(0, 0), int2(int(dimX), int(dimY)) - 1);
            if ((n.x < int(dimX / 2)) != (id.x < dimX / 2)) continue;
            const float2 m = src.Load(int3(n, 0));
            const float diff = length(m - c);
            const float rangeW = exp(-(diff * diff) / (2.0 * 6.0 * 6.0));
            const float w = kSpatial[j][i] * rangeW;
            sum += m * w;
            wSum += w;
        }
    }
    dst[id.xy] = wSum > 1e-5 ? (sum / wSum) : c;
}
)";

ComPtr<ID3D11ComputeShader> Compile(ID3D11Device* device, const char* name, const char* source) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                          0, &code, &errors))) {
        RC_LOGE("Motion estimator: %s shader compile failed: %s", name,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler diagnostic");
        return nullptr;
    }
    ComPtr<ID3D11ComputeShader> cs;
    const HRESULT result = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    if (FAILED(result)) {
        RC_LOGE("Motion estimator: %s shader object creation failed: %08lx", name, static_cast<unsigned long>(result));
        return nullptr;
    }
    return cs;
}

UINT Groups(uint32_t size) { return (size + 7) / 8; }

}  // namespace

bool MotionEstimator::MakeLevel(DXGI_FORMAT format, uint32_t w, uint32_t h, Level& level) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(m_device->CreateTexture2D(&d, nullptr, &level.tex)) &&
           SUCCEEDED(m_device->CreateShaderResourceView(level.tex.Get(), nullptr, &level.srv)) &&
           SUCCEEDED(m_device->CreateUnorderedAccessView(level.tex.Get(), nullptr, &level.uav));
}

bool MotionEstimator::Init(ID3D11Device* device, uint32_t width, uint32_t height) {
    m_device = device;
    m_width = width;
    m_height = height;
    m_quarterW = width / 4;
    m_quarterH = height / 4;
    m_sixteenthW = m_quarterW / 4;
    m_sixteenthH = m_quarterH / 4;
    m_gridWidth = width / kBlockSize;
    m_gridHeight = height / kBlockSize;
    Reset();
    m_lumaCs = Compile(device, "motion-luma", kLumaCs);
    m_downCs = Compile(device, "motion-down", kDownCs);
    m_coarseCs = Compile(device, "motion-coarse", (std::string(kExpected) + kCoarseCs).c_str());
    m_refineCs = Compile(device, "motion-refine", (std::string(kExpected) + kRefineCs).c_str());
    m_medianCs = Compile(device, "motion-median", kMedianCs);
    m_bilateralCs = Compile(device, "motion-bilateral", kBilateralCs);
    if (!m_lumaCs || !m_downCs || !m_coarseCs || !m_refineCs || !m_medianCs || !m_bilateralCs) {
        RC_LOGE("Motion estimator init failed at %ux%u: one or more compute shaders are unavailable", width, height);
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_constants))) return false;
    for (int i = 0; i < 2; i++) {
        if (!MakeLevel(DXGI_FORMAT_R8_UNORM, m_quarterW, m_quarterH, m_quarter[i]) ||
            !MakeLevel(DXGI_FORMAT_R8_UNORM, m_sixteenthW, m_sixteenthH, m_sixteenth[i]))
            return false;
    }
    if (!MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_coarse) ||
        !MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_refined) ||
        !MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_median) ||
        !MakeLevel(DXGI_FORMAT_R16G16_FLOAT, m_gridWidth, m_gridHeight, m_field))
        return false;
    D3D11_TEXTURE2D_DESC d{};
    m_field.tex->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& staging : m_staging)
        if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return false;
    return true;
}

void MotionEstimator::Dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT srvCount,
                               ID3D11UnorderedAccessView* uav, UINT x, UINT y) {
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, srvCount, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch(x, y, 1);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 3, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
}

bool MotionEstimator::Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, const float headTurn[9], const float fovTan[2][4]) {
    D3D11_MAPPED_SUBRESOURCE constants;
    if (FAILED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &constants))) return false;
    auto* c = static_cast<float*>(constants.pData);
    for (int row = 0; row < 3; row++) {
        std::memcpy(c + row * 4, headTurn + row * 3, sizeof(float) * 3);
        c[row * 4 + 3] = 0;
    }
    std::memcpy(c + 12, fovTan, sizeof(float) * 8);
    c[20] = float(m_width);
    c[21] = float(m_height);
    ctx->Unmap(m_constants.Get(), 0);
    ID3D11Buffer* buffer = m_constants.Get();
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    m_current ^= 1;
    Level& quarter = m_quarter[m_current];
    Level& sixteenth = m_sixteenth[m_current];
    Dispatch(ctx, m_lumaCs.Get(), &source, 1, quarter.uav.Get(), Groups(m_quarterW), Groups(m_quarterH));
    ID3D11ShaderResourceView* quarterSrv = quarter.srv.Get();
    Dispatch(ctx, m_downCs.Get(), &quarterSrv, 1, sixteenth.uav.Get(), Groups(m_sixteenthW), Groups(m_sixteenthH));
    const bool compared = m_hasPrevious;
    if (compared) {
        ID3D11ShaderResourceView* coarseIn[2] = {sixteenth.srv.Get(), m_sixteenth[m_current ^ 1].srv.Get()};
        Dispatch(ctx, m_coarseCs.Get(), coarseIn, 2, m_coarse.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
        ID3D11ShaderResourceView* refineIn[3] = {quarter.srv.Get(), m_quarter[m_current ^ 1].srv.Get(), m_coarse.srv.Get()};
        Dispatch(ctx, m_refineCs.Get(), refineIn, 3, m_refined.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
        ID3D11ShaderResourceView* medianIn = m_refined.srv.Get();
        Dispatch(ctx, m_medianCs.Get(), &medianIn, 1, m_median.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
        ID3D11ShaderResourceView* bilateralIn = m_median.srv.Get();
        Dispatch(ctx, m_bilateralCs.Get(), &bilateralIn, 1, m_field.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
    }
    m_hasPrevious = true;
    return compared;
}

void MotionEstimator::Decode(const D3D11_MAPPED_SUBRESOURCE& map, std::vector<float>& out) {
    out.resize(size_t(m_gridWidth) * m_gridHeight * 2);
    for (uint32_t y = 0; y < m_gridHeight; y++) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch);
        for (uint32_t x = 0; x < m_gridWidth * 2; x++) {
            out[size_t(y) * m_gridWidth * 2 + x] = DecodeMotionHalf(row[x]);
        }
    }
}

void MotionEstimator::ApplyTemporalSmoothing(std::vector<float>& field) {
    if (m_prevField.size() == field.size()) {
        for (size_t i = 0; i + 1 < field.size(); i += 2) {
            const float cx = field[i], cy = field[i + 1];
            const float px = m_prevField[i], py = m_prevField[i + 1];
            const float dot = cx * px + cy * py;
            const float dx = cx - px, dy = cy - py;
            if (dot > 0.f && (dx * dx + dy * dy) < (12.0f * 12.0f)) {
                field[i] = 0.75f * cx + 0.25f * px;
                field[i + 1] = 0.75f * cy + 0.25f * py;
            }
        }
    }
    m_prevField = field;
}

bool MotionEstimator::Read(ID3D11DeviceContext* ctx, std::vector<float>& out) {
    ctx->CopyResource(m_staging[0].Get(), m_field.tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(m_staging[0].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    Decode(map, out);
    ctx->Unmap(m_staging[0].Get(), 0);
    ApplyTemporalSmoothing(out);
    return true;
}

#ifdef RINCAST_TEST_EXPOSE_MOTION_STAGES
bool MotionEstimator::ReadCoarse(ID3D11DeviceContext* ctx, std::vector<float>& out) {
    ctx->CopyResource(m_staging[0].Get(), m_coarse.tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(m_staging[0].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    Decode(map, out);
    ctx->Unmap(m_staging[0].Get(), 0);
    return true;
}

bool MotionEstimator::ReadRefined(ID3D11DeviceContext* ctx, std::vector<float>& out) {
    ctx->CopyResource(m_staging[0].Get(), m_refined.tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(m_staging[0].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    Decode(map, out);
    ctx->Unmap(m_staging[0].Get(), 0);
    return true;
}

bool MotionEstimator::RefineForTest(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* current,
                                    ID3D11ShaderResourceView* previous, ID3D11ShaderResourceView* coarse,
                                    const float headTurn[9], const float fovTan[2][4]) {
    D3D11_MAPPED_SUBRESOURCE constants;
    if (FAILED(ctx->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &constants))) return false;
    auto* c = static_cast<float*>(constants.pData);
    for (int row = 0; row < 3; row++) {
        std::memcpy(c + row * 4, headTurn + row * 3, sizeof(float) * 3);
        c[row * 4 + 3] = 0;
    }
    std::memcpy(c + 12, fovTan, sizeof(float) * 8);
    c[20] = float(m_width);
    c[21] = float(m_height);
    ctx->Unmap(m_constants.Get(), 0);
    ID3D11Buffer* buffer = m_constants.Get();
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView* inputs[3] = {current, previous, coarse};
    Dispatch(ctx, m_refineCs.Get(), inputs, 3, m_refined.uav.Get(), Groups(m_gridWidth), Groups(m_gridHeight));
    return true;
}
#endif

bool MotionEstimator::QueueRead(ID3D11DeviceContext* ctx, const rc::MotionReadbackTiming& timing) {
    int slot = -1;
    for (int i = 0; i < kReadbackSlots; i++) {
        const int candidate = (m_readWrite + i) % kReadbackSlots;
        if (!m_readPending[candidate]) {
            slot = candidate;
            break;
        }
    }
    // An old field is not worth stalling the capture path: retain the newest available fields.
    if (slot < 0) {
        slot = m_readNext;
        m_readNext = (slot + 1) % kReadbackSlots;
        m_readPending[slot] = false;
        m_readTiming[slot] = {};
        m_readQuery[slot].Reset();
        ++m_overwrittenReadbacks;
    }
    if (!m_readQuery[slot]) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(m_device->CreateQuery(&qd, &m_readQuery[slot]))) return false;
    }
    ctx->CopyResource(m_staging[slot].Get(), m_field.tex.Get());
    ctx->End(m_readQuery[slot].Get());
    m_readTiming[slot] = timing;
    m_readPending[slot] = true;
    m_readWrite = (slot + 1) % kReadbackSlots;
    return true;
}

bool MotionEstimator::TryRead(ID3D11DeviceContext* ctx, std::vector<float>& out, rc::MotionReadbackTiming& timing, bool& slotConsumed) {
    slotConsumed = false;
    timing = {};
    int slot = -1;
    for (int i = 0; i < kReadbackSlots; i++) {
        const int candidate = (m_readNext + i) % kReadbackSlots;
        if (m_readPending[candidate]) {
            slot = candidate;
            break;
        }
    }
    if (slot < 0) return false;
    BOOL done = FALSE;
    const HRESULT query = ctx->GetData(m_readQuery[slot].Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (FAILED(query)) {
        m_readPending[slot] = false;
        m_readTiming[slot] = {};
        m_readNext = (slot + 1) % kReadbackSlots;
        slotConsumed = true;
        return false;
    }
    if (!done) return false;
    D3D11_MAPPED_SUBRESOURCE map;
    const HRESULT hr = ctx->Map(m_staging[slot].Get(), 0, D3D11_MAP_READ, 0, &map);
    timing = m_readTiming[slot];
    m_readPending[slot] = false;
    m_readTiming[slot] = {};
    m_readNext = (slot + 1) % kReadbackSlots;
    slotConsumed = true;
    if (FAILED(hr)) return false;
    Decode(map, out);
    ctx->Unmap(m_staging[slot].Get(), 0);
    ApplyTemporalSmoothing(out);
    return true;
}

}  // namespace rcs
