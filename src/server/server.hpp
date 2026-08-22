#pragma once

#include "../common/protocol.hpp"
#include "../common/socket_util.hpp"
#include "../common/audio_types.hpp"
#include "audio_capture.hpp"
#include "audio_endpoint_control.hpp"
#include "plugin_chain.hpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <string>

namespace audiorouter {

struct ServerConfig {
    uint16_t port = protocol::DEFAULT_PORT;
    std::string bind_ip = "0.0.0.0";
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint32_t frames_per_packet = 240; // 5ms at 48kHz (960 bytes PCM) -> single MTU
    bool auto_mute_pc_speaker = true;
    MuteMethod mute_method = MuteMethod::EndpointMute;
    uint32_t client_timeout_ms = 8000;
    bool use_test_tone = false;
    double test_tone_freq = 440.0;
    // Voice over USB: bind to loopback only and accept the client through an
    // "adb reverse tcp:PORT tcp:PORT" tunnel instead of the LAN. The tunnel
    // injects the phone's TCP traffic straight into the PC's loopback; the
    // server relays each UDP datagram as a length-prefixed frame over it.
    bool usb_mode = false;
    // VST3 audio effects: each path is loaded as a VST3 plugin and
    // applied in series to the captured audio before packetisation. An
    // empty list means no effects (passthrough). The on-wire format
    // stays S16LE; internally the audio is converted to float32 for the
    // plugins, then back to S16LE for transmission.
    std::vector<std::string> vst3_plugins;
};

enum class ServerState {
    STOPPED = 0,
    LISTENING,
    STREAMING
};

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

    bool start();
    void stop();
    bool is_running() const;

    ServerState get_state() const;
    ServerStats get_stats() const;
    SocketAddress get_active_client() const;

private:
    void network_receive_thread();
    void watchdog_thread();
    void on_audio_captured(const void* data, size_t num_frames, const AudioConfig& config);
    void usb_relay_thread();

    void handle_discovery_req(const protocol::CommonHeader& hdr, const SocketAddress& sender);
    void handle_connect_req(const protocol::CommonHeader& hdr, const uint8_t* payload, size_t len, const SocketAddress& sender);
    void handle_disconnect_req(const protocol::CommonHeader& hdr, const SocketAddress& sender);
    void handle_heartbeat_ping(const protocol::CommonHeader& hdr, const uint8_t* payload, size_t len, const SocketAddress& sender);
    void handle_control_cmd(const protocol::CommonHeader& hdr, const uint8_t* payload, size_t len, const SocketAddress& sender);

    void disconnect_client(const std::string& reason, bool send_ack = false);

    ServerConfig config_;
    std::atomic<bool> is_running_;
    std::atomic<ServerState> state_;

    UdpSocket socket_;
    std::unique_ptr<IAudioCapture> capture_engine_;
    std::unique_ptr<IPluginChain> plugin_chain_;
    AudioEndpointControl endpoint_control_;
    AudioConfig actual_audio_config_;

    // Voice over USB TCP relay (config_.usb_mode only): the adb server connects
    // TCP to 127.0.0.1:port; the relay translates length-prefixed frames on the
    // accepted connection into UDP datagrams to this engine's UDP socket (and
    // back). usb_listener_ accepts the tunnel's connections; the per-connection
    // socket lives inside the relay thread.
    std::thread usb_relay_thread_;
    TcpSocket usb_listener_;
    UdpSocket usb_relay_udp_;

    // Client Session State
    SocketAddress active_client_;
    std::atomic<uint64_t> last_client_activity_time_ms_;
    std::atomic<uint32_t> sequence_number_;

    // Stats
    ServerStats stats_;
    mutable std::mutex stats_mutex_;
    mutable std::mutex client_mutex_;

    // Threads
    std::thread net_thread_;
    std::thread watchdog_thread_;

    // Audio Packet Buffer Chunking
    std::vector<uint8_t> chunk_buffer_;
    std::mutex chunk_mutex_;
};

} // namespace audiorouter
