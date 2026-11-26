// What both decoders share (MediaCodec on Android, V4L2 on Linux): the chain of frames handed
// to the hardware, the statistics and the log.
#include "video_decoder.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "rc/platform.h"

namespace rv {

bool IsValidDecodedCrop(uint32_t width, uint32_t height, int32_t left, int32_t top, int32_t right,
                        int32_t bottom) {
    return width > 0 && height > 0 && left >= 0 && top >= 0 && right > left && bottom > top &&
           uint32_t(right) <= width && uint32_t(bottom) <= height;
}

void VideoDecoder::PublishTextureGeometry(GLuint texture, int width, int height, const float crop[4]) {
    std::lock_guard lock(m_geometryMutex);
    m_texture = texture;
    m_textureWidth = width;
    m_textureHeight = height;
    std::memcpy(m_crop, crop, sizeof(m_crop));
}

void VideoDecoder::ResetTextureGeometry() {
    const float crop[4] = {0, 0, 0, 0};
    PublishTextureGeometry(0, 0, 0, crop);
}

const char* VideoDecoder::CodecName(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return "H.264";
        case rc::VideoCodec::HEVC: return "HEVC";
        case rc::VideoCodec::AV1: return "AV1";
    }
    return "HEVC";
}

// What can go wrong on every frame is logged at most once every 5 seconds per decoder and
// reason, with how often it happened since, so a missing picture shows up in the log without
// flooding it.
void VideoDecoder::Warn(const char* what, const char* detail) {
    const char* reason = what ? what : "";
    std::lock_guard lock(m_warnMutex);
    auto& [last, count] = m_warnSeen[reason];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    if (detail && *detail)
        RC_LOGW("Decoder %s %dx%d: %s (%u times) %s", CodecName(m_codec), m_width, m_height, reason, count, detail);
    else
        RC_LOGW("Decoder %s %dx%d: %s (%u times)", CodecName(m_codec), m_width, m_height, reason, count);
    last = now;
    count = 0;
}

void VideoDecoder::Fail(const char* what, const char* detail) {
    Warn(what, detail);
    m_failed = true;
    m_running = false;
    m_cv.notify_all();
}

void VideoDecoder::Submit(uint32_t frameIndex, bool keyframe, bool recovery, std::vector<uint8_t>&& bitstream,
                          std::vector<uint8_t>&& metadata, int64_t receivedNs) {
    std::lock_guard lock(m_mutex);
    if (m_hadKeyframe && !keyframe && frameIndex <= m_lastFrameIndex) return;  // stale duplicate / reordered
    if (bitstream.empty()) {
        Warn("empty bitstream rejected before queueing",
             rc::StrFormat("frame=%u keyframe=%d", frameIndex, int(keyframe)).c_str());
        m_queue.clear();
        m_seenKeyframe = false;
        m_lastFrameIndex = m_lastQueued;
        m_needKeyframe = true;
        return;
    }
    // Frames are predicted from the ones before. If any frame went missing (lost on
    // the network, or arrived too late), everything after it would decode into smeared
    // garbage, so keep showing the last good picture and ask the PC right away for a
    // frame to resume from: a keyframe, or a recovery frame that only references
    // frames up to LastGoodFrame().
    if (keyframe) {
        m_hadKeyframe = true;
        m_seenKeyframe = true;
    } else if (recovery && m_hadKeyframe) {
        if (!m_seenKeyframe) RC_LOGI("Resuming at recovery frame %u", frameIndex);
        m_seenKeyframe = true;
    } else if (m_seenKeyframe && frameIndex != m_lastFrameIndex + 1) {
        RC_LOGI("Frame %u missing (got %u), waiting for a frame to resume from", m_lastFrameIndex + 1, frameIndex);
        m_seenKeyframe = false;
    }
    if (!m_seenKeyframe) {
        Warn("frame skipped while waiting for a keyframe");
        m_needKeyframe = true;
        return;
    }
    // Never build a backlog: if the decoder falls behind, drop what is queued (the
    // chain then ends at the last frame MediaCodec got; a keyframe starts a new one).
    if (m_queue.size() > 6) {
        m_queue.clear();
        if (!keyframe) {
            m_lastFrameIndex = m_lastQueued;
            m_seenKeyframe = false;
            m_needKeyframe = true;
            return;
        }
    }
    m_lastFrameIndex = frameIndex;
    // Both VR and desktop frame metadata are at least this large. The decoder does not own the
    // stream id, so report the generic protocol floor rather than guessing a stream role.
    if (metadata.size() < sizeof(rc::DesktopFrameMetadata)) {
        Warn("metadata truncated", rc::StrFormat("stage=submit frame=%u metadata=%zu minimum=%zu bitstream=%zu",
                                                 frameIndex, metadata.size(), sizeof(rc::DesktopFrameMetadata),
                                                 bitstream.size())
                                     .c_str());
    }
    DecodedFrameInfo info;
    info.frameIndex = frameIndex;
    info.metadata = std::move(metadata);
    info.receivedNs = receivedNs;
    m_infos[int64_t(frameIndex)] = std::move(info);
    while (m_infos.size() > 64) m_infos.erase(m_infos.begin());
    m_queue.push_back({frameIndex, keyframe, std::move(bitstream), receivedNs});
    m_cv.notify_one();
}

uint32_t VideoDecoder::LastGoodFrame() {
    std::lock_guard lock(m_mutex);
    return m_hadKeyframe ? m_lastFrameIndex : 0;
}

double VideoDecoder::AverageDecodeMs() {
    std::lock_guard lock(m_statsMutex);
    const double avg = m_decodeCount ? m_decodeMsSum / m_decodeCount : 0;
    m_decodeMsSum = 0;
    m_decodeCount = 0;
    return avg;
}

}  // namespace rv
