#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>

namespace audiorouter {

enum class AudioSampleFormat : uint8_t {
    UNKNOWN = 0,
    PCM_S16LE = 1,      // 16-bit signed integer, Little Endian
    PCM_FLOAT32LE = 2,  // 32-bit IEEE float, Little Endian
    PCM_S24LE = 3,      // 24-bit signed integer in 3 bytes
    PCM_S32LE = 4       // 32-bit signed integer
};

struct AudioConfig {
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    AudioSampleFormat format = AudioSampleFormat::PCM_S16LE;
    uint32_t frames_per_packet = 480; // 10ms at 48kHz

    size_t bytes_per_sample() const {
        switch (format) {
            case AudioSampleFormat::PCM_S16LE: return 2;
            case AudioSampleFormat::PCM_FLOAT32LE: return 4;
            case AudioSampleFormat::PCM_S24LE: return 3;
            case AudioSampleFormat::PCM_S32LE: return 4;
            default: return 2;
        }
    }

    size_t bytes_per_frame() const {
        return bytes_per_sample() * channels;
    }

    size_t packet_payload_size() const {
        return frames_per_packet * bytes_per_frame();
    }

    size_t frames_to_bytes(size_t frames) const {
        return frames * bytes_per_frame();
    }

    size_t bytes_to_frames(size_t bytes) const {
        size_t bpf = bytes_per_frame();
        return (bpf > 0) ? (bytes / bpf) : 0;
    }

    double packet_duration_ms() const {
        if (sample_rate == 0) return 0.0;
        return (static_cast<double>(frames_per_packet) * 1000.0) / static_cast<double>(sample_rate);
    }

    std::string to_string() const {
        std::string fmt_str;
        switch (format) {
            case AudioSampleFormat::PCM_S16LE: fmt_str = "S16LE"; break;
            case AudioSampleFormat::PCM_FLOAT32LE: fmt_str = "FLOAT32LE"; break;
            case AudioSampleFormat::PCM_S24LE: fmt_str = "S24LE"; break;
            case AudioSampleFormat::PCM_S32LE: fmt_str = "S32LE"; break;
            default: fmt_str = "UNKNOWN"; break;
        }
        return std::to_string(sample_rate) + "Hz, " +
               std::to_string(channels) + "ch, " +
               fmt_str + " (" + std::to_string(frames_per_packet) + " frames/pkt)";
    }
};

class AudioConverter {
public:
    // Fast conversion from Float32 [-1.0f, +1.0f] to Int16 [-32768, 32767] with clamping
    static void float32_to_s16le(const float* src, int16_t* dst, size_t sample_count) {
        for (size_t i = 0; i < sample_count; ++i) {
            float val = src[i];
            if (val > 1.0f) val = 1.0f;
            else if (val < -1.0f) val = -1.0f;
            dst[i] = static_cast<int16_t>(val * 32767.0f);
        }
    }

    // Fast conversion from Int16 to Float32
    static void s16le_to_float32(const int16_t* src, float* dst, size_t sample_count) {
        for (size_t i = 0; i < sample_count; ++i) {
            dst[i] = static_cast<float>(src[i]) / 32768.0f;
        }
    }

    // Downmix multichannel float (e.g. 5.1, 7.1) to stereo 2.0 float
    static void downmix_to_stereo_float(const float* src, size_t src_channels,
                                        float* dst, size_t frame_count) {
        if (src_channels == 2) {
            std::copy(src, src + frame_count * 2, dst);
            return;
        }

        if (src_channels == 1) {
            for (size_t i = 0; i < frame_count; ++i) {
                dst[i * 2 + 0] = src[i];
                dst[i * 2 + 1] = src[i];
            }
            return;
        }

        // 5.1 (FL, FR, FC, LFE, BL, BR) -> Stereo (L, R)
        if (src_channels >= 6) {
            const float center_gain = 0.7071f;
            const float surround_gain = 0.7071f;
            for (size_t i = 0; i < frame_count; ++i) {
                const float* frame = src + (i * src_channels);
                float fl = frame[0];
                float fr = frame[1];
                float fc = frame[2];
                // LFE = frame[3] (omitted in standard stereo downmix)
                float bl = frame[4];
                float br = frame[5];

                float left = fl + center_gain * fc + surround_gain * bl;
                float right = fr + center_gain * fc + surround_gain * br;

                // Normalize/clamp to avoid clipping
                dst[i * 2 + 0] = std::clamp(left * 0.7f, -1.0f, 1.0f);
                dst[i * 2 + 1] = std::clamp(right * 0.7f, -1.0f, 1.0f);
            }
            return;
        }

        // Generic fallback for any channels > 2: average odd/even channels
        for (size_t i = 0; i < frame_count; ++i) {
            const float* frame = src + (i * src_channels);
            float sum_l = 0.0f;
            float sum_r = 0.0f;
            for (size_t ch = 0; ch < src_channels; ++ch) {
                if (ch % 2 == 0) sum_l += frame[ch];
                else sum_r += frame[ch];
            }
            dst[i * 2 + 0] = std::clamp(sum_l / static_cast<float>((src_channels + 1) / 2), -1.0f, 1.0f);
            dst[i * 2 + 1] = std::clamp(sum_r / static_cast<float>(src_channels / 2), -1.0f, 1.0f);
        }
    }

    // Apply digital gain / volume scaling to int16 samples
    static void apply_volume_s16le(int16_t* samples, size_t count, float volume) {
        if (std::abs(volume - 1.0f) < 0.001f) return;
        if (volume <= 0.0f) {
            std::fill(samples, samples + count, 0);
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            int32_t val = static_cast<int32_t>(static_cast<float>(samples[i]) * volume);
            samples[i] = static_cast<int16_t>(std::clamp(val, -32768, 32767));
        }
    }
};

} // namespace audiorouter
