// Hardware video decoding on Linux through a V4L2 stateful decoder: on the Steam Frame, Qualcomm's
// iris driver at /dev/video-dec0 (H.264 and 8-bit HEVC). Bitstream goes into mmap'd OUTPUT
// buffers; decoded NV12 pictures come back in CAPTURE buffers, which are exported as dmabufs and
// imported into EGL as external textures, without copies.
//
// The Frame's driver has quirks this follows: the CAPTURE side may only be set up after the
// driver's source-change event (before it, its size is a placeholder and the buffers come out
// too small), and each dmabuf is imported once and kept (importing per frame crashes its kernel).
#include "video_decoder.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include "rc/platform.h"

// DRM_FORMAT_NV12 (drm_fourcc.h): the same fourcc as V4L2's NV12.
constexpr int kDrmFormatNv12 = int(v4l2_fourcc('N', 'V', '1', '2'));

namespace rv {

namespace {

PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;

void LoadEglProcs() {
    if (pCreateImage) return;
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
}

int Ioctl(int fd, unsigned long request, void* arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

uint32_t PixelFormat(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return V4L2_PIX_FMT_H264;
        case rc::VideoCodec::HEVC: return V4L2_PIX_FMT_HEVC;
        case rc::VideoCodec::AV1: return v4l2_fourcc('A', 'V', '0', '1');
    }
    return V4L2_PIX_FMT_HEVC;
}

bool HasOutputFormat(int fd, uint32_t fourcc) {
    for (uint32_t i = 0;; i++) {
        v4l2_fmtdesc d{};
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (Ioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) return false;
        if (d.pixelformat == fourcc) return true;
    }
}

// The Frame names its decoder /dev/video-dec0. Elsewhere, the first memory-to-memory device that
// takes H.264 or HEVC in.
int OpenDecoder() {
    int fd = open("/dev/video-dec0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) return fd;
    for (int i = 0; i < 64; i++) {
        const std::string path = "/dev/video" + std::to_string(i);
        fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        const uint32_t caps = Ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
                                  ? (cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities)
                                  : 0;
        if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
            (HasOutputFormat(fd, V4L2_PIX_FMT_HEVC) || HasOutputFormat(fd, V4L2_PIX_FMT_H264)))
            return fd;
        close(fd);
    }
    return -1;
}

// v4l2_buffer::timestamp: timeval in older kernel headers (the SDK's), v4l2_timeval in newer ones.
using BufferTime = decltype(v4l2_buffer::timestamp);

BufferTime Timestamp(uint32_t frameIndex) {
    BufferTime t{};
    t.tv_sec = frameIndex / 1'000'000;
    t.tv_usec = frameIndex % 1'000'000;
    return t;
}

uint32_t FrameIndex(const BufferTime& t) { return uint32_t(t.tv_sec * 1'000'000 + t.tv_usec); }

}  // namespace

VideoDecoder::~VideoDecoder() { Shutdown(); }

DecoderCaps VideoDecoder::Capabilities() {
    DecoderCaps caps;
    caps.h264 = caps.hevc = false;
    const int fd = OpenDecoder();
    if (fd < 0) {
        RC_LOGE("No V4L2 video decoder found");
        return caps;
    }
    caps.h264 = HasOutputFormat(fd, V4L2_PIX_FMT_H264);
    caps.hevc = HasOutputFormat(fd, V4L2_PIX_FMT_HEVC);
    // The Frame's firmware could do AV1 and 10-bit, but its driver offers neither yet.
    for (uint32_t i = 0;; i++) {
        v4l2_frmsizeenum s{};
        s.index = i;
        s.pixel_format = caps.hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
        if (Ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &s) < 0) break;
        if (s.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            caps.maxWidth = std::max(caps.maxWidth, int(s.discrete.width));
            caps.maxHeight = std::max(caps.maxHeight, int(s.discrete.height));
        } else {
            caps.maxWidth = int(s.stepwise.max_width);
            caps.maxHeight = int(s.stepwise.max_height);
            break;
        }
    }
    close(fd);
    RC_LOGI("V4L2 decoder: H.264 %d, HEVC %d, up to %dx%d", caps.h264, caps.hevc, caps.maxWidth, caps.maxHeight);
    return caps;
}

bool VideoDecoder::Init(rc::VideoCodec codec, int width, int height, float fps) {
    LoadEglProcs();
    ResetTextureGeometry();
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps > 1 ? fps : 72;  // ROUND18 L4 fallback
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        RC_LOGE("Decoder %s %dx%d: glGenTextures failed", CodecName(codec), width, height);
        m_failed = true;
        return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const float crop[4] = {0, 0, 0, 0};
    PublishTextureGeometry(texture, 0, 0, crop);
    if (!pCreateImage || !pImageTargetTexture) {
        RC_LOGE("Decoder %s: EGL can't import dmabufs (eglCreateImageKHR / glEGLImageTargetTexture2DOES missing)",
                CodecName(codec));
        m_failed = true;
        return false;
    }

    m_device = OpenDecoder();
    if (m_device < 0) {
        RC_LOGE("Decoder %s %dx%d: no V4L2 decoder", CodecName(codec), width, height);
        m_failed = true;
        return false;
    }
    // Bitstream in: one plane per buffer, big enough for a keyframe.
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = PixelFormat(codec);
    fmt.fmt.pix_mp.width = uint32_t(width);
    fmt.fmt.pix_mp.height = uint32_t(height);
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = uint32_t(std::max(width * height, 2 << 20));
    if (Ioctl(m_device, VIDIOC_S_FMT, &fmt) < 0) {
        RC_LOGE("Decoder %s %dx%d: the driver refused the format (%s)", CodecName(codec), width, height, strerror(errno));
        m_failed = true;
        return false;
    }
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    Ioctl(m_device, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = V4L2_EVENT_EOS;
    Ioctl(m_device, VIDIOC_SUBSCRIBE_EVENT, &sub);

    v4l2_requestbuffers req{};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_device, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        RC_LOGE("Decoder %s: no bitstream buffers (%s)", CodecName(codec), strerror(errno));
        m_failed = true;
        return false;
    }
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = &plane;
        if (Ioctl(m_device, VIDIOC_QUERYBUF, &buf) < 0) break;
        void* p = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_device, plane.m.mem_offset);
        if (p == MAP_FAILED) break;
        m_bitstream.push_back({p, plane.length});
    }
    m_bitstreamFree.assign(m_bitstream.size(), true);
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (m_bitstream.empty() || Ioctl(m_device, VIDIOC_STREAMON, &type) < 0) {
        RC_LOGE("Decoder %s: couldn't start the bitstream queue (%s)", CodecName(codec), strerror(errno));
        m_failed = true;
        return false;
    }
    RC_LOGI("Decoder %s %dx%d opened (%zu bitstream buffers)", CodecName(codec), width, height, m_bitstream.size());
    m_running = true;
    m_inputThread = std::thread([this] { InputLoop(); });
    m_outputThread = std::thread([this] { OutputLoop(); });
    return true;
}

void VideoDecoder::DestroyRetiredImagesLocked() {
    // Caller holds m_pictureMutex AND a current EGL context (GL thread only).
    if (!pDestroyImage) {
        m_retiredImages.clear();
        return;
    }
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        // A caller outside the GL thread (or an unusually early context teardown): keep the
        // images queued instead of handing EGL_NO_DISPLAY to the driver, which would make
        // the destroy a silent no-op and leak them.  The next GL-thread pass drains them.
        return;
    }
    for (EGLImageKHR image : m_retiredImages) {
        if (image != EGL_NO_IMAGE_KHR) pDestroyImage(display, image);
    }
    m_retiredImages.clear();
}

void VideoDecoder::Shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_inputThread.joinable()) m_inputThread.join();
    if (m_outputThread.joinable()) m_outputThread.join();
    if (m_device >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        Ioctl(m_device, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        Ioctl(m_device, VIDIOC_STREAMOFF, &type);
    }
    {
        std::lock_guard lock(m_pictureMutex);
        // Shutdown runs on the GL thread: safe to destroy EGLImages here, but collect the
        // live ones into the same retired queue so all destruction goes through one path.
        for (auto& p : m_pictures)
            if (p.image != EGL_NO_IMAGE_KHR) m_retiredImages.push_back(p.image);
        DestroyRetiredImagesLocked();
        for (auto& p : m_pictures)
            if (p.fd >= 0) close(p.fd);
        m_pictures.clear();
    }
    for (auto& b : m_bitstream) munmap(b.data, b.length);
    m_bitstream.clear();
    if (m_device >= 0) close(m_device);
    m_device = -1;
    auto geometry = Geometry();
    ResetTextureGeometry();
    if (geometry.texture) glDeleteTextures(1, &geometry.texture);
    m_configured = false;
    m_captureReady = false;
    m_captureFormat = m_captureWidth = m_captureHeight = m_captureStride = m_captureUvOffset = 0;
    m_ready.clear();
    m_shown = m_retiredPicture = -1;
    ResetTextureGeometry();
}

// Called by the output thread once the driver knows the stream's size.
bool VideoDecoder::SetUpCapture() {
    const auto fail = [&] {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        Ioctl(m_device, VIDIOC_STREAMOFF, &type);
        {
            std::lock_guard lock(m_pictureMutex);
            // fail() runs on the output thread with NO current EGL context: eglGetCurrentDisplay()
            // would be EGL_NO_DISPLAY and pDestroyImage would leak.  Queue the EGLImages for
            // the GL thread to destroy in Latch()/Shutdown().
            for (auto& picture : m_pictures) {
                if (picture.image != EGL_NO_IMAGE_KHR) m_retiredImages.push_back(picture.image);
                if (picture.fd >= 0) close(picture.fd);
            }
            m_pictures.clear();
            m_ready.clear();
            m_shown = m_retiredPicture = -1;
            // Fail is a batch boundary just like a rebuild: a Latched copy already taken by
            // the GL thread still carries the old generation, and must not bind, publish
            // geometry or write m_shown after the queue was torn down.
            ++m_batchGeneration;
        }
        m_configured = false;
        m_captureReady = false;
        m_captureFormat = m_captureWidth = m_captureHeight = m_captureStride = m_captureUvOffset = 0;
        // Keep the GL texture handle alive until Shutdown() deletes it.  Resetting the
        // geometry here set m_texture=0, making Shutdown's glDeleteTextures a no-op and
        // leaking the object on every failed rollback / resolution change.
        {
            const float zero[4] = {0, 0, 0, 0};
            const GLuint texture = Geometry().texture;  // locked snapshot
            PublishTextureGeometry(texture, 0, 0, zero);
            std::lock_guard lock(m_pictureMutex);
            std::memcpy(m_pendingCrop, zero, sizeof(m_pendingCrop));
        }
        return false;
    };
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Ioctl(m_device, VIDIOC_G_FMT, &fmt) < 0) return fail();
    // Linear NV12: what EGL can import as one dmabuf with two planes.
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (Ioctl(m_device, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
        RC_LOGE("Decoder: the driver won't give linear NV12 (%s)", strerror(errno));
        return fail();
    }
    const uint32_t captureWidth = fmt.fmt.pix_mp.width;
    const uint32_t captureHeight = fmt.fmt.pix_mp.height;
    const uint32_t captureStride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    const uint32_t captureUvOffset = captureStride * captureHeight;

    // The visible part (the rest is the decoder's padding).
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    uint32_t vx = 0, vy = 0, vw = uint32_t(m_width), vh = uint32_t(m_height);
    if (Ioctl(m_device, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width && sel.r.height) {
        vx = uint32_t(sel.r.left);
        vy = uint32_t(sel.r.top);
        vw = sel.r.width;
        vh = sel.r.height;
    }
    if (!captureWidth || !captureHeight || !captureStride || !vw || !vh ||
        uint64_t(vx) + vw > captureWidth || uint64_t(vy) + vh > captureHeight) {
        RC_LOGE("Decoder: invalid capture geometry %ux%u stride %u visible %ux%u at %u,%u", captureWidth,
                captureHeight, captureStride, vw, vh, vx, vy);
        return fail();
    }
    const float W = float(captureWidth), H = float(captureHeight);
    // Decoder crop already carries the outer half-texel insets used by both backends.
    const float x0 = (vx + (vx > 0 ? 0.5f : 0.f)) / W, y0 = (vy + (vy > 0 ? 0.5f : 0.f)) / H;
    const float x1 = (vx + vw - (vx + vw < captureWidth ? 0.5f : 0.f)) / W;
    const float y1 = (vy + vh - (vy + vh < captureHeight ? 0.5f : 0.f)) / H;
    const float nextCrop[4] = {x0, y0, x1 - x0, y1 - y0};

    v4l2_control minBuffers{};
    minBuffers.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    const int needed = Ioctl(m_device, VIDIOC_G_CTRL, &minBuffers) == 0 ? minBuffers.value : 4;
    v4l2_requestbuffers req{};
    req.count = uint32_t(needed + 3);  // + shown, the one before it, and one ready
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_device, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        RC_LOGE("Decoder: no picture buffers (%s)", strerror(errno));
        return fail();
    }
    bool exported = true;
    {
        std::lock_guard lock(m_pictureMutex);
        // Rebuilding the batch (resolution switch) must retire the previous generation's
        // resources.  Old fd and EGLImage with them; queue the images for GL-thread
        // destruction and close the fds here (close is safe on any thread).
        // Ownership rule: an EGLImage stored in m_pictures is destroyed exactly once, through
        // m_retiredImages.  Nothing else may destroy it — that is what made the old
        // generation-mismatch path a double free.
        for (auto& picture : m_pictures) {
            if (picture.image != EGL_NO_IMAGE_KHR) m_retiredImages.push_back(picture.image);
            if (picture.fd >= 0) close(picture.fd);
        }
        const uint64_t generation = ++m_batchGeneration;
        m_pictures.assign(req.count, {});
        for (auto& picture : m_pictures) picture.generation = generation;
        m_ready.clear();  // stale indices from the old batch must not be latched
        m_shown = m_retiredPicture = -1;
        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_exportbuffer ex{};
            ex.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ex.index = i;
            ex.plane = 0;
            ex.flags = O_RDONLY | O_CLOEXEC;
            if (Ioctl(m_device, VIDIOC_EXPBUF, &ex) < 0) {
                RC_LOGE("Decoder: couldn't export picture buffer %u as a dmabuf (%s)", i, strerror(errno));
                exported = false;
                break;
            }
            m_pictures[i].fd = ex.fd;
        }
    }
    if (!exported) return fail();
    for (uint32_t i = 0; i < req.count; i++) QueuePicture(int(i));
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Ioctl(m_device, VIDIOC_STREAMON, &type) < 0) {
        RC_LOGE("Decoder: couldn't start the picture queue (%s)", strerror(errno));
        return fail();
    }
    {
        std::lock_guard lock(m_pictureMutex);
        m_captureFormat = V4L2_PIX_FMT_NV12;
        m_captureWidth = captureWidth;
        m_captureHeight = captureHeight;
        m_captureStride = captureStride;
        m_captureUvOffset = captureUvOffset;
        std::memcpy(m_pendingCrop, nextCrop, sizeof(m_pendingCrop));
        // Geometry is published by Latch() once the first EGLImage of this batch is bound,
        // so a resolution switch never exposes "new width/height + old picture".
    }
    RC_LOGI("Decoder %s: %ux%u NV12 pictures (stride %u, visible %ux%u at %u,%u), %u buffers", CodecName(m_codec),
            captureWidth, captureHeight, captureStride, vw, vh, vx, vy, req.count);
    m_configured = true;
    return true;
}

void VideoDecoder::QueuePicture(int index) {
    v4l2_plane plane{};
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = uint32_t(index);
    buf.length = 1;
    buf.m.planes = &plane;
    if (Ioctl(m_device, VIDIOC_QBUF, &buf) < 0) Warn("couldn't give a picture buffer back to the driver");
}

bool VideoDecoder::Configure(const std::vector<uint8_t>&) { return true; }  // V4L2 starts in Init

void VideoDecoder::InputLoop() {
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), -10);  // best effort
    while (m_running) {
        Pending p;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return !m_running || !m_queue.empty(); });
            if (!m_running) break;
            if (m_queue.empty()) continue;
            p = std::move(m_queue.front());
            m_queue.pop_front();
        }
        // A free bitstream buffer: never used yet, or one the driver is done with.
        int index = -1;
        for (int attempt = 0; attempt < 50 && m_running && index < 0; attempt++) {
            for (size_t i = 0; i < m_bitstreamFree.size(); i++)
                if (m_bitstreamFree[i]) {
                    index = int(i);
                    break;
                }
            if (index >= 0) break;
            v4l2_plane plane{};
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.length = 1;
            buf.m.planes = &plane;
            if (Ioctl(m_device, VIDIOC_DQBUF, &buf) == 0) {
                m_bitstreamFree[buf.index] = true;
                continue;
            }
            pollfd pfd{m_device, POLLOUT, 0};
            poll(&pfd, 1, 2);
        }
        if (index < 0) {
            Warn("no free bitstream buffer for 100 ms, the decoder is falling behind");
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_seenKeyframe = false;
            m_lastFrameIndex = m_lastQueued;
            m_needKeyframe = true;
            continue;
        }
        const Plane& dst = m_bitstream[size_t(index)];
        if (p.data.size() > dst.length) {
            Warn("a frame is bigger than a bitstream buffer; it's cut off");
            p.data.resize(dst.length);
        }
        std::memcpy(dst.data, p.data.data(), p.data.size());
        v4l2_plane plane{};
        plane.bytesused = uint32_t(p.data.size());
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = uint32_t(index);
        buf.length = 1;
        buf.m.planes = &plane;
        buf.timestamp = Timestamp(p.frameIndex);
        {
            std::lock_guard lock(m_statsMutex);
            m_inputTimes[int64_t(p.frameIndex)] = rc::NowNs();
            while (m_inputTimes.size() > 64) m_inputTimes.erase(m_inputTimes.begin());
        }
        if (Ioctl(m_device, VIDIOC_QBUF, &buf) < 0) {
            Warn("the driver refused a frame");
            continue;
        }
        m_bitstreamFree[size_t(index)] = false;
        m_lastQueued = p.frameIndex;
    }
}

void VideoDecoder::OutputLoop() {
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), -10);  // best effort
    while (m_running) {
        pollfd pfd{m_device, short(POLLIN | POLLPRI), 0};
        if (poll(&pfd, 1, 10) <= 0) continue;
        if (pfd.revents & POLLPRI) {
            v4l2_event ev{};
            while (Ioctl(m_device, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !m_configured) {
                    if (!SetUpCapture()) m_failed = true;
                } else if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    // The PC starts a new stream (and a new decoder) for a new size, so this
                    // shouldn't happen mid-stream.
                    Warn("the stream changed size mid-stream; pictures may be wrong");
                }
            }
        }
        if (!(pfd.revents & POLLIN) || !m_configured) continue;
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = &plane;
        if (Ioctl(m_device, VIDIOC_DQBUF, &buf) < 0) continue;
        if (plane.bytesused == 0 || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
            if (buf.flags & V4L2_BUF_FLAG_ERROR) Warn("the driver returned a corrupt picture");
            QueuePicture(int(buf.index));
            continue;
        }
        const uint32_t frameIndex = FrameIndex(buf.timestamp);
        const int64_t now = rc::NowNs();
        {
            std::lock_guard lock(m_statsMutex);
            auto it = m_inputTimes.find(int64_t(frameIndex));
            if (it != m_inputTimes.end()) {
                m_decodeMsSum += rc::NsToMs(now - it->second);
                m_decodeMsMax = std::max(m_decodeMsMax, rc::NsToMs(now - it->second));
                m_decodeCount++;
                m_inputTimes.erase(it);
            }
        }
        {
            std::lock_guard lock(m_mutex);
            auto it = m_infos.find(int64_t(frameIndex));
            if (it != m_infos.end()) it->second.decodedNs = now;
        }
        {
            std::lock_guard lock(m_pictureMutex);
            m_pictures[buf.index].frameIndex = frameIndex;
            m_ready.push_back(int(buf.index));
        }
        if (!m_decoded++) RC_LOGI("Decoder %s %dx%d: first frame decoded", CodecName(m_codec), m_width, m_height);
    }
}

bool VideoDecoder::Latch(DecodedFrameInfo& out) {
    int index = -1;
    std::vector<int> skipped;
    // Everything read from the capture batch is copied under ONE lock.  SetUpCapture()
    // (output thread) may rebuild m_pictures and rewrite m_capture* concurrently; the old
    // code released the lock here then indexed m_pictures[m_capture*] unlocked ⇒ UAF/race.
    struct Latched {
        int fd = -1;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        uint32_t frameIndex = 0;
        uint64_t generation = 0;
        uint32_t width = 0, height = 0, stride = 0, uvOffset = 0;
        float crop[4] = {0, 0, 1, 1};
        GLuint texture = 0;
    } latched;
    {
        std::lock_guard lock(m_pictureMutex);
        // Latch runs on the GL thread: destroy images retired by a rebuild/failure here.
        DestroyRetiredImagesLocked();
        if (m_ready.empty()) return false;
        index = m_ready.back();  // the newest; older ones go straight back to the driver
        m_ready.pop_back();
        skipped.assign(m_ready.begin(), m_ready.end());
        m_ready.clear();
        if (size_t(index) >= m_pictures.size()) return false;  // a rebuild raced us
        Picture& pic = m_pictures[size_t(index)];
        // A successful rebuild replaces m_pictures but may leave stale m_ready indices;
        // such an entry belongs to the previous batch and must never be latched.
        if (pic.generation != m_batchGeneration) return false;
        latched.fd = pic.fd;
        latched.image = pic.image;
        latched.frameIndex = pic.frameIndex;
        latched.generation = pic.generation;
        latched.width = m_captureWidth;
        latched.height = m_captureHeight;
        latched.stride = m_captureStride;
        latched.uvOffset = m_captureUvOffset;
        std::memcpy(latched.crop, m_pendingCrop, sizeof(latched.crop));
        // m_texture is owned by the geometry mutex, not m_pictureMutex — take a snapshot.
        latched.texture = Geometry().texture;
    }
    {
        std::lock_guard lock(m_pictureMutex);
        // Hand back only indices that still belong to the live batch.  SetUpCapture()/fail()
        // may have re-REQBUFS'd the queue while we were outside the lock; an old index would
        // then mark an unrelated new buffer as filled (garbage picture, torn frame).
        if (latched.generation == m_batchGeneration) {
            for (int i : skipped) QueuePicture(i);
        }
    }

    if (latched.image == EGL_NO_IMAGE_KHR && latched.fd >= 0 && latched.width && latched.height) {
        const EGLint attribs[] = {
            EGL_WIDTH, EGLint(latched.width),
            EGL_HEIGHT, EGLint(latched.height),
            EGL_LINUX_DRM_FOURCC_EXT, kDrmFormatNv12,
            EGL_DMA_BUF_PLANE0_FD_EXT, latched.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(latched.stride),
            EGL_DMA_BUF_PLANE1_FD_EXT, latched.fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGLint(latched.uvOffset),
            EGL_DMA_BUF_PLANE1_PITCH_EXT, EGLint(latched.stride),
            // What the PC's encoders produce.
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_NONE};
        latched.image = pCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
        if (latched.image == EGL_NO_IMAGE_KHR) {
            Warn("EGL couldn't import a decoded picture (eglCreateImageKHR on a dmabuf)");
            std::lock_guard lock(m_pictureMutex);
            if (m_batchGeneration == latched.generation && size_t(index) < m_pictures.size())
                QueuePicture(index);
            return false;
        }
        // Cache the EGLImage on the batch entry under the same lock so it is imported once.
        // Match by BATCH GENERATION, not fd: POSIX reuses the lowest free fd after close(),
        // so a freshly exported dmabuf can collide with latched.fd from the old batch.  fd
        // equality alone would write an old-resolution EGLImage into a new-batch slot (ABA).
        std::lock_guard lock(m_pictureMutex);
        const bool sameBatch = size_t(index) < m_pictures.size() &&
                               m_batchGeneration == latched.generation &&
                               m_pictures[size_t(index)].generation == latched.generation &&
                               m_pictures[size_t(index)].fd == latched.fd;
        if (sameBatch) {
            m_pictures[size_t(index)].image = latched.image;
        } else {
            // Batch changed while we imported (or fd collided).  This image was just created
            // and never stored in m_pictures, so it is NOT in m_retiredImages — destroying it
            // here is the single owner's job.  Do NOT bind/publish a picture/geometry pair
            // that no longer matches current capture settings.
            const EGLDisplay display = eglGetCurrentDisplay();
            if (latched.image != EGL_NO_IMAGE_KHR && pDestroyImage && display != EGL_NO_DISPLAY)
                pDestroyImage(display, latched.image);
            return false;
        }
    }
    if (latched.image == EGL_NO_IMAGE_KHR) return false;
    // Bind, publish and advance m_shown under ONE m_pictureMutex hold.  SetUpCapture() and
    // fail() reset m_shown and bump m_batchGeneration under that same lock, so a rebuild
    // landing between the bind and the m_shown update would otherwise write an old-batch
    // index into the new batch: the next Latch would take a freshly filled buffer for the
    // retired one and hand it back to the driver while the GPU still samples it.
    {
        std::lock_guard lock(m_pictureMutex);
        if (latched.generation != m_batchGeneration) {
            // The queue was rebuilt/torn down while we imported.  The rebuild already pushed
            // THIS image into m_retiredImages (every image stored in m_pictures is retired
            // there — and reaching this line implies it was read from m_pictures[index] or
            // written into it above).  Destroying it here as well would free the same
            // EGLImageKHR twice, so hand ownership to the retire queue: drain it once and
            // publish nothing.
            DestroyRetiredImagesLocked();
            return false;
        }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, latched.texture);
        pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, latched.image);
        // Publish geometry only AFTER the EGLImage is bound: the publisher and the picture
        // then always describe the same frame (no "new geometry + old picture" window).
        PublishTextureGeometry(latched.texture, int(latched.width), int(latched.height), latched.crop);
        // The picture shown before this one is still being read by the frame in flight; the
        // one before that is free again.  fail() writes these under m_pictureMutex too.
        if (m_retiredPicture >= 0) QueuePicture(m_retiredPicture);
        m_retiredPicture = m_shown;
        m_shown = index;
    }

    const int64_t pts = int64_t(latched.frameIndex);
    std::lock_guard lock(m_mutex);
    auto it = m_infos.find(pts);
    if (it != m_infos.end()) {
        out = it->second;
        m_infos.erase(m_infos.begin(), std::next(it));
    } else {
        out = {};
        out.frameIndex = uint32_t(pts);
    }
    return true;
}

}  // namespace rv
