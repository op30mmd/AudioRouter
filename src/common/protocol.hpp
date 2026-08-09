#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include "audio_types.hpp"

namespace audiorouter {
namespace protocol {

constexpr uint32_t MAGIC = 0x41554452; // "AUDR" in ASCII / hex
constexpr uint8_t CURRENT_VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 44100;
constexpr size_t MAX_UDP_PACKET_SIZE = 65507;
constexpr size_t SAFE_PAYLOAD_MTU = 1400; // Prevents Wi-Fi IP fragmentation

enum class MsgType : uint8_t {
    UNKNOWN = 0x00,
    DISCOVERY_REQ = 0x01,
    DISCOVERY_RESP = 0x02,
    CONNECT_REQ = 0x10,
    CONNECT_ACK = 0x11,
    CONNECT_NAK = 0x12,
    DISCONNECT_REQ = 0x20,
    DISCONNECT_ACK = 0x21,
    AUDIO_DATA = 0x30,
    HEARTBEAT_PING = 0x40,
    HEARTBEAT_PONG = 0x41,
    CONTROL_CMD = 0x50
};

enum PacketFlags : uint16_t {
    FLAG_NONE = 0x0000,
    FLAG_DISCONTINUITY = 0x0001, // Audio stream had a gap or restart
    FLAG_SILENCE = 0x0002,       // Packet represents silence
    FLAG_KEYFRAME = 0x0004,      // Format sync point
    FLAG_LAST_PACKET = 0x0008    // Stream ending
};

#pragma pack(push, 1)

struct CommonHeader {
    uint32_t magic;         // Must be MAGIC (0x41554452)
    uint8_t  version;       // Protocol version
    uint8_t  msg_type;      // MsgType
    uint16_t flags;         // PacketFlags
    uint32_t seq_num;       // Monotonically increasing sequence number
    uint64_t timestamp_us;  // Sender timestamp in microseconds
    uint32_t payload_size;  // Size of data following this header
};

struct DiscoveryReqPayload {
    char client_name[32];
    uint16_t client_version;
};

struct DiscoveryRespPayload {
    char server_name[32];
    uint16_t server_version;
    uint16_t server_port;
    uint8_t is_busy;        // 1 if server already streaming to a client
    uint8_t pc_muted;       // 1 if PC speaker currently muted
};

struct ConnectReqPayload {
    char client_name[32];
    uint32_t preferred_sample_rate; // e.g. 48000 or 44100 (0 = any)
    uint16_t preferred_channels;    // e.g. 2 (0 = any)
    uint8_t  preferred_format;      // AudioSampleFormat (0 = any)
    uint16_t target_latency_ms;     // e.g. 30ms
};

struct ConnectAckPayload {
    uint8_t  status_code;           // 0 = OK, >0 = Error
    uint32_t sample_rate;           // Negotiated sample rate
    uint16_t channels;              // Negotiated channels
    uint8_t  format;                // AudioSampleFormat
    uint16_t frames_per_packet;     // Audio frames per packet
    uint8_t  pc_speaker_muted;      // 1 if PC speaker muted successfully
    char     status_msg[64];
};

struct ConnectNakPayload {
    uint8_t  error_code;
    char     reason[64];
};

struct DisconnectPayload {
    uint8_t  reason_code;           // 0 = Normal, 1 = Timeout, 2 = Error
    char     reason[64];
};

struct AudioPacketHeader {
    CommonHeader common;
    uint32_t sample_rate;
    uint16_t channels;
    uint8_t  format;
    uint8_t  reserved;
    uint32_t num_frames;
};

struct HeartbeatPayload {
    uint64_t orig_timestamp_us;     // Echoed timestamp for RTT calculation
    uint32_t client_buffer_level_frames;
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t buffer_underruns;
    uint32_t buffer_overruns;
};

struct ControlCmdPayload {
    uint8_t  cmd_id;                // 1 = MUTE_PC, 2 = UNMUTE_PC, 3 = SET_VOLUME
    float    param_float;           // e.g. volume 0.0 - 1.0
    uint32_t param_int;
};

#pragma pack(pop)

inline bool is_valid_header(const CommonHeader& hdr, size_t received_bytes) {
    if (received_bytes < sizeof(CommonHeader)) {
        return false;
    }
    if (hdr.magic != MAGIC) {
        return false;
    }
    if (hdr.version != CURRENT_VERSION) {
        return false;
    }
    if (received_bytes < sizeof(CommonHeader) + hdr.payload_size) {
        return false;
    }
    return true;
}

inline const char* msg_type_to_string(MsgType type) {
    switch (type) {
        case MsgType::DISCOVERY_REQ: return "DISCOVERY_REQ";
        case MsgType::DISCOVERY_RESP: return "DISCOVERY_RESP";
        case MsgType::CONNECT_REQ: return "CONNECT_REQ";
        case MsgType::CONNECT_ACK: return "CONNECT_ACK";
        case MsgType::CONNECT_NAK: return "CONNECT_NAK";
        case MsgType::DISCONNECT_REQ: return "DISCONNECT_REQ";
        case MsgType::DISCONNECT_ACK: return "DISCONNECT_ACK";
        case MsgType::AUDIO_DATA: return "AUDIO_DATA";
        case MsgType::HEARTBEAT_PING: return "HEARTBEAT_PING";
        case MsgType::HEARTBEAT_PONG: return "HEARTBEAT_PONG";
        case MsgType::CONTROL_CMD: return "CONTROL_CMD";
        default: return "UNKNOWN";
    }
}

} // namespace protocol
} // namespace audiorouter
