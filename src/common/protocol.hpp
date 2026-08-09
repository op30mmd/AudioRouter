#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <span>
#include <expected>
#include <bit>
#include <concepts>
#include <array>
#include "audio_types.hpp"

namespace audiorouter {
namespace protocol {

constexpr uint32_t MAGIC = 0x41554452; // "AUDR"
constexpr uint8_t CURRENT_VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 44100;
constexpr size_t MAX_UDP_PACKET_SIZE = 65507;
constexpr size_t SAFE_PAYLOAD_MTU = 1400;

enum class MsgType : uint8_t {
    UNKNOWN        = 0x00,
    DISCOVERY_REQ  = 0x01,
    DISCOVERY_RESP = 0x02,
    CONNECT_REQ    = 0x10,
    CONNECT_ACK    = 0x11,
    CONNECT_NAK    = 0x12,
    DISCONNECT_REQ = 0x20,
    DISCONNECT_ACK = 0x21,
    AUDIO_DATA     = 0x30,
    HEARTBEAT_PING = 0x40,
    HEARTBEAT_PONG = 0x41,
    CONTROL_CMD    = 0x50
};

[[nodiscard]] constexpr bool is_valid_msg_type(uint8_t v) noexcept {
    switch (static_cast<MsgType>(v)) {
        case MsgType::DISCOVERY_REQ: case MsgType::DISCOVERY_RESP:
        case MsgType::CONNECT_REQ:   case MsgType::CONNECT_ACK: case MsgType::CONNECT_NAK:
        case MsgType::DISCONNECT_REQ:case MsgType::DISCONNECT_ACK:
        case MsgType::AUDIO_DATA:    case MsgType::HEARTBEAT_PING: case MsgType::HEARTBEAT_PONG:
        case MsgType::CONTROL_CMD:   return true;
        default: return false;
    }
}

enum PacketFlags : uint16_t {
    FLAG_NONE          = 0x0000,
    FLAG_DISCONTINUITY = 0x0001,
    FLAG_SILENCE       = 0x0002,
    FLAG_KEYFRAME      = 0x0004,
    FLAG_LAST_PACKET   = 0x0008
};

#pragma pack(push, 1)
struct CommonHeader {
    uint32_t magic;         // MAGIC
    uint8_t  version;
    uint8_t  msg_type;      // MsgType as uint8_t
    uint16_t flags;
    uint32_t seq_num;
    uint64_t timestamp_us;
    uint32_t payload_size;
};
struct DiscoveryReqPayload { char client_name[32]; uint16_t client_version; };
struct DiscoveryRespPayload { char server_name[32]; uint16_t server_version; uint16_t server_port; uint8_t is_busy; uint8_t pc_muted; };
struct ConnectReqPayload { char client_name[32]; uint32_t preferred_sample_rate; uint16_t preferred_channels; uint8_t preferred_format; uint16_t target_latency_ms; };
struct ConnectAckPayload { uint8_t status_code; uint32_t sample_rate; uint16_t channels; uint8_t format; uint16_t frames_per_packet; uint8_t pc_speaker_muted; char status_msg[64]; };
struct ConnectNakPayload { uint8_t error_code; char reason[64]; };
struct DisconnectPayload { uint8_t reason_code; char reason[64]; };
struct AudioPacketHeader { CommonHeader common; uint32_t sample_rate; uint16_t channels; uint8_t format; uint8_t reserved; uint32_t num_frames; };
struct HeartbeatPayload { uint64_t orig_timestamp_us; uint32_t client_buffer_level_frames; uint32_t packets_received; uint32_t packets_lost; uint32_t buffer_underruns; uint32_t buffer_overruns; };
struct ControlCmdPayload { uint8_t cmd_id; float param_float; uint32_t param_int; };
#pragma pack(pop)

static_assert(sizeof(CommonHeader) == 24, "CommonHeader must be 24 bytes packed");
static_assert(sizeof(AudioPacketHeader) == 36, "AudioPacketHeader must be 36 bytes packed");
static_assert(std::is_standard_layout_v<CommonHeader> && std::is_trivially_copyable_v<CommonHeader>);
static_assert(std::is_standard_layout_v<AudioPacketHeader>);

// String helpers — constexpr
[[nodiscard]] constexpr std::string_view msg_type_to_string(MsgType type) noexcept {
    switch (type) {
        case MsgType::DISCOVERY_REQ:  return "DISCOVERY_REQ";
        case MsgType::DISCOVERY_RESP: return "DISCOVERY_RESP";
        case MsgType::CONNECT_REQ:    return "CONNECT_REQ";
        case MsgType::CONNECT_ACK:    return "CONNECT_ACK";
        case MsgType::CONNECT_NAK:    return "CONNECT_NAK";
        case MsgType::DISCONNECT_REQ: return "DISCONNECT_REQ";
        case MsgType::DISCONNECT_ACK: return "DISCONNECT_ACK";
        case MsgType::AUDIO_DATA:     return "AUDIO_DATA";
        case MsgType::HEARTBEAT_PING: return "HEARTBEAT_PING";
        case MsgType::HEARTBEAT_PONG: return "HEARTBEAT_PONG";
        case MsgType::CONTROL_CMD:    return "CONTROL_CMD";
        default:                      return "UNKNOWN";
    }
}
inline const char* msg_type_to_cstr(MsgType t) noexcept { return msg_type_to_string(t).data(); }

// Validation — utmost strict
[[nodiscard]] inline bool is_valid_header(const CommonHeader& hdr, size_t received_bytes) noexcept {
    if (received_bytes < sizeof(CommonHeader)) return false;
    if (hdr.magic != MAGIC) return false;
    if (hdr.version != CURRENT_VERSION) return false;
    if (!is_valid_msg_type(hdr.msg_type)) return false;
    // Prevent integer overflow: hdr.payload_size could be up to 2^32
    if (hdr.payload_size > MAX_UDP_PACKET_SIZE) return false;
    if (hdr.payload_size > SAFE_PAYLOAD_MTU * 8) {
        // Allow up to MAX but flag huge; still check bounds
    }
    // Check total size does not overflow size_t
    size_t total = sizeof(CommonHeader) + static_cast<size_t>(hdr.payload_size);
    if (total < sizeof(CommonHeader)) return false; // overflow
    if (received_bytes < total) return false;
    // Flags must be subset of known bits
    constexpr uint16_t known = FLAG_DISCONTINUITY | FLAG_SILENCE | FLAG_KEYFRAME | FLAG_LAST_PACKET;
    if ((hdr.flags & ~known) != 0) return false;
    return true;
}

[[nodiscard]] inline std::expected<void, std::string> validate_header(const CommonHeader& hdr, size_t received_bytes) noexcept {
    if (received_bytes < sizeof(CommonHeader)) return std::unexpected(std::string("packet too small for header"));
    if (hdr.magic != MAGIC) return std::unexpected(std::string("bad magic"));
    if (hdr.version != CURRENT_VERSION) return std::unexpected(std::string("unsupported version"));
    if (!is_valid_msg_type(hdr.msg_type)) return std::unexpected(std::string("unknown msg_type"));
    if (hdr.payload_size > MAX_UDP_PACKET_SIZE) return std::unexpected(std::string("payload too large"));
    size_t total = sizeof(CommonHeader) + static_cast<size_t>(hdr.payload_size);
    if (received_bytes < total) return std::unexpected(std::string("truncated payload"));
    constexpr uint16_t known = FLAG_DISCONTINUITY | FLAG_SILENCE | FLAG_KEYFRAME | FLAG_LAST_PACKET;
    if ((hdr.flags & ~known) != 0) return std::unexpected(std::string("unknown flags set"));
    return {};
}

// Safe span-based header parsing — zero-copy view with bounds check
[[nodiscard]] inline std::expected<CommonHeader, std::string> parse_header(std::span<const std::byte> buf) noexcept {
    if (buf.size() < sizeof(CommonHeader)) return std::unexpected(std::string("buffer too small"));
    CommonHeader hdr;
    std::memcpy(&hdr, buf.data(), sizeof(CommonHeader));
    auto v = validate_header(hdr, buf.size());
    if (!v) return std::unexpected(v.error());
    return hdr;
}

[[nodiscard]] inline std::expected<std::span<const std::byte>, std::string>
payload_span(const CommonHeader& hdr, std::span<const std::byte> buf) noexcept {
    size_t total = sizeof(CommonHeader) + static_cast<size_t>(hdr.payload_size);
    if (buf.size() < total) return std::unexpected(std::string("buffer smaller than header+payload"));
    return buf.subspan(sizeof(CommonHeader), hdr.payload_size);
}

// Audio header validation
[[nodiscard]] inline std::expected<void, std::string> validate_audio_header(const AudioPacketHeader& ah, size_t received_bytes) noexcept {
    auto v = validate_header(ah.common, received_bytes);
    if (!v) return v;
    if (ah.common.msg_type != static_cast<uint8_t>(MsgType::AUDIO_DATA)) return std::unexpected(std::string("not AUDIO_DATA"));
    if (ah.sample_rate == 0 || ah.sample_rate > 192000) return std::unexpected(std::string("invalid sample_rate"));
    if (ah.channels == 0 || ah.channels > 32) return std::unexpected(std::string("invalid channels"));
    if (ah.num_frames == 0 || ah.num_frames > 8192) return std::unexpected(std::string("invalid num_frames"));
    // Check payload size matches header + PCM
    size_t expected_pcm = static_cast<size_t>(ah.num_frames) * static_cast<size_t>(ah.channels) * 2; // S16LE
    size_t header_only = sizeof(AudioPacketHeader) - sizeof(CommonHeader);
    if (ah.common.payload_size != header_only + expected_pcm) {
        // Allow other formats (float=4 bytes) - compute via format if known
        // For now require exact match for S16LE; other formats unchecked beyond bounds
        if (ah.format == static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE) && ah.common.payload_size != header_only + expected_pcm)
            return std::unexpected(std::string("payload size mismatches num_frames"));
    }
    return {};
}

// Factory helpers — type-safe construction
[[nodiscard]] inline CommonHeader make_header(MsgType type, uint32_t seq, uint64_t ts_us, uint32_t payload_sz, uint16_t flags = FLAG_NONE) noexcept {
    CommonHeader h{};
    h.magic = MAGIC;
    h.version = CURRENT_VERSION;
    h.msg_type = static_cast<uint8_t>(type);
    h.flags = flags;
    h.seq_num = seq;
    h.timestamp_us = ts_us;
    h.payload_size = payload_sz;
    return h;
}

} // namespace protocol
} // namespace audiorouter
