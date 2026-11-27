#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../../headset/app/src/main/cpp/gfx/scene.cpp"
#include "../../headset/app/src/main/cpp/media/video_decoder.h"
#include "rc/platform.h"

namespace {

constexpr int kSourceWidth = 2880;
constexpr int kSourceHeight = 1600;
constexpr int kOutputWidth = 512;
constexpr int kOutputHeight = 8;

struct Pixel {
    std::array<float, 4> value;
};

void LogSink(rc::LogLevel level, const char* message) {
    __android_log_write(level == rc::LogLevel::Error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        "ShaderPixels", message);
    std::fprintf(stderr, "%s %s\n", level == rc::LogLevel::Error ? "ERROR" : "LOG", message);
}

float SamplePlane(const std::vector<uint8_t>& source, size_t offset, int width, int height, float u, float v) {
    const float x = u * width - 0.5f;
    const float y = v * height - 0.5f;
    const int x0 = std::clamp(int(std::floor(x)), 0, width - 1);
    const int y0 = std::clamp(int(std::floor(y)), 0, height - 1);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float p00 = source[offset + size_t(y0) * width + x0];
    const float p10 = source[offset + size_t(y0) * width + x1];
    const float p01 = source[offset + size_t(y1) * width + x0];
    const float p11 = source[offset + size_t(y1) * width + x1];
    return (1.f - fx) * (1.f - fy) * p00 + fx * (1.f - fy) * p10 +
           (1.f - fx) * fy * p01 + fx * fy * p11;
}

Pixel Expected(const std::vector<uint8_t>& source, const float crop[4], int x, int y) {
    const float u = crop[0] + ((x + 0.5f) / kOutputWidth) * crop[2];
    const float v = crop[1] + (1.f - (y + 0.5f) / kOutputHeight) * crop[3];
    const float luma = SamplePlane(source, 0, kSourceWidth, kSourceHeight, u, v);
    const float cb = SamplePlane(source, size_t(kSourceWidth) * kSourceHeight,
                                 kSourceWidth / 2, kSourceHeight / 2, u, v);
    const float cr = SamplePlane(source, size_t(kSourceWidth) * kSourceHeight * 5 / 4,
                                 kSourceWidth / 2, kSourceHeight / 2, u, v);
    const float yy = (luma - 16.f) / 219.f;
    const float uu = (cb - 128.f) / 224.f;
    const float vv = (cr - 128.f) / 224.f;
    return {{std::clamp(yy + 1.5748f * vv, 0.f, 1.f),
             std::clamp(yy - 0.1873f * uu - 0.4681f * vv, 0.f, 1.f),
             std::clamp(yy + 1.8556f * uu, 0.f, 1.f), 1.f}};
}

std::array<uint8_t, 4> Quantize(const Pixel& p) {
    std::array<uint8_t, 4> out{};
    for (int c = 0; c < 4; ++c)
        out[c] = uint8_t(std::lround(std::clamp(p.value[c], 0.f, 1.f) * 255.f));
    return out;
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: shader-pixel-probe fixture.yuv fixture.hevc\n");
        return 2;
    }
    const std::vector<uint8_t> source = ReadFile(argv[1]);
    std::vector<uint8_t> bitstream = ReadFile(argv[2]);
    if (source.size() != size_t(kSourceWidth) * kSourceHeight * 3 / 2 || bitstream.empty()) {
        std::printf("FAIL fixture-size yuv=%zu hevc=%zu\n", source.size(), bitstream.size());
        return 1;
    }
    rc::SetLogSink(LogSink);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 2;
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE,
                                    EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0) return 2;
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttribs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) return 2;
    std::printf("CONTEXT %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    rv::SceneRenderer scene;
    if (!scene.Init()) {
        std::printf("FAIL scene-init\n");
        return 1;
    }
    rv::VideoDecoder decoder;
    if (!decoder.Init(rc::VideoCodec::HEVC, kSourceWidth, kSourceHeight, 1.f)) {
        std::printf("FAIL decoder-init\n");
        return 1;
    }
    decoder.Submit(1, true, false, std::move(bitstream), {}, rc::NowNs());
    rv::DecodedFrameInfo info;
    bool latched = false;
    for (int attempt = 0; attempt < 500 && !latched && !decoder.Failed(); ++attempt) {
        latched = decoder.Latch(info);
        if (!latched) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!latched) {
        std::printf("FAIL decoder-latch decoded=%u failed=%d\n", decoder.DecodedFrames(), int(decoder.Failed()));
        return 1;
    }
    const auto geometry = decoder.Geometry();
    std::printf("DECODER texture=%ux%u crop=%.9f,%.9f,%.9f,%.9f frames=%u\n",
                geometry.width, geometry.height, geometry.crop[0], geometry.crop[1],
                geometry.crop[2], geometry.crop[3], decoder.DecodedFrames());

    GLuint targetTexture = 0;
    glGenTextures(1, &targetTexture);
    glBindTexture(GL_TEXTURE_2D, targetTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kOutputWidth, kOutputHeight);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("FAIL destination-fbo\n");
        return 1;
    }

    const float crop[4] = {16.5f / kSourceWidth, 80.5f / kSourceHeight,
                           2827.f / kSourceWidth, 1439.f / kSourceHeight};
    scene.BlitExternal(fbo, geometry.texture, kOutputWidth, kOutputHeight, crop,
                       0.f, 0.f, 1.f, 1.f, nullptr, 0.f, 0.f, 0.f, geometry.width, geometry.height);
    glFinish();
    const GLenum error = glGetError();
    std::vector<uint8_t> actual(size_t(kOutputWidth) * kOutputHeight * 4);
    glReadPixels(0, 0, kOutputWidth, kOutputHeight, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    int mismatches = 0;
    int compared = 0;
    int isolationErrors = 0;
    float maxError = 0.f;
    int firstX = -1, firstY = -1;
    std::array<uint8_t, 4> firstExpected{}, firstActual{};
    for (int y = 0; y < kOutputHeight; ++y) {
        for (int x = 0; x < kOutputWidth; ++x) {
            const float vUvX = (x + 0.5f) / kOutputWidth;
            const float vUvY = (y + 0.5f) / kOutputHeight;
            const float sourceX = 16.5f + vUvX * 2827.f;
            const float sourceY = 80.5f + (1.f - vUvY) * 1439.f;
            const bool awayFromSeam = sourceX <= 1428.f || sourceX >= 1452.f;
            const bool safe = awayFromSeam && sourceX >= 32.f && sourceX <= 2828.f &&
                              sourceY >= 96.f && sourceY <= 1504.f;
            const auto expected = Quantize(Expected(source, crop, x, y));
            const uint8_t* got = actual.data() + (size_t(y) * kOutputWidth + x) * 4;
            if (safe) {
                ++compared;
                bool mismatch = false;
                for (int c = 0; c < 4; ++c) {
                    const float difference = std::abs(int(got[c]) - int(expected[c]));
                    maxError = std::max(maxError, difference);
                    mismatch |= difference > 8.f;
                }
                if (mismatch) {
                    if (firstX < 0) {
                        firstX = x; firstY = y;
                        for (int c = 0; c < 4; ++c) {
                            firstExpected[c] = expected[c]; firstActual[c] = got[c];
                        }
                    }
                    ++mismatches;
                }
                if ((sourceX < 1440.f && got[2] > got[0] * 0.6f + 10.f) ||
                    (sourceX >= 1440.f && got[0] > got[2] * 0.6f + 10.f))
                    ++isolationErrors;
            }
        }
    }
    for (const auto [x, y] : std::array<std::array<int, 2>, 6>{{
             {{32, 0}}, {{224, 4}}, {{257, 0}}, {{258, 0}}, {{288, 4}}, {{480, 7}}}}) {
        const auto expected = Quantize(Expected(source, crop, x, y));
        const uint8_t* got = actual.data() + (size_t(y) * kOutputWidth + x) * 4;
        std::printf("PIXEL x=%d y=%d expected=%u,%u,%u,%u actual=%u,%u,%u,%u\n",
                    x, y, expected[0], expected[1], expected[2], expected[3],
                    got[0], got[1], got[2], got[3]);
    }
    std::printf("CONTRACT buffer=%ux%u crop=(16.5,80.5)-(2843.5,1519.5) output=%ux%u compared=%d\n",
                kSourceWidth, kSourceHeight, kOutputWidth, kOutputHeight, compared);
    std::printf("RESULT pixels mismatches=%d isolation=%d max_error=%.0f gl_error=0x%x\n",
                mismatches, isolationErrors, maxError, error);
    if (mismatches)
        std::printf("FIRST x=%d y=%d expected=%u,%u,%u,%u actual=%u,%u,%u,%u\n",
                    firstX, firstY, firstExpected[0], firstExpected[1], firstExpected[2], firstExpected[3],
                    firstActual[0], firstActual[1], firstActual[2], firstActual[3]);

    FILE* out = std::fopen("/data/local/tmp/rincast-shader-pixels.rgba", "wb");
    if (out) {
        std::fwrite(actual.data(), 1, actual.size(), out);
        std::fclose(out);
    }
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &targetTexture);
    decoder.Shutdown();
    const bool ok = error == GL_NO_ERROR && mismatches == 0 && isolationErrors == 0;
    std::printf("EXIT %d\n", ok ? 0 : 1);
    return ok ? 0 : 1;
}
