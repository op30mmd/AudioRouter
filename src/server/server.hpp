#pragma once

#include "../common/protocol.hpp"
#include "../common/socket_util.hpp"
#include "../common/audio_types.hpp"
#include "audio_capture.hpp"
#include "audio_endpoint_control.hpp"

#include <atomic>
#include <thread>
#include <stop_token>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include <expected>
#include <span>

namespace audiorouter {

struct ServerConfig {
    uint16_t port = protocol::DEFAULT_PORT;
    std::string bind_ip = "0.0.0.0";
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t frames_per_packet = 240;
    bool auto_mute_pc_speaker = true;
    MuteMethod mute_method = MuteMethod::EndpointMute;
    uint32_t client_timeout_ms = 3500;
    bool use_test_tone = false;
    double test_tone_freq = 440.0;

    [[nodiscard]] std::expected<void, std::string> validate() const noexcept {
        if (port == 0) return std::unexpected(std::string("port 0"));
        if (sample_rate < 8000 || sample_rate > 192000) return std::unexpected(std::string("sample_rate out of range"));
        if (channels == 0 || channels > 32) return std::unexpected(std::string("channels out of range"));
        if (frames_per_packet == 0 || frames_per_packet > 8192) return std::unexpected(std::string("frames_per_packet out of range"));
        if (client_timeout_ms < 500) return std::unexpected(std::string("client_timeout_ms too small"));
        return {};
    }
};

enum class ServerState : uint8_t { STOPPED = 0, LISTENING, STREAMING };

struct ServerStats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t audio_frames_captured = 0;
    uint32_t client_rtt_us = 0;
    uint32_t client_buffer_level = 0;
    uint32_t client_lost_packets = 0;
};

class AudioRouterServer {
public:
    explicit AudioRouterServer(const ServerConfig& config = ServerConfig{});
    ~AudioRouterServer();

    AudioRouterServer(const AudioRouterServer&) = delete;
    AudioRouterServer& operator=(const AudioRouterServer&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] ServerState get_state() const noexcept;
    [[nodiscard]] ServerStats get_stats() const;
    [[nodiscard]] SocketAddress get_active_client() const;

private:
    void network_receive_thread(std::stop_token st);
    void watchdog_thread(std::stop_token st);
    void on_audio_captured(const void* data, size_t num_frames, const AudioConfig& config);

    void handle_discovery_req(const protocol::CommonHeader& hdr, const SocketAddress& sender);
    void handle_connect_req(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender);
    void handle_disconnect_req(const protocol::CommonHeader& hdr, const SocketAddress& sender);
    void handle_heartbeat_ping(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender);
    void handle_control_cmd(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender);

    void disconnect_client(const std::string& reason, bool send_ack = false) noexcept;

    ServerConfig config_;
    std::atomic<bool> is_running_{false};
    std::atomic<ServerState> state_{ServerState::STOPPED};

    UdpSocket socket_;
    std::unique_ptr<IAudioCapture> capture_engine_;
    AudioEndpointControl endpoint_control_;
    AudioConfig actual_audio_config_{};

    SocketAddress active_client_;
    std::atomic<uint64_t> last_client_activity_time_ms_{0};
    std::atomic<uint32_t> sequence_number_{0};

    ServerStats stats_{};
    mutable std::mutex stats_mutex_;
    mutable std::mutex client_mutex_;

    std::jthread net_thread_;
    std::jthread watchdog_thread_;

    std::vector<uint8_t> chunk_buffer_;
    std::mutex chunk_mutex_;
};

} // namespace audiorouter
