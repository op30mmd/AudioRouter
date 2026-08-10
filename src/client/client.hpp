#pragma once

#include "../common/protocol.hpp"
#include "../common/socket_util.hpp"
#include "../common/audio_types.hpp"
#include "audio_player.hpp"
#include "jitter_buffer.hpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <memory>

namespace audiorouter {

struct ClientConfig {
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = protocol::DEFAULT_PORT;
    std::string device_name = "default";
    uint32_t target_latency_ms = 35;
    bool auto_discover = false;
    bool use_dummy_player = false;
    uint32_t reconnect_timeout_ms = 5000;
};

enum class ClientState {
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

    bool start();
    void stop();
    bool is_running() const;

    ClientState get_state() const;
    ClientStats get_stats() const;

    // Send remote PC volume / mute control command to Windows server
    bool send_pc_mute_command(bool mute);
    bool send_pc_volume_command(float volume_0_to_1);

private:
    bool discover_server(SocketAddress& out_server_addr);
    bool perform_handshake();
    void network_receive_thread();
    void audio_playback_thread();
    void heartbeat_thread();
    void open_player_with_timeout(const std::string& device_name, uint32_t timeout_ms);
    void device_open_thread(const std::string& device_name);

    ClientConfig config_;
    std::atomic<bool> is_running_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<ClientState> state_;

    UdpSocket socket_;
    SocketAddress server_addr_;
    AudioConfig audio_config_;

    // Guards all socket send/receive/close calls (UdpSocket is not thread-safe)
    std::mutex socket_mutex_;

    // Guarded by player_open_mutex_: the playback thread reads it, the device
    // open thread hot-swaps it to the real device once it opens.
    std::shared_ptr<IAudioPlayer> player_;
    JitterBuffer jitter_buffer_;

    std::atomic<uint64_t> last_packet_time_ms_;
    std::atomic<uint32_t> last_rtt_us_;

    // Stats
    ClientStats stats_;
    mutable std::mutex stats_mutex_;

    // Background Threads
    std::thread net_thread_;
    std::thread playback_thread_;
    std::thread heartbeat_thread_;

    // Device-open retry thread. The ALSA/direct open can hang in a kernel
    // ioctl, so it runs here (never joined; detached on shutdown). It keeps
    // its own shared_ptr to the player, so it can hot-swap it into player_
    // when the device finally opens.
    std::thread device_thread_;

    // Bounded audio-device open coordination.
    std::mutex player_open_mutex_;
    std::condition_variable player_open_cv_;
    bool player_open_pending_ = false;
    bool player_open_result_ = false;
    std::atomic<bool> player_open_cancelled_{false};
};

} // namespace audiorouter
