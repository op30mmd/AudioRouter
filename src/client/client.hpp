#pragma once

#include "../common/protocol.hpp"
#include "../common/socket_util.hpp"
#include "../common/audio_types.hpp"
#include "audio_player.hpp"
#include "jitter_buffer.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <memory>
#include "../common/span_compat.hpp"
#include "../common/expected_compat.hpp"
#include "../common/thread_compat.hpp"

namespace audiorouter {

struct ClientConfig {
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = protocol::DEFAULT_PORT;
    std::string device_name = "default";
    uint32_t target_latency_ms = 35;
    bool auto_discover = false;
    bool use_dummy_player = false;
    uint32_t reconnect_timeout_ms = 5000;

    [[nodiscard]] std::expected<void, std::string> validate() const noexcept {
        if (server_ip.empty()) return std::unexpected<std::string>(std::string("server_ip empty"));
        if (server_port == 0) return std::unexpected<std::string>(std::string("server_port 0"));
        if (target_latency_ms < 5 || target_latency_ms > 500) return std::unexpected<std::string>(std::string("target_latency_ms out of range"));
        return {};
    }
};

enum class ClientState : uint8_t {
    STOPPED = 0,
    DISCOVERING,
    CONNECTING,
    STREAMING,
    DISCONNECTED
};

struct ClientStats {
    uint64_t packets_received = 0;
    uint64_t bytes_received = 0;
    uint64_t frames_played = 0;
    uint32_t round_trip_time_us = 0;
    JitterBufferStats jitter_stats;
};

class AudioRouterClient {
public:
    explicit AudioRouterClient(const ClientConfig& config = ClientConfig{});
    ~AudioRouterClient();

    AudioRouterClient(const AudioRouterClient&) = delete;
    AudioRouterClient& operator=(const AudioRouterClient&) = delete;
    AudioRouterClient(AudioRouterClient&&) = delete;
    AudioRouterClient& operator=(AudioRouterClient&&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] ClientState get_state() const noexcept;
    [[nodiscard]] ClientStats get_stats() const;

    // Remote control — type-safe, bounds-checked
    [[nodiscard]] bool send_pc_mute_command(bool mute);
    [[nodiscard]] bool send_pc_volume_command(float volume_0_to_1);

private:
    [[nodiscard]] bool discover_server(SocketAddress& out_server_addr);
    [[nodiscard]] bool perform_handshake();

    // C++23 jthread workers — stop_token aware for cooperative cancellation
    void network_receive_thread(audiorouter::stop_token st);
    void audio_playback_thread(audiorouter::stop_token st);
    void heartbeat_thread(audiorouter::stop_token st);

    ClientConfig config_;
    std::atomic<bool> is_running_{false};
    std::atomic<ClientState> state_{ClientState::STOPPED};

    UdpSocket socket_;
    SocketAddress server_addr_;
    AudioConfig audio_config_{};

    std::unique_ptr<IAudioPlayer> player_;
    JitterBuffer jitter_buffer_;

    std::atomic<uint64_t> last_packet_time_ms_{0};
    std::atomic<uint32_t> last_rtt_us_{0};

    ClientStats stats_{};
    mutable std::mutex stats_mutex_;

    // C++23 cooperative threads — automatically joining, stop-aware (compat fallback to std::thread)
    audiorouter::jthread net_thread_;
    audiorouter::jthread playback_thread_;
    audiorouter::jthread heartbeat_thread_;
};

} // namespace audiorouter
