#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../headset/app/src/main/cpp/gfx/scene.cpp"
#include "rc/platform.h"

namespace {

void LogSink(rc::LogLevel level, const char* message) {
    const int priority = level == rc::LogLevel::Error ? ANDROID_LOG_ERROR
                        : level == rc::LogLevel::Warn ? ANDROID_LOG_WARN
                                                      : ANDROID_LOG_INFO;
    __android_log_write(priority, "ShaderProbe", message);
    std::printf("%s %s\n", level == rc::LogLevel::Error ? "ERROR" : "LOG", message);
}

bool Compile(const char* name, const std::string& fragment) {
    rv::Program program;
    const bool ok = program.Build(rv::kBlitVs, fragment.c_str(), name);
    std::printf("RESULT %s %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

std::string Replace(std::string source, const char* from, const char* to) {
    const size_t at = source.find(from);
    if (at == std::string::npos) {
        std::fprintf(stderr, "MISSING PATTERN: %s\n", from);
        std::exit(2);
    }
    source.replace(at, std::char_traits<char>::length(from), to);
    return source;
}

}  // namespace

int main() {
    rc::SetLogSink(LogSink);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        std::fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError());
        return 2;
    }
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE,
                                    EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0) {
        std::fprintf(stderr, "eglChooseConfig failed\n");
        return 2;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttribs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        std::fprintf(stderr, "EGL setup failed 0x%x\n", eglGetError());
        return 2;
    }
    std::printf("CONTEXT %s | %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER),
                glGetString(GL_SHADING_LANGUAGE_VERSION));

    const std::string ext = "#extension GL_OES_EGL_image_external_essl3 : require\n";
    const std::string toLayer =
        "vec3 ToLayer(vec3 g) { return mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)); }\n";
    const std::string common = rv::kPanelFsCommon;
    const std::string minimalMain = R"(
uniform samplerExternalOES uTex;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";
    const std::string helperParam = R"(
uniform samplerExternalOES uTex;
in vec2 vUv;
out vec4 oColor;
vec4 Read(samplerExternalOES tex, vec2 uv) { return texture(tex, uv); }
void main() { oColor = Read(uTex, vUv); }
)";
    const std::string casOnly = R"(
uniform vec4 uCrop;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = vec4(ApplyCasExt(vec2(vUv.x, 1.0-vUv.y), uCrop, uInvTexSize, uSharpen), 1.0); }
)";
    std::string base = ext + toLayer + rv::kColorGradeFs + rv::kCasExtFs + rv::kFoveateFs + rv::kBlitExtFs;
    std::string moved = ext + toLayer + rv::kColorGradeFs + rv::kCasExtFs + rv::kFoveateFs + rv::kExternalMovedFs;
    std::string noEarlyReturn = rv::kCasExtFs;
    noEarlyReturn = Replace(noEarlyReturn,
                            "if (strength <= 0.001 || stepUv.x <= 0.0 || stepUv.y <= 0.0) return e;",
                            "if (strength <= 0.001 || stepUv.x <= 0.0 || stepUv.y <= 0.0) { return vec3(e.r); }");
    std::string stagedMin = rv::kCasExtFs;
    stagedMin = Replace(stagedMin,
        "vec3 mn = min(min(min(d, e), min(f, b)), h);\n    vec3 mx = max(max(max(d, e), max(f, b)), h);",
        "vec3 de = min(d, e);\n    vec3 fb = min(f, b);\n    vec3 mn = min(min(de, fb), h);\n    vec3 mx = max(max(max(d, e), max(f, b)), h);");
    std::string directHelper = rv::kCasExtFs;
    directHelper = Replace(directHelper,
        "return ApplyCasExtAt(crop.xy + uv * crop.zw, lo, hi, invSize * crop.zw, strength);",
        "vec2 tc = crop.xy + uv * crop.zw;\n    vec2 stepUv = invSize * crop.zw;\n    vec3 e = texture(uTex, tc).rgb;\n    if (strength <= 0.001 || stepUv.x <= 0.0 || stepUv.y <= 0.0) return e;\n    vec3 b = texture(uTex, clamp(tc + vec2(0.0, -stepUv.y), lo, hi)).rgb;\n    vec3 d = texture(uTex, clamp(tc + vec2(-stepUv.x, 0.0), lo, hi)).rgb;\n    vec3 f = texture(uTex, clamp(tc + vec2(stepUv.x, 0.0), lo, hi)).rgb;\n    vec3 h = texture(uTex, clamp(tc + vec2(0.0, stepUv.y), lo, hi)).rgb;\n    vec3 mn = min(min(min(d, e), min(f, b)), h);\n    vec3 mx = max(max(max(d, e), max(f, b)), h);\n    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);\n    amp = sqrt(amp);\n    float peak = -1.0 / mix(8.0, 5.0, strength);\n    vec3 w = amp * peak;\n    return clamp((e + (b + d + f + h) * w) / (1.0 + 4.0 * w), 0.0, 1.0);");

    int failures = 0;
    failures += !Compile("minimal-external", ext + minimalMain);
    const bool legacyParameterFails = !Compile("sampler-parameter-must-fail", ext + helperParam);
    std::printf("RESULT sampler-parameter-mutation %s\n", legacyParameterFails ? "REJECTED" : "UNEXPECTED-PASS");
    failures += !legacyParameterFails;
    failures += !Compile("cas-only", ext + rv::kCasExtFs + casOnly);
    failures += !Compile("blit-baseline", base);
    failures += !Compile("moved-baseline", moved);
    failures += !Compile("blit-no-cas", Replace(base, "ApplyCasExt(uv, uCrop, uInvTexSize, uSharpen)",
                                                "texture(uTex, uCrop.xy + uv * uCrop.zw).rgb"));
    failures += !Compile("blit-no-colorgrade", Replace(base, "g = ColorGrade(g);", ""));
    failures += !Compile("blit-no-foveate", Replace(base, "vec2 uv = Foveate(vec2(vUv.x, 1.0 - vUv.y), step(0.5, vUv.x));",
                                                  "vec2 uv = vec2(vUv.x, 1.0 - vUv.y);"));
    failures += !Compile("blit-cas-early-return", ext + toLayer + rv::kColorGradeFs + noEarlyReturn + rv::kFoveateFs + rv::kBlitExtFs);
    failures += !Compile("blit-cas-staged-min", ext + toLayer + rv::kColorGradeFs + stagedMin + rv::kFoveateFs + rv::kBlitExtFs);
    failures += !Compile("blit-cas-direct", ext + toLayer + rv::kColorGradeFs + directHelper + rv::kFoveateFs + rv::kBlitExtFs);

    rv::SceneRenderer scene;
    const bool sceneOk = scene.Init();
    std::printf("RESULT scene-init %s\n", sceneOk ? "PASS" : "FAIL");
    failures += !sceneOk;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    std::printf("EXIT %d\n", failures ? 1 : 0);
    return failures ? 1 : 0;
}
