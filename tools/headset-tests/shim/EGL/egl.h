#pragma once
typedef void* EGLDisplay;
typedef void* EGLClientBuffer;
typedef void* EGLImageKHR;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
inline EGLDisplay eglGetCurrentDisplay() { return EGL_NO_DISPLAY; }
inline unsigned int eglGetError() { return 0x3000; }
