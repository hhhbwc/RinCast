from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import re
import struct
import sys
import traceback
import zlib
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "headset/app/src/main/cpp/gfx/scene.cpp"

FIXED_GSR_HELPER = """highp vec4 GatherEye(highp vec2 uv, float eye) {
    highp float width = uViewport.z;
    highp float lo = eye * 0.5 * width;
    highp float hi = lo + 0.5 * width;
    highp float footprint = uv.x * width - 0.5;
    highp float base = floor(footprint);
    base = clamp(base, lo, max(lo, hi - 2.0));
    vec4 g = textureGather(uTex, vec2((base + 1.0) / width, uv.y), 1);
    if (footprint < lo) return vec4(g.x, g.x, g.w, g.w);
    if (footprint >= hi - 1.0) return vec4(g.y, g.y, g.z, g.z);
    return g;
}"""
# Mutation: the A6 bug — left boundary picked the right gather column (g.z) and the
# gather coordinate sat exactly on a floor() discontinuity ((base+0.5)/width).
GSR_OLD_COLUMN_HELPER = """highp vec4 GatherEye(highp vec2 uv, float eye) {
    highp float width = uViewport.z;
    highp float lo = eye * 0.5 * width;
    highp float hi = lo + 0.5 * width;
    highp float footprint = uv.x * width - 0.5;
    highp float base = floor(footprint);
    base = clamp(base, lo, max(lo, hi - 2.0));
    vec4 g = textureGather(uTex, vec2((base + 0.5) / width, uv.y), 1);
    if (footprint < lo) return vec4(g.x, g.x, g.z, g.z);
    if (footprint >= hi - 1.0) return vec4(g.y, g.y, g.z, g.z);
    return g;
}"""
OLD_GSR_HELPER = """highp vec4 GatherEye(highp vec2 p, float eye) {
    highp float width = uViewport.z;
    highp float lo = eye * 0.5 * width;
    highp float hi = lo + 0.5 * width;
    highp float x = clamp(p.x, lo, hi - 1.0);
    vec4 g = textureGather(uTex, vec2(x, p.y) * uViewport.xy, 1);
    if (p.x < lo || p.x >= hi) return vec4(g.x);
    if (p.x == lo) g.zw = vec2(g.x);
    if (p.x == hi - 1.0) g.y = g.x;
    return g;
}"""
WRONG_TAP_HELPER = """highp vec4 GatherEye(highp vec2 uv, float eye) {
    highp float width = uViewport.z;
    highp float lo = eye * 0.5 * width;
    highp float hi = lo + 0.5 * width;
    highp float footprint = uv.x * width - 0.5;
    highp float base = floor(footprint);
    base = clamp(base, lo, hi - 2.0);
    vec4 g = textureGather(uTex, vec2((base + 0.5) / width, uv.y), 1);
    if (footprint < lo) return vec4(g.y, g.y, g.z, g.z);
    if (footprint >= hi - 1.0) return vec4(g.x, g.x, g.w, g.w);
    return g;
}"""
FIXED_GSR_STD = "float std = 2.181818 / max(sum, 1e-5);"
OLD_GSR_STD = "float std = 2.181818 / sum;"
FIXED_EXTERNAL_BOUNDS = """    float eyeMin = cropMin.x + eye * 1.5 * uHalfTexel.x;
    float eyeMax = cropMax.x - (1.0 - eye) * 1.5 * uHalfTexel.x;
    float eyeMid = 0.5 * (cropMin.x + cropMax.x);
    sampleUv.x = clamp(sampleUv.x, min(eyeMin, eyeMid), max(eyeMax, eyeMid));"""
OLD_EXTERNAL_BOUNDS = """    float eyeMin = max(cropMin.x, eye * 0.5 + uHalfTexel.x);
    float eyeMax = min(cropMax.x, 0.5 + eye * 0.5 - uHalfTexel.x);
    sampleUv.x = clamp(sampleUv.x, eyeMin, eyeMax);"""
NO_SEAM_BOUNDS = """    float eyeMin = cropMin.x;
    float eyeMax = cropMax.x;
    sampleUv.x = clamp(sampleUv.x, eyeMin, eyeMax);"""
# Mutation: A6's 1.0-halfTexel inner seam — green on the symmetric crop, red on the
# one-sided-padding fixture (25% left-eye bleed at the right eye's seam column).
ONE_HALF_SEAM_BOUNDS = """    float eyeMin = cropMin.x + eye * uHalfTexel.x;
    float eyeMax = cropMax.x - (1.0 - eye) * uHalfTexel.x;
    sampleUv.x = clamp(sampleUv.x, eyeMin, eyeMax);"""
NO_MID_GUARD_BOUNDS = """    float eyeMin = cropMin.x + eye * 1.5 * uHalfTexel.x;
    float eyeMax = cropMax.x - (1.0 - eye) * 1.5 * uHalfTexel.x;
    sampleUv.x = clamp(sampleUv.x, eyeMin, eyeMax);"""


def compact_glsl(body: str) -> str:
    return re.sub(r"\s+", "", body)


def extract_shaders() -> tuple[str, str, str, str]:
    text = SOURCE.read_text(encoding="utf-8")
    raw = {name: body for name, body in re.findall(r'static const char\* (\w+) = R"\((.*?)\)";', text, re.S)}
    for name in ("kBlitVs", "kFoveateFs", "kGsrFs", "kExternalMovedFs"):
        if name not in raw:
            raise RuntimeError(f"missing shader {name}")
    return raw["kBlitVs"], raw["kFoveateFs"], raw["kGsrFs"], raw["kExternalMovedFs"]


def apply_mutations(gsr: str, external: str, mutations: list[str]) -> tuple[str, str]:
    for mutation in mutations:
        if mutation == "gsr-old":
            if compact_glsl(FIXED_GSR_HELPER) not in compact_glsl(gsr):
                raise RuntimeError("gsr-old mutation requires the fixed GatherEye")
            gsr = gsr.replace(FIXED_GSR_HELPER, OLD_GSR_HELPER).replace(FIXED_GSR_STD, OLD_GSR_STD)
        elif mutation == "gsr-wrong-taps":
            if compact_glsl(FIXED_GSR_HELPER) not in compact_glsl(gsr):
                raise RuntimeError("gsr-wrong-taps mutation requires the fixed GatherEye")
            gsr = gsr.replace(FIXED_GSR_HELPER, WRONG_TAP_HELPER)
        elif mutation == "gsr-old-column":
            if compact_glsl(FIXED_GSR_HELPER) not in compact_glsl(gsr):
                raise RuntimeError("gsr-old-column mutation requires the fixed GatherEye")
            gsr = gsr.replace(FIXED_GSR_HELPER, GSR_OLD_COLUMN_HELPER)
        elif mutation == "external-old":
            if compact_glsl(FIXED_EXTERNAL_BOUNDS) not in compact_glsl(external):
                raise RuntimeError("external-old mutation requires the fixed crop bounds")
            external = external.replace(FIXED_EXTERNAL_BOUNDS, OLD_EXTERNAL_BOUNDS)
        elif mutation == "external-no-seam":
            if compact_glsl(FIXED_EXTERNAL_BOUNDS) not in compact_glsl(external):
                raise RuntimeError("external-no-seam mutation requires the fixed crop bounds")
            external = external.replace(FIXED_EXTERNAL_BOUNDS, NO_SEAM_BOUNDS)
        elif mutation == "external-one-half-seam":
            if compact_glsl(FIXED_EXTERNAL_BOUNDS) not in compact_glsl(external):
                raise RuntimeError("external-one-half-seam mutation requires the fixed crop bounds")
            external = external.replace(FIXED_EXTERNAL_BOUNDS, ONE_HALF_SEAM_BOUNDS)
        elif mutation == "external-no-mid-guard":
            if compact_glsl(FIXED_EXTERNAL_BOUNDS) not in compact_glsl(external):
                raise RuntimeError("external-no-mid-guard mutation requires the fixed crop bounds")
            external = external.replace(FIXED_EXTERNAL_BOUNDS, NO_MID_GUARD_BOUNDS)
        else:
            raise RuntimeError(f"unknown mutation {mutation}")
    return gsr, external


def desktop_vertex(raw: str) -> str:
    return "#version 400 core\n" + raw


def desktop_fragment(raw: str, external: bool) -> str:
    body = re.sub(r"^\s*precision\s+(?:lowp|mediump|highp)\s+float\s*;\s*$", "", raw, flags=re.M)
    body = re.sub(r"\b(?:lowp|mediump|highp)\s+", "", body)
    if external:
        body = body.replace("samplerExternalOES", "sampler2D")
        body = re.sub(r"^\s*#extension[^\n]*\n", "", body, flags=re.M)
    return "#version 400 core\nvec3 ToLayer(vec3 g) { return g; }\n" + body


class PixelFormatDescriptor(ctypes.Structure):
    _fields_ = [
        ("nSize", ctypes.c_uint16), ("nVersion", ctypes.c_uint16), ("dwFlags", ctypes.c_uint32),
        ("iPixelType", ctypes.c_uint8), ("cColorBits", ctypes.c_uint8), ("cRedBits", ctypes.c_uint8),
        ("cRedShift", ctypes.c_uint8), ("cGreenBits", ctypes.c_uint8), ("cGreenShift", ctypes.c_uint8),
        ("cBlueBits", ctypes.c_uint8), ("cBlueShift", ctypes.c_uint8), ("cAlphaBits", ctypes.c_uint8),
        ("cAlphaShift", ctypes.c_uint8), ("cAccumBits", ctypes.c_uint8), ("cAccumRedBits", ctypes.c_uint8),
        ("cAccumGreenBits", ctypes.c_uint8), ("cAccumBlueBits", ctypes.c_uint8),
        ("cAccumAlphaBits", ctypes.c_uint8), ("cDepthBits", ctypes.c_uint8),
        ("cStencilBits", ctypes.c_uint8), ("cAuxBuffers", ctypes.c_uint8),
        ("iLayerType", ctypes.c_uint8), ("bReserved", ctypes.c_uint8),
        ("dwLayerMask", ctypes.c_uint32), ("dwVisibleMask", ctypes.c_uint32),
        ("dwDamageMask", ctypes.c_uint32),
    ]


class WglContext:
    def __init__(self) -> None:
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self.opengl32 = ctypes.WinDLL("opengl32", use_last_error=True)
        void = ctypes.c_void_p
        self.user32.CreateWindowExW.argtypes = [ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                                ctypes.c_uint32, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                                ctypes.c_int, void, void, void, void]
        self.user32.CreateWindowExW.restype = void
        self.user32.GetDC.argtypes = [void]
        self.user32.GetDC.restype = void
        self.user32.ReleaseDC.argtypes = [void, void]
        self.user32.DestroyWindow.argtypes = [void]
        self.kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
        self.kernel32.GetModuleHandleW.restype = void
        self.gdi32.ChoosePixelFormat.argtypes = [void, ctypes.POINTER(PixelFormatDescriptor)]
        self.gdi32.ChoosePixelFormat.restype = ctypes.c_int
        self.gdi32.SetPixelFormat.argtypes = [void, ctypes.c_int, ctypes.POINTER(PixelFormatDescriptor)]
        self.gdi32.SetPixelFormat.restype = ctypes.c_int
        self.opengl32.wglCreateContext.argtypes = [void]
        self.opengl32.wglCreateContext.restype = void
        self.opengl32.wglMakeCurrent.argtypes = [void, void]
        self.opengl32.wglMakeCurrent.restype = ctypes.c_int
        self.opengl32.wglDeleteContext.argtypes = [void]
        self.opengl32.wglGetProcAddress.argtypes = [ctypes.c_char_p]
        self.opengl32.wglGetProcAddress.restype = void
        self.hwnd = self.user32.CreateWindowExW(0, "STATIC", "RinCast shader pixel proof", 0x80000000,
                                                 0, 0, 64, 64, None, None,
                                                 self.kernel32.GetModuleHandleW(None), None)
        if not self.hwnd:
            raise RuntimeError(f"CreateWindowExW failed: {ctypes.get_last_error()}")
        self.dc = self.user32.GetDC(self.hwnd)
        if not self.dc:
            self.close()
            raise RuntimeError(f"GetDC failed: {ctypes.get_last_error()}")
        pfd = PixelFormatDescriptor()
        pfd.nSize = ctypes.sizeof(pfd)
        pfd.nVersion = 1
        pfd.dwFlags = 0x00000004 | 0x00000020 | 0x00000001
        pfd.iPixelType = 0
        pfd.cColorBits = 24
        pfd.cAlphaBits = 8
        fmt = self.gdi32.ChoosePixelFormat(self.dc, ctypes.byref(pfd))
        if not fmt or not self.gdi32.SetPixelFormat(self.dc, fmt, ctypes.byref(pfd)):
            self.close()
            raise RuntimeError(f"pixel format failed: {ctypes.get_last_error()}")
        self.context = self.opengl32.wglCreateContext(self.dc)
        if not self.context or not self.opengl32.wglMakeCurrent(self.dc, self.context):
            self.close()
            raise RuntimeError(f"WGL context failed: {ctypes.get_last_error()}")
        self.functions = {}
        self.version = self.call("glGetString", ctypes.c_char_p, ctypes.c_uint)(0x1F02).decode()
        self.renderer = self.call("glGetString", ctypes.c_char_p, ctypes.c_uint)(0x1F01).decode()
        self.glsl = self.call("glGetString", ctypes.c_char_p, ctypes.c_uint)(0x8B8C).decode()

    def call(self, name: str, restype, *argtypes):
        if name not in self.functions:
            address = self.opengl32.wglGetProcAddress(name.encode("ascii"))
            if address:
                self.functions[name] = ctypes.cast(address, ctypes.WINFUNCTYPE(restype, *argtypes))
            else:
                function = getattr(self.opengl32, name)
                function.restype = restype
                function.argtypes = list(argtypes)
                self.functions[name] = function
        return self.functions[name]

    def close(self) -> None:
        if getattr(self, "context", None):
            self.opengl32.wglMakeCurrent(None, None)
            self.opengl32.wglDeleteContext(self.context)
            self.context = None
        if getattr(self, "dc", None):
            self.user32.ReleaseDC(self.hwnd, self.dc)
            self.dc = None
        if getattr(self, "hwnd", None):
            self.user32.DestroyWindow(self.hwnd)
            self.hwnd = None


class GlRunner:
    def __init__(self, context: WglContext) -> None:
        self.gl = context.call
        self.vertex_array = ctypes.c_uint()
        self.gl("glGenVertexArrays", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(self.vertex_array))
        self.gl("glBindVertexArray", None, ctypes.c_uint)(self.vertex_array.value)
        self.gl("glDisable", None, ctypes.c_uint)(0x8DB9)
        self.gl("glPixelStorei", None, ctypes.c_uint, ctypes.c_int)(0x0CF5, 1)

    def compile(self, vertex: str, fragment: str) -> int:
        gl = self.gl
        shaders = []
        program = gl("glCreateProgram", ctypes.c_uint)()
        try:
            for kind, source in ((0x8B31, vertex), (0x8B30, fragment)):
                shader = gl("glCreateShader", ctypes.c_uint, ctypes.c_uint)(kind)
                shaders.append(shader)
                data = ctypes.c_char_p(source.encode("utf-8"))
                gl("glShaderSource", None, ctypes.c_uint, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p),
                   ctypes.POINTER(ctypes.c_int))(shader, 1, ctypes.byref(data), None)
                gl("glCompileShader", None, ctypes.c_uint)(shader)
                status = ctypes.c_int()
                gl("glGetShaderiv", None, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_int))(
                    shader, 0x8B81, ctypes.byref(status))
                if status.value != 1:
                    raise RuntimeError(self.info_log(shader, 0x8B84, 0x8B01))
                gl("glAttachShader", None, ctypes.c_uint, ctypes.c_uint)(program, shader)
            gl("glLinkProgram", None, ctypes.c_uint)(program)
            status = ctypes.c_int()
            gl("glGetProgramiv", None, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_int))(
                program, 0x8B82, ctypes.byref(status))
            if status.value != 1:
                raise RuntimeError(self.info_log(program, 0x8B84, 0x8B01))
            error = gl("glGetError", ctypes.c_uint)()
            if error:
                raise RuntimeError(f"OpenGL program setup error 0x{error:x}")
            return program
        finally:
            for shader in shaders:
                gl("glDeleteShader", None, ctypes.c_uint)(shader)

    def info_log(self, obj: int, size_enum: int, log_enum: int) -> str:
        length = ctypes.c_int()
        self.gl("glGetShaderiv" if size_enum == 0x8B84 else "glGetProgramiv", None, ctypes.c_uint,
                ctypes.c_uint, ctypes.POINTER(ctypes.c_int))(obj, size_enum, ctypes.byref(length))
        if length.value <= 1:
            return "shader/program info log unavailable"
        buffer = ctypes.create_string_buffer(length.value)
        self.gl("glGetShaderInfoLog" if size_enum == 0x8B84 else "glGetProgramInfoLog", None, ctypes.c_uint,
                ctypes.c_int, ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p)(obj, length.value, None, buffer)
        return buffer.value.decode("utf-8", errors="replace")

    def uniform_location(self, program: int, name: str) -> int:
        self.gl("glUseProgram", None, ctypes.c_uint)(program)
        return self.gl("glGetUniformLocation", ctypes.c_int, ctypes.c_uint, ctypes.c_char_p)(program, name.encode())

    def set1i(self, program: int, name: str, value: int) -> None:
        location = self.uniform_location(program, name)
        if location >= 0:
            self.gl("glUniform1i", None, ctypes.c_int, ctypes.c_int)(location, value)

    def set1f(self, program: int, name: str, value: float) -> None:
        location = self.uniform_location(program, name)
        if location >= 0:
            self.gl("glUniform1f", None, ctypes.c_int, ctypes.c_float)(location, value)

    def set2f(self, program: int, name: str, x: float, y: float) -> None:
        location = self.uniform_location(program, name)
        if location >= 0:
            self.gl("glUniform2f", None, ctypes.c_int, ctypes.c_float, ctypes.c_float)(location, x, y)

    def set4f(self, program: int, name: str, values: tuple[float, float, float, float]) -> None:
        location = self.uniform_location(program, name)
        if location >= 0:
            self.gl("glUniform4f", None, ctypes.c_int, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                    ctypes.c_float)(location, *values)

    def texture(self, width: int, height: int, data: np.ndarray) -> int:
        texture = ctypes.c_uint()
        self.gl("glGenTextures", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(texture))
        error = self.gl("glGetError", ctypes.c_uint)()
        if error:
            raise RuntimeError(f"OpenGL texture generation error 0x{error:x}")
        self.gl("glBindTexture", None, ctypes.c_uint, ctypes.c_uint)(0x0DE1, texture.value)
        error = self.gl("glGetError", ctypes.c_uint)()
        if error:
            raise RuntimeError(f"OpenGL texture bind error 0x{error:x}: texture={texture.value}")
        for name, value in ((0x2801, 0x2601), (0x2800, 0x2601), (0x2802, 0x812F), (0x2803, 0x812F)):
            self.gl("glTexParameteri", None, ctypes.c_uint, ctypes.c_uint, ctypes.c_int)(0x0DE1, name, value)
            error = self.gl("glGetError", ctypes.c_uint)()
            if error:
                raise RuntimeError(f"OpenGL texture parameter 0x{name:x} error 0x{error:x}")
        self.gl("glTexImage2D", None, ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)(0x0DE1, 0, 0x8814, width, height, 0, 0x1908,
                                                              0x1406, data.ctypes.data)
        error = self.gl("glGetError", ctypes.c_uint)()
        if error:
            raise RuntimeError(f"OpenGL texture upload error 0x{error:x}")
        return texture.value

    def render(self, program: int, width: int, height: int, textures: list[tuple[int, int]]) -> np.ndarray:
        gl = self.gl

        def check(stage: str) -> None:
            error = gl("glGetError", ctypes.c_uint)()
            if error:
                raise RuntimeError(f"OpenGL {stage} error 0x{error:x}")

        check("entry")
        target = ctypes.c_uint()
        gl("glGenTextures", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(target))
        gl("glBindTexture", None, ctypes.c_uint, ctypes.c_uint)(0x0DE1, target.value)
        gl("glTexParameteri", None, ctypes.c_uint, ctypes.c_uint, ctypes.c_int)(0x0DE1, 0x2801, 0x2600)
        gl("glTexParameteri", None, ctypes.c_uint, ctypes.c_uint, ctypes.c_int)(0x0DE1, 0x2800, 0x2600)
        gl("glTexImage2D", None, ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)(0x0DE1, 0, 0x8814, width, height, 0, 0x1908,
                                                              0x1406, None)
        framebuffer = ctypes.c_uint()
        gl("glGenFramebuffers", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(framebuffer))
        gl("glBindFramebuffer", None, ctypes.c_uint, ctypes.c_uint)(0x8D40, framebuffer.value)
        gl("glFramebufferTexture2D", None, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
                ctypes.c_int)(0x8D40, 0x8CE0, 0x0DE1, target.value, 0)
        status = gl("glCheckFramebufferStatus", ctypes.c_uint, ctypes.c_uint)(0x8D40)
        check("framebuffer-attachment")
        if status != 0x8CD5:
            raise RuntimeError(f"framebuffer incomplete: 0x{status:x}")
        gl("glViewport", None, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int)(0, 0, width, height)
        gl("glClearColor", None, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float)(0, 0, 0, 0)
        gl("glClear", None, ctypes.c_uint)(0x4000)
        check("clear")
        gl("glUseProgram", None, ctypes.c_uint)(program)
        check("use-program")
        for unit, texture in textures:
            gl("glActiveTexture", None, ctypes.c_uint)(0x84C0 + unit)
            gl("glBindTexture", None, ctypes.c_uint, ctypes.c_uint)(0x0DE1, texture)
            check(f"bind-texture-{unit}")
        gl("glBindVertexArray", None, ctypes.c_uint)(self.vertex_array.value)
        check("bind-vao")
        gl("glDrawArrays", None, ctypes.c_uint, ctypes.c_int, ctypes.c_int)(0x0004, 0, 3)
        check("draw")
        gl("glFinish", None)()
        pixels = np.empty((height, width, 4), dtype=np.float32)
        gl("glReadPixels", None, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                ctypes.c_uint, ctypes.c_void_p)(0, 0, width, height, 0x1908, 0x1406, pixels.ctypes.data)
        error = gl("glGetError", ctypes.c_uint)()
        if error:
            raise RuntimeError(f"OpenGL error 0x{error:x}")
        gl("glBindFramebuffer", None, ctypes.c_uint, ctypes.c_uint)(0x8D40, 0)
        gl("glDeleteFramebuffers", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(framebuffer))
        gl("glDeleteTextures", None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))(1, ctypes.byref(target))
        return np.ascontiguousarray(pixels)


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def sample_bilinear(texture: np.ndarray, u: float, v: float) -> np.ndarray:
    height, width = texture.shape[:2]
    x = u * width - 0.5
    y = v * height - 0.5
    x0 = math.floor(x)
    y0 = math.floor(y)
    fx = x - x0
    fy = y - y0
    x0 = int(clamp(x0, 0, width - 1))
    y0 = int(clamp(y0, 0, height - 1))
    x1 = int(clamp(x0 + 1, 0, width - 1))
    y1 = int(clamp(y0 + 1, 0, height - 1))
    return ((1 - fx) * (1 - fy) * texture[y0, x0] + fx * (1 - fy) * texture[y0, x1] +
            (1 - fx) * fy * texture[y1, x0] + fx * fy * texture[y1, x1]).astype(np.float32)


def gather(texture: np.ndarray, u: float, v: float, component: int = 1) -> np.ndarray:
    height, width = texture.shape[:2]
    i0 = int(clamp(math.floor(u * width - 0.5), 0, width - 1))
    j0 = int(clamp(math.floor(v * height - 0.5), 0, height - 1))
    i1 = int(clamp(i0 + 1, 0, width - 1))
    j1 = int(clamp(j0 + 1, 0, height - 1))
    return np.array([texture[j1, i0, component], texture[j1, i1, component],
                     texture[j0, i1, component], texture[j0, i0, component]], dtype=np.float32)


def check_gather_rounding(gsr_source: str) -> dict:
    """Deterministic float32 gate for the (base + c)/width floor() discontinuity.

    Unlike the GPU pixel fixtures, this does not depend on rasterizer vUv interpolation.
    It proves the shader's gather-centering constant directly: with c=1.0, u*W-0.5 sits
    0.5 texel away from the floor() edge for every even width; with c=0.5 it lands exactly
    on the edge and float32 rounding pulls a column left for many widths.
    """
    import struct as _struct
    def _f32(x):
        return _struct.unpack("<f", _struct.pack("<f", x))[0]

    uses_centered = "(base + 1.0) / width" in gsr_source
    def bad_columns(width, c):
        bad = 0
        for base in range(1, width - 2):
            u = _f32((base + c) / width)
            if math.floor(_f32(u * _f32(float(width))) - 0.5) != base:
                bad += 1
        return bad

    widths = (2496, 2828, 2880, 5376)
    centered = {w: bad_columns(w, 1.0) for w in widths}
    legacy = {w: bad_columns(w, 0.5) for w in widths}
    legacy_bad_widths = sum(1 for W in range(4, 8193, 2) if
                            math.floor(_f32(_f32((W // 2 + 0.5) / W) * _f32(float(W))) - 0.5) == W // 2 - 1)
    passed = uses_centered and all(v == 0 for v in centered.values()) and legacy_bad_widths == 578
    return {"passed": passed, "name": "gather-rounding-float32-gate",
            "uses_centered_constant": uses_centered,
            "centered_bad_columns": centered, "legacy_bad_columns": legacy,
            "legacy_bad_even_widths_4_8192": legacy_bad_widths,
            "detail": "c=1.0 must give 0 shifted columns; c=0.5 must give 578 bad widths (A6 bug)"}


def f32(x: float) -> float:
    """Round to IEEE-754 binary32, matching the shader's highp float算术 and GL uniform upload."""
    return float(np.float32(x))


def gather_eye(texture: np.ndarray, uv: tuple[float, float], eye: int) -> np.ndarray:
    """Independent per-eye 2x2 gather reference.

    Deliberately works in INTEGER texel space: it never reconstructs a normalized u as
    (base + c) / width and feeds it back through floor(u*W - 0.5).  That float round-trip
    was the source of the pseudo-independent reference in A6 — it copied both the wrong
    gather column (g.z) and the floor() discontinuity bug from the shader, so GPU and CPU
    failed identically and mismatch stayed 0.  Here the four taps are indexed directly.
    """
    height, width = texture.shape[:2]
    # Mirror the shader's highp float evaluation so non-power-of-two widths land on
    # the same texels as the GPU (uViewport carries f32(1/width)).  The independence is
    # in indexing i0/i1 directly per eye, not in the float precision.
    inv_w, w_f = f32(1.0 / width), f32(float(width))
    lo = f32(eye * f32(0.5) * w_f)
    hi = f32(lo + f32(0.5) * w_f)
    footprint = f32(f32(uv[0] * w_f) - f32(0.5))
    i0 = int(clamp(math.floor(footprint), lo, max(lo, f32(hi - f32(2.0)))))
    i1 = int(clamp(i0 + 1, 0, width - 1))
    h_f = f32(float(height))
    j0 = int(clamp(math.floor(f32(f32(uv[1] * h_f) - f32(0.5))), 0, height - 1))
    j1 = int(clamp(j0 + 1, 0, height - 1))
    # textureGather order: .x=(i0,j0+1) .y=(i1,j0+1) .z=(i1,j0) .w=(i0,j0)
    result = np.array([texture[j1, i0, 1], texture[j1, i1, 1],
                       texture[j0, i1, 1], texture[j0, i0, 1]], dtype=np.float32)
    if footprint < lo:
        return np.array([result[0], result[0], result[3], result[3]], dtype=np.float32)
    if footprint >= hi - 1.0:
        return np.array([result[1], result[1], result[2], result[2]], dtype=np.float32)
    return result


def fast_lanczos2(x: float) -> float:
    w_a = x - 4.0
    w_b = x * w_a - w_a
    w_a *= w_a
    return w_b * w_a


def weight_y(dx: float, dy: float, c: float, std: float) -> np.ndarray:
    x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0)
    w = fast_lanczos2(x)
    return np.array([w, w * c], dtype=np.float32)


def reference_gsr(texture: np.ndarray) -> np.ndarray:
    height, width = texture.shape[:2]
    output = np.empty_like(texture)
    # uViewport.zw = f32(width/height), uViewport.xy = f32(1/width, 1/height).
    w_f, h_f = f32(float(width)), f32(float(height))
    inv_w, inv_h = f32(1.0 / width), f32(1.0 / height)
    for y in range(height):
        for x in range(width):
            v_uv = (f32((x + 0.5) * inv_w), f32((y + 0.5) * inv_h))
            color = sample_bilinear(texture, *v_uv).copy()
            img_x = f32(f32(v_uv[0] * w_f) - f32(0.5))
            img_y = f32(f32(v_uv[1] * h_f) + f32(0.5))
            pixel_x = math.floor(img_x)
            pixel_y = math.floor(img_y)
            coord = (f32(pixel_x * inv_w), f32(pixel_y * inv_h))
            pl = (f32(img_x - pixel_x), f32(img_y - pixel_y))
            eye = 1 if v_uv[0] >= 0.5 else 0
            left = gather_eye(texture, coord, eye)
            edge_vote = abs(left[2] - left[1]) + abs(color[1] - left[1]) + abs(color[1] - left[2])
            if edge_vote > 8.0 / 255.0:
                shifted = (f32(coord[0] + inv_w), coord[1])
                right = gather_eye(texture, (f32(shifted[0] + inv_w), shifted[1]), eye)
                up_down = np.empty(4, dtype=np.float32)
                up_down[:2] = gather_eye(texture, (shifted[0], f32(shifted[1] - inv_h)), eye)[[3, 2]]
                up_down[2:] = gather_eye(texture, (shifted[0], f32(shifted[1] + inv_h)), eye)[[1, 0]]
                mean = (left[1] + left[2] + right[0] + right[3]) * 0.25
                left = left - mean
                right = right - mean
                up_down = up_down - mean
                color_w = color[1] - mean
                total = float(np.abs(left).sum() + np.abs(right).sum() + np.abs(up_down).sum())
                std = 2.181818 / max(total, 1e-5)
                weights = np.zeros(2, dtype=np.float32)
                weights += weight_y(pl[0], pl[1] + 1.0, up_down[0], std)
                weights += weight_y(pl[0] - 1.0, pl[1] + 1.0, up_down[1], std)
                weights += weight_y(pl[0] - 1.0, pl[1] - 2.0, up_down[2], std)
                weights += weight_y(pl[0], pl[1] - 2.0, up_down[3], std)
                weights += weight_y(pl[0] + 1.0, pl[1] - 1.0, left[0], std)
                weights += weight_y(pl[0], pl[1] - 1.0, left[1], std)
                weights += weight_y(pl[0], pl[1], left[2], std)
                weights += weight_y(pl[0] + 1.0, pl[1], left[3], std)
                weights += weight_y(pl[0] - 1.0, pl[1] - 1.0, right[0], std)
                weights += weight_y(pl[0] - 2.0, pl[1] - 1.0, right[1], std)
                weights += weight_y(pl[0] - 2.0, pl[1], right[2], std)
                weights += weight_y(pl[0] - 1.0, pl[1], right[3], std)
                final_y = weights[1] / weights[0]
                max_y = max(max(left[1], left[2]), max(right[0], right[3]))
                min_y = min(min(left[1], left[2]), min(right[0], right[3]))
                final_y = clamp(2.0 * final_y, min_y, max_y)
                delta = clamp(final_y - color_w, -23.0 / 255.0, 23.0 / 255.0)
                color[:3] = np.clip(color[:3] + delta, 0.0, 1.0)
            output[y, x] = [color[0], color[1], color[2], 1.0]
    return output


def reference_external(texture: np.ndarray, crop: tuple[float, float, float, float],
                       out_width: int, out_height: int, motion: np.ndarray) -> np.ndarray:
    height, width = texture.shape[:2]
    half_x = 0.5 / width
    output = np.empty((out_height, out_width, 4), dtype=np.float32)
    for y in range(out_height):
        for x in range(out_width):
            v_uv = ((x + 0.5) / out_width, (y + 0.5) / out_height)
            uv = (v_uv[0], 1.0 - v_uv[1])
            eye = 1 if v_uv[0] >= 0.5 else 0
            motion_uv = (clamp(uv[0], eye * 0.5 + 0.25, 0.5 + eye * 0.5 - 0.25),
                         clamp(uv[1], 0.25, 0.75))
            movement = sample_bilinear(motion, *motion_uv)
            uv = (uv[0] - movement[0], uv[1] - movement[1])
            uv = (clamp(uv[0], eye * 0.5, 0.5 + eye * 0.5), clamp(uv[1], 0.0, 1.0))
            sample = (crop[0] + uv[0] * crop[2], crop[1] + uv[1] * crop[3])
            crop_min = crop[0] + eye * 0.5 * crop[2]
            crop_max = crop_min + 0.5 * crop[2]
            eye_min = crop_min + eye * 1.5 * half_x
            eye_max = crop_max - (1.0 - eye) * 1.5 * half_x
            eye_mid = 0.5 * (crop_min + crop_max)
            sample = (clamp(sample[0], min(eye_min, eye_mid), max(eye_max, eye_mid)),
                      clamp(sample[1], crop[1], crop[1] + crop[3]))
            output[y, x] = [*sample_bilinear(texture, *sample)[:3], 1.0]
    return output


def gsr_texture(gradient: bool, width: int = 16) -> np.ndarray:
    """Left-red/right-blue fixture.  width is the ACTUAL buffer width and must be even.

    Non-power-of-two widths (52, 2880, ...) exercise the float32 floor() discontinuity
    that (base+0.5)/width used to land on; power-of-two 16 masks it.
    """
    half = width // 2
    texture = np.zeros((8, width, 4), dtype=np.float32)
    if gradient:
        rng = np.random.default_rng(0xA5)
        texture[:, :, :3] = rng.random((8, width, 3), dtype=np.float32)
        texture[:, :half, 0] = rng.uniform(0.75, 1.0, (8, half))
        texture[:, :half, 2] = rng.uniform(0.0, 0.25, (8, half))
        texture[:, half:, 0] = rng.uniform(0.0, 0.25, (8, half))
        texture[:, half:, 2] = rng.uniform(0.75, 1.0, (8, half))
        texture[:, :, 3] = 1.0
        return texture
    for x in range(width):
        color = (1.0, 0.0, 0.0, 1.0) if x < half else (0.0, 0.0, 1.0, 1.0)
        texture[:, x] = color
    return texture


def external_texture(gradient: bool) -> tuple[np.ndarray, tuple[float, float, float, float]]:
    width, height = 2880, 16
    crop = (16.0 / width, 1.0 / height, 2828.0 / width, 14.0 / height)
    texture = np.zeros((height, width, 4), dtype=np.float32)
    texture[:] = (0.0, 1.0, 0.0, 1.0)
    seam = 1430.0
    for x in range(16, 2844):
        if x < int(seam):
            color = (1.0, x / width, 0.0, 1.0) if gradient else (1.0, 0.0, 0.0, 1.0)
        else:
            color = ((x - seam) / (width - seam), 0.0, 1.0, 1.0) if gradient else (0.0, 0.0, 1.0, 1.0)
        texture[:, x] = color
    return texture, crop


def external_single_sided_texture() -> tuple[np.ndarray, tuple[float, float, float, float]]:
    """Real MediaCodec/V4L2 layout: 2828-wide visible frame in a 2880-wide buffer.

    Padding is on the RIGHT only (left=0, right=2828).  The decoder applies its
    half-texel inset only beside padding (video_decoder.cpp:558-561), so the published
    crop is (0, 0, 2827.5/2880, 1).  That makes the true eye seam sit at texel 1414.0,
    while crop.x + 0.5*crop.z = 1413.75 — a 0.25-texel offset toward the left eye.
    A one-sided inset of only 1.0*uHalfTexel then samples 25% of the left eye into the
    right eye's seam column.
    """
    width, height = 2880, 16
    left, right = 0, 2828
    texture = np.zeros((height, width, 4), dtype=np.float32)
    texture[:] = (0.0, 0.0, 0.0, 1.0)  # right-side padding: black
    for x in range(left, right):
        color = (1.0, 0.0, 0.0, 1.0) if x < right // 2 else (0.0, 0.0, 1.0, 1.0)
        texture[:, x] = color
    x0 = (left + (0.5 if left > 0 else 0.0)) / width
    x1 = (right - (0.5 if right < width else 0.0)) / width
    crop = (x0, 0.0, x1 - x0, 1.0)
    return texture, crop


def external_seam_motion() -> np.ndarray:
    motion = np.zeros((2, 2, 4), dtype=np.float32)
    motion[:, :, 0] = (-0.02, 0.02)
    motion[:, :, 3] = 1.0
    return motion


def compare(name: str, actual: np.ndarray, expected: np.ndarray, tolerance: float,
            solid_eyes: bool, out_dir: Path) -> dict:
    finite = bool(np.isfinite(actual).all())
    difference = np.abs(actual - expected)
    max_error = float(difference.max())
    mismatch = difference > tolerance
    mismatch_count = int(mismatch.any(axis=2).sum())
    isolation_errors = 0
    if solid_eyes:
        height, width = actual.shape[:2]
        left = actual[:, : width // 2]
        right = actual[:, width // 2 :]
        isolation_errors = int((left[:, :, 2] > tolerance).sum() + (right[:, :, 0] > tolerance).sum())
    passed = finite and mismatch_count == 0 and isolation_errors == 0
    image = np.rint(np.clip(actual[:, :, :3], 0.0, 1.0) * 255.0).astype(np.uint8)
    write_png(out_dir / f"{name}.png", image)
    first = None
    if mismatch_count:
        y, x = np.argwhere(mismatch.any(axis=2))[0]
        first = {"x": int(x), "y": int(y), "actual": actual[y, x].tolist(),
                 "expected": expected[y, x].tolist()}
    return {"name": name, "passed": passed, "finite": finite, "tolerance": tolerance,
            "mismatch_pixels": mismatch_count, "max_abs_error": max_error,
            "isolation_errors": isolation_errors, "first_mismatch": first,
            "image": str((out_dir / f"{name}.png").relative_to(ROOT))}


def write_png(path: Path, rgb: np.ndarray) -> None:
    height, width = rgb.shape[:2]
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(height))
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mutate", action="append", default=[],
                        choices=["gsr-old", "gsr-wrong-taps", "gsr-old-column",
                                 "external-old", "external-no-seam",
                                 "external-one-half-seam", "external-no-mid-guard"])
    parser.add_argument("--label", default="current")
    parser.add_argument("--out", type=Path, default=ROOT / "build/evidence/shader-pixels")
    args = parser.parse_args()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    vertex_raw, foveate_raw, gsr_raw, external_raw = extract_shaders()
    gsr_raw, external_raw = apply_mutations(gsr_raw, external_raw, args.mutate)
    vertex = desktop_vertex(vertex_raw)
    gsr_fragment = desktop_fragment(gsr_raw, False)
    external_fragment = desktop_fragment(foveate_raw + external_raw, True)
    context = WglContext()
    runner = GlRunner(context)
    results = []
    try:
        gsr_program = runner.compile(vertex, gsr_fragment)
        external_program = runner.compile(vertex, external_fragment)
        runner.set4f(gsr_program, "uRect", (0.0, 0.0, 1.0, 1.0))
        # w16 (power of two) is the pixel-exact fixture: uViewport=1/16 is exact, so the
        # double/float coordinate chains agree and it cleanly distinguishes g.w from g.z.
        # Non-power-of-two widths are covered by solid fixtures (cross-eye isolation) and
        # by the deterministic float32 rounding gate below; their gradient fixtures are
        # excluded because rasterizer vUv interpolation is driver-specific, not a contract.
        for width, gradient in ((16, False), (16, True), (52, False), (2880, False)):
            texture = gsr_texture(gradient, width)
            suffix = "gradient" if gradient else "solid"
            name = f"{args.label}-gsr-w{width}-{suffix}"
            gpu_texture = runner.texture(texture.shape[1], texture.shape[0], texture)
            runner.set4f(gsr_program, "uViewport", (1.0 / texture.shape[1], 1.0 / texture.shape[0],
                                                    float(texture.shape[1]), float(texture.shape[0])))
            runner.set1i(gsr_program, "uTex", 0)
            actual = runner.render(gsr_program, texture.shape[1], texture.shape[0], [(0, gpu_texture)])
            expected = reference_gsr(texture)
            results.append(compare(name, actual, expected, 0.01, not gradient, out_dir))
        rounding_result = check_gather_rounding(gsr_raw)
        results.append(rounding_result)
        motion = np.zeros((2, 2, 4), dtype=np.float32)
        motion_texture = runner.texture(2, 2, motion)
        runner.set4f(external_program, "uRect", (0.0, 0.0, 1.0, 1.0))
        runner.set1i(external_program, "uFoveated", 0)
        runner.set1i(external_program, "uTex", 0)
        runner.set1i(external_program, "uMotion", 1)
        runner.set2f(external_program, "uScale", 1.0, 1.0)
        runner.set2f(external_program, "uField", 1.0, 1.0)
        runner.set2f(external_program, "uMotionHalfTexel", 0.25, 0.25)
        for gradient in (False, True):
            texture, crop = external_texture(gradient)
            name = f"{args.label}-external-{'gradient' if gradient else 'solid'}"
            gpu_texture = runner.texture(texture.shape[1], texture.shape[0], texture)
            runner.set4f(external_program, "uCrop", crop)
            runner.set2f(external_program, "uHalfTexel", 0.5 / texture.shape[1], 0.5 / texture.shape[0])
            actual = runner.render(external_program, 512, 8, [(0, gpu_texture), (1, motion_texture)])
            expected = reference_external(texture, crop, 512, 8, motion)
            results.append(compare(name, actual, expected, 0.005, not gradient, out_dir))
        texture, crop = external_texture(False)
        motion = external_seam_motion()
        motion_texture = runner.texture(2, 2, motion)
        gpu_texture = runner.texture(texture.shape[1], texture.shape[0], texture)
        runner.set4f(external_program, "uCrop", crop)
        runner.set2f(external_program, "uHalfTexel", 0.5 / texture.shape[1], 0.5 / texture.shape[0])
        name = f"{args.label}-external-motion-seam"
        actual = runner.render(external_program, 512, 8, [(0, gpu_texture), (1, motion_texture)])
        expected = reference_external(texture, crop, 512, 8, motion)
        results.append(compare(name, actual, expected, 0.005, True, out_dir))

        # Real one-sided decoder padding: 2828 visible in a 2880 buffer, right pad only.
        # A 1.0 half-texel inner seam mixes 25% of the left eye into the right seam; the
        # 1.5 half-texel fix removes it.  Isolation check catches cross-eye bleed.
        texture, crop = external_single_sided_texture()
        motion = external_seam_motion()
        motion_texture = runner.texture(2, 2, motion)
        gpu_texture = runner.texture(texture.shape[1], texture.shape[0], texture)
        runner.set4f(external_program, "uCrop", crop)
        runner.set2f(external_program, "uHalfTexel", 0.5 / texture.shape[1], 0.5 / texture.shape[0])
        name = f"{args.label}-external-one-sided-pad"
        actual = runner.render(external_program, 512, 8, [(0, gpu_texture), (1, motion_texture)])
        expected = reference_external(texture, crop, 512, 8, motion)
        results.append(compare(name, actual, expected, 0.005, True, out_dir))
        report = {
            "verified_at_utc": datetime.now(timezone.utc).isoformat(),
            "status": "VERIFIED(host GL)" if all(item["passed"] for item in results) else "FAILED",
            "source": str(SOURCE),
            "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            "mutations": args.mutate,
            "context": {"version": context.version, "renderer": context.renderer, "glsl": context.glsl},
            "coordinate_contract": {
                "gsr": "main passes normalized UV; GatherEye converts to footprint uv.x*width-0.5 and keeps both gather columns inside the raster eye",
                "external": "crop is actual-buffer UV; seam is crop.x + 0.5*crop.z; only the inner seam receives one half-texel inset",
            },
            "hand_calculations": [
                "GSR right-eye first pixel: uv.x=0.5, footprint=7.5<lo=8, replicate right-eye texel 8 blue; old helper gathers texel 7 red",
                "GSR left-eye last pixel: uv.x=7/16, footprint=6.5, base=6, taps 6/7 remain red",
                "GSR left boundary: textureGather .w=(i0,j0) is the left column; the old vec4(g.x,g.x,g.z,g.z) used g.z=(i1,j0) from the next column",
                "GSR rounding: (base+0.5)/W puts u*W-0.5 on a floor() edge; (base+1.0)/W gives 0.5 texel of float32 margin. 578 even widths in 4..8192 shift a column; W=2496/2828/2880/5376 give 123/174/162/268 bad columns",
                "External left seam: seam=1430 px, clamp to 1429.5/2880 (texel 1429 center), no blue blend",
                "External right seam: clamp to 1430.5/2880 (texel 1430 center), no 10.5 px crop shift",
                "External one-sided pad: right-only padding 2828/2880 makes true seam texel 1414.0 but crop midpoint 1413.75; 1.0 half-texel gives 25% left-eye bleed, 1.5 half-texels lands exactly on 1414.0",
            ],
            "fixtures": {
                "gsr": "16x8 / 52x8 / 2880x8, left red/right blue or asymmetric gradients; 52 and 2880 are non-power-of-two widths that expose the floor() discontinuity",
                "external": "2880x16 actual buffer, 2828x14 crop at offset (16,1), 36 px right and 1 px vertical padding; plus right-only padding 2828/2880 (crop 0..2827.5), which exposes one-sided seam offset",
            },
            "results": results,
            "real_device_verified": False,
        }
        report_path = out_dir / f"{args.label}.json"
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        for item in results:
            if "mismatch_pixels" in item:
                print(f"{'PASS' if item['passed'] else 'FAIL'} {item['name']} "
                      f"mismatch={item['mismatch_pixels']} max_error={item['max_abs_error']:.6f} "
                      f"isolation={item['isolation_errors']} finite={item['finite']}")
            else:
                print(f"{'PASS' if item['passed'] else 'FAIL'} {item['name']} "
                      f"centered_bad={sum(item['centered_bad_columns'].values())} "
                      f"legacy_bad_widths={item['legacy_bad_even_widths_4_8192']}")
        print(f"REPORT {report_path}")
        print(f"CONTEXT {context.version} | {context.renderer} | {context.glsl}")
        return 0 if all(item["passed"] for item in results) else 1
    finally:
        context.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        traceback.print_exc()
        print(f"ERROR {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(2)
