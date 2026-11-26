#include "rc/audio_codec.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "opus.h"
#include "rc/platform.h"

namespace rc {

AudioEncoder::~AudioEncoder() {
    if (m_enc) opus_encoder_destroy(m_enc);
}

bool AudioEncoder::Init(int sampleRate, int channels, int bitrate, bool lowDelay) {
    int err = 0;
    m_channels = channels;
    m_enc = opus_encoder_create(sampleRate, channels, lowDelay ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_AUDIO,
                                &err);
    if (err != OPUS_OK || !m_enc) {
        RC_LOGE("opus_encoder_create failed: %d", err);
        return false;
    }
    opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(m_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_enc, OPUS_SET_PACKET_LOSS_PERC(5));
    return true;
}

int AudioEncoder::Encode(const int16_t* pcm, int frameSamples, uint8_t* out, int capacity) {
    if (!m_enc || !pcm || !out || frameSamples <= 0 || capacity <= 0) return -1;
    return opus_encode(m_enc, pcm, frameSamples, out, capacity);
}

AudioDecoder::~AudioDecoder() {
    if (m_dec) opus_decoder_destroy(m_dec);
}

bool AudioDecoder::Init(int sampleRate, int channels) {
    int err = 0;
    // Again with the same format: start over without reallocating.
    opus_int32 rate = 0;
    if (m_dec && channels == m_channels && opus_decoder_ctl(m_dec, OPUS_GET_SAMPLE_RATE(&rate)) == OPUS_OK && rate == sampleRate)
        return opus_decoder_ctl(m_dec, OPUS_RESET_STATE) == OPUS_OK;
    if (m_dec) opus_decoder_destroy(m_dec);
    m_channels = channels;
    m_dec = opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || !m_dec) {
        RC_LOGE("opus_decoder_create failed: %d", err);
        return false;
    }
    return true;
}

int AudioDecoder::Decode(const uint8_t* data, int size, int16_t* pcm, int maxFrameSamples) {
    if (!m_dec || !pcm || maxFrameSamples <= 0) return -1;
    // Opus max frame size at 48 kHz is 120 ms (5760 samples/ch); PLC loops up to frame_size
    // unconditionally in libopus, so an unchecked wire value would overflow fixed PCM buffers.
    const int safeSamples = std::min(maxFrameSamples, 5760);
    return opus_decode(m_dec, data, size, pcm, safeSamples, 0);
}

void JitterBuffer::Configure(int channels, int sampleRate, int targetMs, int maxMs) {
    std::lock_guard lock(m_mutex);
    if (channels <= 0 || channels > 8 || sampleRate <= 0 || maxMs <= 0) {
        m_data.clear();
        m_read = 0;
        m_size = 0;
        m_priming = true;
        m_started = false;
        m_target = 0;
        m_minTarget = 0;
        m_max = 0;
        m_channels = 2;
        m_sampleRate = 48000;
        m_calmSinceNs = NowNs();
        return;
    }
    const int clampedTargetMs = std::clamp(targetMs, 0, maxMs);
    const int64_t maxFrames = int64_t(sampleRate) * int64_t(maxMs) / 1000;
    const int64_t targetFrames = int64_t(sampleRate) * int64_t(clampedTargetMs) / 1000;
    m_channels = channels;
    m_sampleRate = sampleRate;
    m_max = int(std::clamp<int64_t>(maxFrames, 1, std::numeric_limits<int>::max() / 2));
    m_target = int(std::clamp<int64_t>(targetFrames, 0, m_max));
    m_minTarget = m_target;
    m_started = false;
    m_calmSinceNs = NowNs();
    m_data.assign(size_t(m_max) * 2 * size_t(m_channels), 0);
    m_read = 0;
    m_size = 0;
    m_priming = true;
}

void JitterBuffer::Clear() {
    std::lock_guard lock(m_mutex);
    m_read = 0;
    m_size = 0;
    m_priming = true;
    m_started = false;
}

int JitterBuffer::TargetMs() {
    std::lock_guard lock(m_mutex);
    return m_sampleRate > 0 ? int(int64_t(m_target) * 1000 / m_sampleRate) : 0;
}

int JitterBuffer::BufferedFrames() {
    std::lock_guard lock(m_mutex);
    return int(m_size);
}

void JitterBuffer::Push(const int16_t* pcm, int frames) {
    std::lock_guard lock(m_mutex);
    if (!pcm || frames <= 0 || m_channels <= 0 || m_data.empty()) return;
    const size_t ch = size_t(m_channels);
    const size_t capacity = m_data.size() / ch;
    if (capacity == 0) return;
    const size_t nFrames = size_t(frames);
    if (nFrames >= capacity) {
        m_read = 0;
        m_size = 0;
        std::memcpy(m_data.data(), pcm + (nFrames - capacity) * ch, capacity * ch * sizeof(int16_t));
        m_size = capacity;
        if (m_priming && m_size >= size_t(m_target)) m_priming = false;
        return;
    }
    // Overflow: drop the oldest audio to bound latency.
    if (m_size + nFrames > size_t(m_max)) {
        const size_t targetDrop =
            (m_size + nFrames > size_t(m_target)) ? (m_size + nFrames - size_t(m_target)) : size_t(0);
        const size_t capDrop =
            (m_size + nFrames > capacity) ? (m_size + nFrames - capacity) : size_t(0);
        const size_t drop = std::max(targetDrop, capDrop);
        const size_t d = std::min(drop, m_size);
        m_read = (m_read + d) % capacity;
        m_size -= d;
    }
    size_t write = (m_read + m_size) % capacity;
    for (size_t i = 0; i < nFrames; i++) {
        std::memcpy(&m_data[write * ch], pcm + i * ch, sizeof(int16_t) * ch);
        write = (write + 1) % capacity;
    }
    m_size += nFrames;
    if (m_priming && m_size >= size_t(m_target)) m_priming = false;
}

int JitterBuffer::Pop(int16_t* out, int frames) {
    std::lock_guard lock(m_mutex);
    if (!out || frames <= 0 || m_channels <= 0) return 0;
    const size_t ch = size_t(m_channels);
    const size_t nFrames = size_t(frames);
    if (m_target > m_minTarget && NowNs() - m_calmSinceNs > 15'000'000'000LL) {
        m_target = std::max(m_minTarget, m_target - m_sampleRate / 100);
        m_calmSinceNs = NowNs();
    }
    const size_t capacity = m_data.size() / ch;
    if (m_data.empty() || capacity == 0 || m_priming) {
        std::memset(out, 0, sizeof(int16_t) * nFrames * ch);
        return 0;
    }
    // Drift correction: when well above target, consume one extra frame per
    // ~100 output frames; when below, repeat one. Inaudible at 1%.
    int real = 0;
    for (size_t i = 0; i < nFrames; i++) {
        if (m_size == 0) {
            std::memset(out + i * ch, 0, sizeof(int16_t) * (nFrames - i) * ch);
            m_priming = true;
            // Audio arrived too late for this buffer: hold more from now on.
            if (m_started) {
                m_target = std::min(m_target + m_sampleRate / 50, m_max / 2);
                m_calmSinceNs = NowNs();
            }
            break;
        }
        m_started = true;
        std::memcpy(out + i * ch, &m_data[m_read * ch], sizeof(int16_t) * ch);
        real++;
        bool advance = true;
        if (i % 100 == 50) {
            if (m_size > size_t(m_target) * 3 / 2 && m_size > 1) {
                m_read = (m_read + 1) % capacity;  // skip one
                m_size--;
            } else if (m_size < size_t(m_target) / 2) {
                advance = false;  // hold one
            }
        }
        if (advance) {
            m_read = (m_read + 1) % capacity;
            m_size--;
        }
    }
    return real;
}

}  // namespace rc
