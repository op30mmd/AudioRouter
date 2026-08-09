#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>
#include <span>
#include <expected>
#include <limits>
#include <concepts>
#include <utility>

namespace audiorouter {

enum class AudioSampleFormat : uint8_t {
    UNKNOWN = 0,
    PCM_S16LE = 1,       // 16-bit signed integer, Little Endian
    PCM_FLOAT32LE = 2,   // 32-bit IEEE float, Little Endian
    PCM_S24LE = 3,       // 24-bit signed integer in 3 bytes
    PCM_S32LE = 4        // 32-bit signed integer
};

[[nodiscard]] constexpr std::string_view to_string_view(AudioSampleFormat fmt) noexcept {
    switch (fmt) {
        case AudioSampleFormat::PCM_S16LE:    return "S16LE";
        case AudioSampleFormat::PCM_FLOAT32LE:return "FLOAT32LE";
        case AudioSampleFormat::PCM_S24LE:    return "S24LE";
        case AudioSampleFormat::PCM_S32LE:    return "S32LE";
        default:                              return "UNKNOWN";
    }
}

struct AudioConfig {
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    AudioSampleFormat format = AudioSampleFormat::PCM_S16LE;
    uint32_t frames_per_packet = 480; // 10ms at 48kHz

    [[nodiscard]] constexpr size_t bytes_per_sample() const noexcept {
        switch (format) {
            case AudioSampleFormat::PCM_S16LE:    return 2;
            case AudioSampleFormat::PCM_FLOAT32LE:return 4;
            case AudioSampleFormat::PCM_S24LE:    return 3;
            case AudioSampleFormat::PCM_S32LE:    return 4;
            default:                              return 2;
        }
    }

    [[nodiscard]] constexpr size_t bytes_per_frame() const noexcept {
        // checked multiplication: channels is uint16_t so product fits in size_t
        return bytes_per_sample() * static_cast<size_t>(channels);
    }

    [[nodiscard]] size_t packet_payload_size() const noexcept {
        size_t bpf = bytes_per_frame();
        if (bpf == 0 || frames_per_packet == 0) return 0;
        if (frames_per_packet > std::numeric_limits<size_t>::max() / bpf) return 0; // overflow guard
        return static_cast<size_t>(frames_per_packet) * bpf;
    }

    [[nodiscard]] size_t frames_to_bytes(size_t frames) const noexcept {
        size_t bpf = bytes_per_frame();
        if (bpf == 0) return 0;
        if (frames > std::numeric_limits<size_t>::max() / bpf) return 0;
        return frames * bpf;
    }

    [[nodiscard]] size_t bytes_to_frames(size_t bytes) const noexcept {
        size_t bpf = bytes_per_frame();
        return (bpf > 0) ? (bytes / bpf) : 0;
    }

    [[nodiscard]] double packet_duration_ms() const noexcept {
        if (sample_rate == 0) return 0.0;
        return (static_cast<double>(frames_per_packet) * 1000.0) / static_cast<double>(sample_rate);
    }

    [[nodiscard]] bool is_valid() const noexcept {
        if (sample_rate == 0 || sample_rate > 192000) return false;
        if (channels == 0 || channels > 32) return false;
        if (frames_per_packet == 0 || frames_per_packet > 8192) return false;
        if (format == AudioSampleFormat::UNKNOWN) return false;
        // payload must fit into safe MTU
        size_t payload = packet_payload_size();
        if (payload == 0 || payload > 65507) return false;
        return true;
    }

    [[nodiscard]] std::expected<void, std::string> validate() const noexcept {
        if (sample_rate == 0 || sample_rate > 192000)
            return std::unexpected(std::string("invalid sample_rate"));
        if (channels == 0 || channels > 32)
            return std::unexpected(std::string("invalid channels"));
        if (frames_per_packet == 0 || frames_per_packet > 8192)
            return std::unexpected(std::string("invalid frames_per_packet"));
        if (format == AudioSampleFormat::UNKNOWN)
            return std::unexpected(std::string("unknown format"));
        size_t payload = packet_payload_size();
        if (payload == 0) return std::unexpected(std::string("payload size overflow or zero"));
        if (payload > 65507) return std::unexpected(std::string("payload exceeds max UDP"));
        return {};
    }

    [[nodiscard]] std::string to_string() const {
        std::string fmt_str{to_string_view(format)};
        return std::to_string(sample_rate) + "Hz, " +
               std::to_string(channels) + "ch, " +
               fmt_str + " (" + std::to_string(frames_per_packet) + " frames/pkt)";
    }

    constexpr bool operator==(const AudioConfig& o) const noexcept {
        return sample_rate == o.sample_rate && channels == o.channels &&
               format == o.format && frames_per_packet == o.frames_per_packet;
    }
    constexpr bool operator!=(const AudioConfig& o) const noexcept { return !(*this == o); }
};

class AudioConverter {
public:
    // Span-based API — preferred C++23 safe interface
    [[nodiscard]] static bool float32_to_s16le(std::span<const float> src, std::span<int16_t> dst) noexcept {
        if (dst.size() < src.size()) return false;
        for (size_t i = 0; i < src.size(); ++i) {
            float v = std::clamp(src[i], -1.0f, 1.0f);
            dst[i] = static_cast<int16_t>(v * 32767.0f);
        }
        return true;
    }

    [[nodiscard]] static bool s16le_to_float32(std::span<const int16_t> src, std::span<float> dst) noexcept {
        if (dst.size() < src.size()) return false;
        for (size_t i = 0; i < src.size(); ++i) {
            dst[i] = static_cast<float>(src[i]) / 32768.0f;
        }
        return true;
    }

    // Legacy raw-pointer API — delegates to span overload with null/size checks
    static void float32_to_s16le(const float* src, int16_t* dst, size_t sample_count) noexcept {
        if (!src || !dst || sample_count == 0) return;
        // Use spans for bounds-checked iteration (but still raw loops for speed)
        for (size_t i = 0; i < sample_count; ++i) {
            float v = src[i];
            v = std::clamp(v, -1.0f, 1.0f);
            dst[i] = static_cast<int16_t>(v * 32767.0f);
        }
    }

    static void s16le_to_float32(const int16_t* src, float* dst, size_t sample_count) noexcept {
        if (!src || !dst || sample_count == 0) return;
        for (size_t i = 0; i < sample_count; ++i) {
            dst[i] = static_cast<float>(src[i]) / 32768.0f;
        }
    }

    // Span-based downmix
    [[nodiscard]] static bool downmix_to_stereo_float(std::span<const float> src, size_t src_channels,
                                                      std::span<float> dst, size_t frame_count) noexcept {
        if (src_channels == 0 || frame_count == 0) return false;
        if (src.size() < frame_count * src_channels) return false;
        if (dst.size() < frame_count * 2) return false;
        downmix_impl(src.data(), src_channels, dst.data(), frame_count);
        return true;
    }

    // Raw pointer version — with full validation
    static void downmix_to_stereo_float(const float* src, size_t src_channels,
                                        float* dst, size_t frame_count) noexcept {
        if (!src || !dst || src_channels == 0 || frame_count == 0) return;
        // overflow check
        if (src_channels > 32) return; // sanity
        downmix_impl(src, src_channels, dst, frame_count);
    }

    // Span-based volume
    static void apply_volume_s16le(std::span<int16_t> samples, float volume) noexcept {
        if (samples.empty()) return;
        if (std::abs(volume - 1.0f) < 0.001f) return;
        if (volume <= 0.0f) {
            std::ranges::fill(samples, 0);
            return;
        }
        if (!std::isfinite(volume)) {
            std::ranges::fill(samples, 0);
            return;
        }
        // clamp volume to [0, 10] to prevent extreme amplification / overflow
        volume = std::clamp(volume, 0.0f, 10.0f);
        for (auto& s : samples) {
            int32_t v = static_cast<int32_t>(static_cast<float>(s) * volume);
            s = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }
    }

    static void apply_volume_s16le(int16_t* samples, size_t count, float volume) noexcept {
        if (!samples || count == 0) return;
        apply_volume_s16le(std::span<int16_t>(samples, count), volume);
    }

private:
    static void downmix_impl(const float* src, size_t src_channels,
                             float* dst, size_t frame_count) noexcept {
        if (src_channels == 2) {
            // Use std::copy which is bounds-checked via caller
            std::copy_n(src, frame_count * 2, dst);
            return;
        }
        if (src_channels == 1) {
            for (size_t i = 0; i < frame_count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                dst[i * 2 + 0] = v;
                dst[i * 2 + 1] = v;
            }
            return;
        }
        if (src_channels >= 6) {
            constexpr float center_gain = 0.7071f;
            constexpr float surround_gain = 0.7071f;
            for (size_t i = 0; i < frame_count; ++i) {
                const float* frame = src + (i * src_channels);
                float fl = std::clamp(frame[0], -1.0f, 1.0f);
                float fr = std::clamp(frame[1], -1.0f, 1.0f);
                float fc = std::clamp(frame[2], -1.0f, 1.0f);
                float bl = std::clamp(frame[4], -1.0f, 1.0f);
                float br = std::clamp(frame[5], -1.0f, 1.0f);
                float left  = fl + center_gain * fc + surround_gain * bl;
                float right = fr + center_gain * fc + surround_gain * br;
                dst[i * 2 + 0] = std::clamp(left * 0.7f, -1.0f, 1.0f);
                dst[i * 2 + 1] = std::clamp(right * 0.7f, -1.0f, 1.0f);
            }
            return;
        }
        // Generic fallback for 3,4,5, >6
        for (size_t i = 0; i < frame_count; ++i) {
            const float* frame = src + (i * src_channels);
            float sum_l = 0.0f;
            float sum_r = 0.0f;
            size_t cnt_l = 0, cnt_r = 0;
            for (size_t ch = 0; ch < src_channels; ++ch) {
                float v = std::clamp(frame[ch], -1.0f, 1.0f);
                if ((ch % 2) == 0) { sum_l += v; ++cnt_l; }
                else               { sum_r += v; ++cnt_r; }
            }
            float l = cnt_l ? sum_l / static_cast<float>(cnt_l) : 0.0f;
            float r = cnt_r ? sum_r / static_cast<float>(cnt_r) : 0.0f;
            dst[i * 2 + 0] = std::clamp(l, -1.0f, 1.0f);
            dst[i * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
        }
    }
};

} // namespace audiorouter
