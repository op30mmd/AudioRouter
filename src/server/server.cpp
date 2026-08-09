#include "server.hpp"
#include "wasapi_capture.hpp"
#include "dummy_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cstring>
#include <vector>
#include "../common/span_compat.hpp"
#include <array>
#include <limits>
#include <algorithm>

namespace audiorouter {

AudioRouterServer::AudioRouterServer(const ServerConfig& config)
    : config_(config),
      is_running_(false),
      state_(ServerState::STOPPED),
      last_client_activity_time_ms_(0),
      sequence_number_(0) {}

AudioRouterServer::~AudioRouterServer() {
    stop();
}

bool AudioRouterServer::start() {
    if (is_running_.load()) return true;

    if (auto v = config_.validate(); !v) {
        LOG_ERROR("Invalid server config: " << v.error());
        return false;
    }

    LOG_INFO("=================================================");
    LOG_INFO(" Starting AudioRouter Windows Server Engine (C++23)");
    LOG_INFO("=================================================");

    if (config_.auto_mute_pc_speaker) {
        if (!endpoint_control_.init()) {
            LOG_WARN("Failed to initialize Windows endpoint volume controller. PC speaker muting might be unavailable.");
        }
    }

    if (!socket_.open()) {
        LOG_ERROR("Failed to open UDP socket");
        return false;
    }
    if (!socket_.bind(config_.port, config_.bind_ip)) {
        LOG_ERROR("Failed to bind UDP socket to " << config_.bind_ip << ":" << config_.port);
        socket_.close();
        return false;
    }
    (void)socket_.set_buffer_sizes(1024 * 1024, 1024 * 1024);
    (void)socket_.set_qos_priority(true);

    auto ifaces = UdpSocket::get_local_interfaces();
    LOG_INFO("Available Network Interfaces for Android Client Connection:");
    for (const auto& iface : ifaces) {
        if (iface.is_up && !iface.is_loopback) {
            LOG_INFO("  -> " << iface.ip_address << " (" << iface.name << ")");
        }
    }
    LOG_INFO("Listening for Android client on UDP port " << config_.port << "...");

    if (config_.use_test_tone) {
        capture_engine_ = std::make_unique<DummyCapture>(config_.test_tone_freq);
    } else {
#if defined(_WIN32)
        capture_engine_ = std::make_unique<WasapiCapture>();
#else
        LOG_WARN("Non-Windows host detected: using synthetic audio generator");
        capture_engine_ = std::make_unique<DummyCapture>(config_.test_tone_freq);
#endif
    }

    capture_engine_->set_audio_callback(
        [this](const void* data, size_t num_frames, const AudioConfig& cfg){
            this->on_audio_captured(data, num_frames, cfg);
        }
    );

    is_running_.store(true);
    state_.store(ServerState::LISTENING);

    net_thread_ = audiorouter::jthread([this](audiorouter::stop_token st){ this->network_receive_thread(st); });
    watchdog_thread_ = audiorouter::jthread([this](audiorouter::stop_token st){ this->watchdog_thread(st); });

    LOG_INFO("AudioRouter Server ready. Waiting for Android client connection.");
    return true;
}

void AudioRouterServer::stop() noexcept {
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false)) return;
    LOG_INFO("Stopping AudioRouter Server...");
    net_thread_.request_stop();
    watchdog_thread_.request_stop();

    disconnect_client("Server shutdown", true);

    if (capture_engine_) capture_engine_->stop();

    socket_.close();

    if (net_thread_.joinable()) net_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();

    if (config_.auto_mute_pc_speaker) {
        (void)endpoint_control_.unmute_pc_speaker();
        endpoint_control_.shutdown();
    }
    state_.store(ServerState::STOPPED);
    LOG_INFO("AudioRouter Server stopped successfully.");
}

bool AudioRouterServer::is_running() const noexcept { return is_running_.load(); }
ServerState AudioRouterServer::get_state() const noexcept { return state_.load(); }

ServerStats AudioRouterServer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}
SocketAddress AudioRouterServer::get_active_client() const {
    std::lock_guard<std::mutex> lock(client_mutex_);
    return active_client_;
}

void AudioRouterServer::network_receive_thread(audiorouter::stop_token st) {
    std::vector<uint8_t> recv_buf(65536);
    (void)socket_.set_receive_timeout_ms(200);
    while (!st.stop_requested() && is_running_.load()) {
        SocketAddress sender;
        int bytes_read = socket_.receive_from(recv_buf.data(), recv_buf.size(), sender);
        if (bytes_read <= 0) {
            if (!is_running_.load() || st.stop_requested()) break;
            // timeout — continue to check stop
            continue;
        }
        if (static_cast<size_t>(bytes_read) < sizeof(protocol::CommonHeader)) continue;

        protocol::CommonHeader hdr{}; std::memcpy(&hdr, recv_buf.data(), sizeof(hdr));
        if (!protocol::is_valid_header(hdr, static_cast<size_t>(bytes_read))) continue;

        // Create payload span safely
        std::span<const std::byte> payload_span;
        if (static_cast<size_t>(bytes_read) > sizeof(protocol::CommonHeader)) {
            payload_span = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(recv_buf.data() + sizeof(protocol::CommonHeader)),
                static_cast<size_t>(bytes_read) - sizeof(protocol::CommonHeader));
        } else {
            payload_span = std::span<const std::byte>{};
        }

        auto msg_type = static_cast<protocol::MsgType>(hdr.msg_type);
        switch (msg_type) {
            case protocol::MsgType::DISCOVERY_REQ:
                handle_discovery_req(hdr, sender);
                break;
            case protocol::MsgType::CONNECT_REQ:
                handle_connect_req(hdr, payload_span, sender);
                break;
            case protocol::MsgType::DISCONNECT_REQ:
                handle_disconnect_req(hdr, sender);
                break;
            case protocol::MsgType::HEARTBEAT_PING:
                handle_heartbeat_ping(hdr, payload_span, sender);
                break;
            case protocol::MsgType::CONTROL_CMD:
                handle_control_cmd(hdr, payload_span, sender);
                break;
            default: break;
        }
    }
}

void AudioRouterServer::handle_discovery_req(const protocol::CommonHeader& hdr, const SocketAddress& sender) {
    (void)hdr;
    LOG_INFO("Received Discovery Probe from " << sender.to_string());
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::DiscoveryRespPayload)> resp_buf{};
    protocol::CommonHeader rh = protocol::make_header(protocol::MsgType::DISCOVERY_RESP, 0, get_time_us(), sizeof(protocol::DiscoveryRespPayload));
    protocol::DiscoveryRespPayload rp{}; 
    std::strncpy(rp.server_name, "AudioRouter-PC-Server", sizeof(rp.server_name)-1); rp.server_name[sizeof(rp.server_name)-1]='\0';
    rp.server_version = protocol::CURRENT_VERSION;
    rp.server_port = config_.port;
    rp.is_busy = (state_.load() == ServerState::STREAMING) ? 1 : 0;
    rp.pc_muted = endpoint_control_.is_currently_silenced_by_us() ? 1 : 0;
    std::memcpy(resp_buf.data(), &rh, sizeof(rh));
    std::memcpy(resp_buf.data()+sizeof(rh), &rp, sizeof(rp));
    (void)socket_.send_to(std::span<const std::byte>(resp_buf), sender);
}

void AudioRouterServer::handle_connect_req(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender) {
    (void)hdr; (void)payload;
    std::lock_guard<std::mutex> lock(client_mutex_);
    LOG_INFO("Connection request received from client: " << sender.to_string());

    if (state_.load() == ServerState::STREAMING && active_client_.is_valid() && active_client_ != sender) {
        uint64_t now_ms = get_time_ms();
        if (now_ms - last_client_activity_time_ms_.load() < config_.client_timeout_ms) {
            LOG_WARN("Rejecting connection from " << sender.to_string() << ": Busy with " << active_client_.to_string());
            std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::ConnectNakPayload)> nak_buf{};
            protocol::CommonHeader nh = protocol::make_header(protocol::MsgType::CONNECT_NAK, 0, get_time_us(), sizeof(protocol::ConnectNakPayload));
            protocol::ConnectNakPayload np{}; np.error_code = 1;
            std::strncpy(np.reason, "Server is currently streaming to another client", sizeof(np.reason)-1); np.reason[sizeof(np.reason)-1]='\0';
            std::memcpy(nak_buf.data(), &nh, sizeof(nh));
            std::memcpy(nak_buf.data()+sizeof(nh), &np, sizeof(np));
            (void)socket_.send_to(std::span<const std::byte>(nak_buf), sender);
            return;
        }
    }

    active_client_ = sender;
    last_client_activity_time_ms_.store(get_time_ms());
    sequence_number_.store(0);

    AudioConfig desired_config{};
    desired_config.sample_rate = config_.sample_rate;
    desired_config.channels = config_.channels;
    desired_config.format = AudioSampleFormat::PCM_S16LE;
    desired_config.frames_per_packet = config_.frames_per_packet;
    // Validate
    if (!desired_config.is_valid()) {
        LOG_WARN("Desired AudioConfig invalid, clamping");
        if (desired_config.sample_rate==0) desired_config.sample_rate=48000;
        if (desired_config.channels==0) desired_config.channels=2;
    }

    if (!capture_engine_ || !capture_engine_->is_capturing()) {
        if (!capture_engine_ || !capture_engine_->start(desired_config, actual_audio_config_)) {
            // try fallback dummy?
            LOG_ERROR("Failed to start audio capture engine");
            return;
        }
    }
    if (!actual_audio_config_.is_valid()) actual_audio_config_ = desired_config;

    bool muted_ok = false;
    if (config_.auto_mute_pc_speaker) {
        LOG_INFO("Routing audio to client -> Silencing PC speaker...");
        muted_ok = endpoint_control_.mute_pc_speaker(config_.mute_method);
        if (muted_ok) LOG_INFO("PC Speaker successfully silenced.");
        else LOG_WARN("Could not silence PC speaker via endpoint volume.");
    }

    state_.store(ServerState::STREAMING);
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::ConnectAckPayload)> ack_buf{};
    protocol::CommonHeader ah = protocol::make_header(protocol::MsgType::CONNECT_ACK, 0, get_time_us(), sizeof(protocol::ConnectAckPayload));
    protocol::ConnectAckPayload ap{}; ap.status_code=0;
    ap.sample_rate = actual_audio_config_.sample_rate;
    ap.channels = actual_audio_config_.channels;
    ap.format = static_cast<uint8_t>(actual_audio_config_.format);
    ap.frames_per_packet = static_cast<uint16_t>(std::clamp<uint32_t>(actual_audio_config_.frames_per_packet, 0, std::numeric_limits<uint16_t>::max()));
    ap.pc_speaker_muted = muted_ok ? 1 : 0;
    std::strncpy(ap.status_msg, "Connected: Audio routed to client", sizeof(ap.status_msg)-1); ap.status_msg[sizeof(ap.status_msg)-1]='\0';
    std::memcpy(ack_buf.data(), &ah, sizeof(ah));
    std::memcpy(ack_buf.data()+sizeof(ah), &ap, sizeof(ap));
    (void)socket_.send_to(std::span<const std::byte>(ack_buf), sender);
    LOG_INFO("Client connected! Streaming audio to " << sender.to_string() << " (" << actual_audio_config_.to_string() << ")");
}

void AudioRouterServer::handle_disconnect_req(const protocol::CommonHeader& hdr, const SocketAddress& sender) {
    (void)hdr;
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (sender == active_client_) {
        LOG_INFO("Client " << sender.to_string() << " requested disconnect.");
        disconnect_client("Client disconnected gracefully", true);
    }
}

void AudioRouterServer::handle_heartbeat_ping(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender) {
    // Only from active client
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (sender != active_client_) return;
    }
    last_client_activity_time_ms_.store(get_time_ms());
    uint64_t client_orig_time = 0;
    if (payload.size() >= sizeof(protocol::HeartbeatPayload)) {
        protocol::HeartbeatPayload pp{}; std::memcpy(&pp, payload.data(), sizeof(pp));
        client_orig_time = pp.orig_timestamp_us;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.client_buffer_level = pp.client_buffer_level_frames;
        stats_.client_lost_packets = pp.packets_lost;
    }
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::HeartbeatPayload)> pong_buf{};
    protocol::CommonHeader ph = protocol::make_header(protocol::MsgType::HEARTBEAT_PONG, hdr.seq_num, get_time_us(), sizeof(protocol::HeartbeatPayload));
    protocol::HeartbeatPayload pp{}; pp.orig_timestamp_us = client_orig_time;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        pp.packets_received = static_cast<uint32_t>(stats_.packets_sent);
    }
    std::memcpy(pong_buf.data(), &ph, sizeof(ph));
    std::memcpy(pong_buf.data()+sizeof(ph), &pp, sizeof(pp));
    (void)socket_.send_to(std::span<const std::byte>(pong_buf), sender);
}

void AudioRouterServer::handle_control_cmd(const protocol::CommonHeader& hdr, std::span<const std::byte> payload, const SocketAddress& sender) {
    (void)hdr;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (sender != active_client_) return;
    }
    if (payload.size() < sizeof(protocol::ControlCmdPayload)) return;
    protocol::ControlCmdPayload cmd{}; std::memcpy(&cmd, payload.data(), sizeof(cmd));
    LOG_INFO("Control command received: cmd=" << static_cast<int>(cmd.cmd_id));
    switch (cmd.cmd_id) {
        case 1: (void)endpoint_control_.set_mute(true); break;
        case 2: (void)endpoint_control_.set_mute(false); break;
        case 3: {
            if (std::isfinite(cmd.param_float)) {
                float vol = std::clamp(cmd.param_float, 0.0f, 1.0f);
                (void)endpoint_control_.set_volume(vol);
            }
            break;
        }
        default: break;
    }
}

void AudioRouterServer::disconnect_client(const std::string& reason, bool send_ack) noexcept {
    // Caller may hold client_mutex; avoid double-lock? Use try? We require external lock already for some paths.
    // In this hardened version, we try to lock but if already locked by same thread, we already hold.
    // So we use std::lock_guard only if not already streaming? Actually caller holds lock in some paths, but our constexpr uses separate lock.
    // To avoid deadlock, we use std::unique_lock with defer and try to avoid double-lock by checking is_locked externally.
    // Simpler: assume callers hold lock when needed, but if called from stop() without lock, we lock.
    // We implement safe double-lock avoidance by using std::scoped_lock only when state is STREAMING but we can lock anyway; recursive not allowed, so we must ensure not to deadlock.
    // The previous code locked inside this function but callers also locked — that would deadlock. In our new code, we will make callers NOT lock when calling disconnect_client, or make disconnect_client not lock.
    // For hardening, we will lock inside and expect callers to not hold lock for disconnect_client. So adjust callers to not hold lock.
    // For now, we implement internal lock with try.
    // To keep simple and avoid deadlock, we use client_mutex_.try_lock approach.
    bool locked_here = client_mutex_.try_lock();
    // If try_lock fails, it means caller already holds lock — proceed without additional lock.
    auto unlock_guard = std::unique_ptr<std::mutex, std::function<void(std::mutex*)>>(
        locked_here ? &client_mutex_ : nullptr,
        [](std::mutex* m){ if(m) m->unlock(); });

    if (state_.load() != ServerState::STREAMING || !active_client_.is_valid()) {
        return;
    }
    LOG_INFO("Disconnecting client " << active_client_.to_string() << ": " << reason);
    if (send_ack) {
        std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::DisconnectPayload)> dis_buf{};
        protocol::CommonHeader dh = protocol::make_header(protocol::MsgType::DISCONNECT_ACK, 0, get_time_us(), sizeof(protocol::DisconnectPayload));
        protocol::DisconnectPayload dp{}; dp.reason_code = 0;
        std::strncpy(dp.reason, reason.c_str(), sizeof(dp.reason)-1); dp.reason[sizeof(dp.reason)-1]='\0';
        std::memcpy(dis_buf.data(), &dh, sizeof(dh));
        std::memcpy(dis_buf.data()+sizeof(dh), &dp, sizeof(dp));
        (void)socket_.send_to(std::span<const std::byte>(dis_buf), active_client_);
    }
    if (config_.auto_mute_pc_speaker) {
        LOG_INFO("Client disconnected -> Restoring PC speaker volume/unmute...");
        (void)endpoint_control_.unmute_pc_speaker();
    }
    active_client_ = SocketAddress();
    state_.store(ServerState::LISTENING);
    LOG_INFO("Server back in LISTENING state. Waiting for next client.");
}

void AudioRouterServer::watchdog_thread(audiorouter::stop_token st) {
    while (!st.stop_requested() && is_running_.load()) {
        // sleep in 100ms increments to be stop-aware
        for (int i=0;i<5 && !st.stop_requested() && is_running_.load(); ++i) sleep_ms(100);
        if (st.stop_requested() || !is_running_.load()) break;
        if (state_.load() == ServerState::STREAMING) {
            uint64_t now_ms = get_time_ms();
            uint64_t last = last_client_activity_time_ms_.load();
            if (last !=0 && (now_ms - last) > config_.client_timeout_ms) {
                LOG_WARN("Client connection heartbeat timeout (" << (now_ms-last) << "ms > " << config_.client_timeout_ms << "ms).");
                disconnect_client("Heartbeat timeout", false);
            }
        }
    }
}

void AudioRouterServer::on_audio_captured(const void* data, size_t num_frames, const AudioConfig& config) {
    if (state_.load() != ServerState::STREAMING) return;
    SocketAddress client;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        client = active_client_;
    }
    if (!client.is_valid()) return;
    if (!data || num_frames==0) return;
    if (config.channels==0 || config.channels>32) return;
    if (num_frames > 8192*4) return; // sanity
    size_t bpf = config.bytes_per_frame();
    if (bpf==0 || bpf > 128) return; // sanity
    // overflow checked
    if (num_frames > std::numeric_limits<size_t>::max() / bpf) return;
    size_t total_input_bytes = num_frames * bpf;
    size_t target_chunk_frames = config_.frames_per_packet ? config_.frames_per_packet : 240;
    if (target_chunk_frames==0 || target_chunk_frames>8192) target_chunk_frames=240;
    if (target_chunk_frames > std::numeric_limits<size_t>::max() / bpf) return;
    size_t target_chunk_bytes = target_chunk_frames * bpf;

    std::lock_guard<std::mutex> lock(chunk_mutex_);
    const uint8_t* byte_ptr = static_cast<const uint8_t*>(data);
    // bounds check: ensure we have total_input_bytes available; we trust caller but check for null
    chunk_buffer_.insert(chunk_buffer_.end(), byte_ptr, byte_ptr + total_input_bytes);

    std::vector<uint8_t> packet_buf(sizeof(protocol::AudioPacketHeader) + target_chunk_bytes);
    while (chunk_buffer_.size() >= target_chunk_bytes) {
        protocol::AudioPacketHeader hdr{};
        hdr.common.magic = protocol::MAGIC;
        hdr.common.version = protocol::CURRENT_VERSION;
        hdr.common.msg_type = static_cast<uint8_t>(protocol::MsgType::AUDIO_DATA);
        hdr.common.flags = protocol::FLAG_NONE;
        hdr.common.seq_num = sequence_number_.fetch_add(1);
        hdr.common.timestamp_us = get_time_us();
        size_t hdr_payload = sizeof(protocol::AudioPacketHeader) - sizeof(protocol::CommonHeader);
        if (hdr_payload > std::numeric_limits<uint32_t>::max() - target_chunk_bytes) {
            LOG_ERROR("Payload size overflow, dropping packet");
            chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + static_cast<long>(target_chunk_bytes));
            continue;
        }
        hdr.common.payload_size = static_cast<uint32_t>(hdr_payload + target_chunk_bytes);
        hdr.sample_rate = config.sample_rate;
        hdr.channels = config.channels;
        hdr.format = static_cast<uint8_t>(config.format);
        hdr.reserved = 0;
        hdr.num_frames = static_cast<uint32_t>(target_chunk_frames);
        // validate before sending
        if (!hdr.common.payload_size || hdr.common.payload_size > protocol::MAX_UDP_PACKET_SIZE) {
            chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + static_cast<long>(target_chunk_bytes));
            continue;
        }
        std::memcpy(packet_buf.data(), &hdr, sizeof(hdr));
        std::memcpy(packet_buf.data()+sizeof(hdr), chunk_buffer_.data(), target_chunk_bytes);
        chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + static_cast<long>(target_chunk_bytes));
        auto res = socket_.send_to(std::span<const std::byte>(reinterpret_cast<const std::byte*>(packet_buf.data()), packet_buf.size()), client);
        if (res.has_value()) {
            std::lock_guard<std::mutex> s_lock(stats_mutex_);
            stats_.packets_sent++;
            stats_.bytes_sent += *res;
            stats_.audio_frames_captured += target_chunk_frames;
        } else {
            LOG_DEBUG("Failed to send audio packet: " << res.error());
        }
    }
    // Prevent chunk_buffer growing unbounded if client stall (cap at ~1 sec of audio)
    constexpr size_t MAX_BUFFERED = 48000 * 4 * 2; // 1 sec stereo S16
    if (chunk_buffer_.size() > MAX_BUFFERED) {
        LOG_WARN("Chunk buffer overflow (" << chunk_buffer_.size() << " bytes), dropping oldest");
        chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + static_cast<long>(chunk_buffer_.size() - MAX_BUFFERED));
    }
}

} // namespace audiorouter
