#include "client.hpp"
#include "alsa_player.hpp"
#include "dummy_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cstring>
#include <vector>

namespace audiorouter {

AudioRouterClient::AudioRouterClient(const ClientConfig& config)
    : config_(config),
      is_running_(false),
      state_(ClientState::STOPPED),
      jitter_buffer_(config.target_latency_ms),
      last_packet_time_ms_(0),
      last_rtt_us_(0) {}

AudioRouterClient::~AudioRouterClient() {
    stop();
}

bool AudioRouterClient::start() {
    if (is_running_) return true;

    LOG_INFO("=================================================");
    LOG_INFO(" Starting AudioRouter Android ALSA Client Engine");
    LOG_INFO("=================================================");

    // Check Android root permissions
    if (AndroidHelpers::is_running_as_root()) {
        LOG_INFO("Running with root privileges (UID 0). Direct ALSA access enabled.");
        AndroidHelpers::fix_snd_permissions();
    } else {
        LOG_WARN("Not running as root. If ALSA device fails to open, run 'su' or 'tsu' in Termux.");
    }

    // Open UDP Socket
    if (!socket_.open()) {
        LOG_ERROR("Failed to open client UDP socket");
        return false;
    }

    socket_.set_buffer_sizes(1024 * 1024, 1024 * 1024);
    socket_.set_qos_priority(true);

    // Auto-discovery if requested
    if (config_.auto_discover) {
        state_ = ClientState::DISCOVERING;
        if (!discover_server(server_addr_)) {
            LOG_ERROR("Server auto-discovery failed. Please specify server IP manually: -s <IP>");
            socket_.close();
            return false;
        }
    } else {
        server_addr_ = SocketAddress(config_.server_ip, config_.server_port);
        if (!server_addr_.is_valid()) {
            LOG_ERROR("Invalid server address: " << config_.server_ip << ":" << config_.server_port);
            socket_.close();
            return false;
        }
    }

    LOG_INFO("Target Server: " << server_addr_.to_string());

    // Create Audio Player (ALSA or Dummy)
    if (config_.use_dummy_player) {
        player_ = std::make_unique<DummyPlayer>();
    } else {
        player_ = std::make_unique<AlsaPlayer>();
    }

    // Connect & Handshake with Windows Server
    state_ = ClientState::CONNECTING;
    if (!perform_handshake()) {
        LOG_ERROR("Failed to connect to Windows AudioRouter Server at " << server_addr_.to_string());
        socket_.close();
        return false;
    }

    // Configure Jitter Buffer with negotiated stream parameters
    jitter_buffer_.configure(audio_config_, config_.target_latency_ms);

    // Open ALSA Player with negotiated format
    if (!player_->open(audio_config_, config_.device_name)) {
        LOG_WARN("Could not open ALSA device '" << config_.device_name << "'. Trying dummy sink fallback...");
        player_ = std::make_unique<DummyPlayer>();
        player_->open(audio_config_, "dummy_fallback");
        AndroidHelpers::print_android_troubleshooting_tips();
    }

    is_running_ = true;
    state_ = ClientState::STREAMING;
    last_packet_time_ms_ = get_time_ms();

    // Start threads
    net_thread_ = std::thread(&AudioRouterClient::network_receive_thread, this);
    playback_thread_ = std::thread(&AudioRouterClient::audio_playback_thread, this);
    heartbeat_thread_ = std::thread(&AudioRouterClient::heartbeat_thread, this);

    LOG_INFO("AudioRouter Client connected and streaming directly to Android speakers!");
    return true;
}

void AudioRouterClient::stop() {
    if (!is_running_) return;

    LOG_INFO("Stopping AudioRouter Client...");
    is_running_ = false;

    // Send DISCONNECT_REQ to Windows server so it un-mutes PC speaker immediately
    if (server_addr_.is_valid()) {
        std::vector<uint8_t> dis_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DisconnectPayload));
        auto* dis_hdr = reinterpret_cast<protocol::CommonHeader*>(dis_buf.data());
        auto* dis_pay = reinterpret_cast<protocol::DisconnectPayload*>(dis_buf.data() + sizeof(protocol::CommonHeader));

        dis_hdr->magic = protocol::MAGIC;
        dis_hdr->version = protocol::CURRENT_VERSION;
        dis_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCONNECT_REQ);
        dis_hdr->flags = protocol::FLAG_NONE;
        dis_hdr->seq_num = 0;
        dis_hdr->timestamp_us = get_time_us();
        dis_hdr->payload_size = sizeof(protocol::DisconnectPayload);

        dis_pay->reason_code = 0;
        std::strncpy(dis_pay->reason, "Client closed", sizeof(dis_pay->reason) - 1);

        socket_.send_to(dis_buf.data(), dis_buf.size(), server_addr_);
    }

    // Close UDP socket
    socket_.close();

    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    if (net_thread_.joinable()) {
        net_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    if (player_) {
        player_->close();
    }

    state_ = ClientState::STOPPED;
    LOG_INFO("AudioRouter Client stopped successfully.");
}

bool AudioRouterClient::is_running() const {
    return is_running_;
}

ClientState AudioRouterClient::get_state() const {
    return state_.load();
}

ClientStats AudioRouterClient::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ClientStats s = stats_;
    s.jitter_stats = jitter_buffer_.get_stats();
    s.round_trip_time_us = last_rtt_us_.load();
    return s;
}

bool AudioRouterClient::discover_server(SocketAddress& out_server_addr) {
    LOG_INFO("Discovering Windows AudioRouter server on network...");
    socket_.set_broadcast(true);
    socket_.set_receive_timeout_ms(500);

    std::vector<uint8_t> disc_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryReqPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(disc_buf.data());
    auto* pay = reinterpret_cast<protocol::DiscoveryReqPayload*>(disc_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCOVERY_REQ);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::DiscoveryReqPayload);

    std::strncpy(pay->client_name, "AudioRouter-Android", sizeof(pay->client_name) - 1);
    pay->client_version = protocol::CURRENT_VERSION;

    // Send discovery broadcast and targeted probes to hotspot defaults
    std::vector<std::string> probe_ips = {
        "255.255.255.255",
        "192.168.43.1",   // Android hotspot gateway
        "192.168.137.1",  // Windows mobile hotspot gateway
        "192.168.1.1",
        "192.168.0.1",
        "127.0.0.1"
    };

    std::vector<uint8_t> recv_buf(4096);

    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const auto& ip : probe_ips) {
            SocketAddress target(ip, config_.server_port);
            socket_.send_to(disc_buf.data(), disc_buf.size(), target);
        }

        SocketAddress responder;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), responder);
        if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryRespPayload))) {
            const auto* resp_hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
            const auto* resp_pay = reinterpret_cast<const protocol::DiscoveryRespPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));

            if (resp_hdr->magic == protocol::MAGIC &&
                resp_hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::DISCOVERY_RESP)) {
                out_server_addr = responder;
                LOG_INFO("Discovered server '" << resp_pay->server_name << "' at " << responder.to_string()
                         << " (PC Muted: " << (resp_pay->pc_muted ? "Yes" : "No") << ")");
                return true;
            }
        }
        sleep_ms(200);
    }

    return false;
}

bool AudioRouterClient::perform_handshake() {
    LOG_INFO("Initiating handshake with server at " << server_addr_.to_string() << "...");

    std::vector<uint8_t> req_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectReqPayload));
    auto* req_hdr = reinterpret_cast<protocol::CommonHeader*>(req_buf.data());
    auto* req_pay = reinterpret_cast<protocol::ConnectReqPayload*>(req_buf.data() + sizeof(protocol::CommonHeader));

    req_hdr->magic = protocol::MAGIC;
    req_hdr->version = protocol::CURRENT_VERSION;
    req_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONNECT_REQ);
    req_hdr->flags = protocol::FLAG_NONE;
    req_hdr->seq_num = 0;
    req_hdr->timestamp_us = get_time_us();
    req_hdr->payload_size = sizeof(protocol::ConnectReqPayload);

    std::strncpy(req_pay->client_name, "AudioRouter-Android-Termux", sizeof(req_pay->client_name) - 1);
    req_pay->preferred_sample_rate = 48000;
    req_pay->preferred_channels = 2;
    req_pay->preferred_format = static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE);
    req_pay->target_latency_ms = static_cast<uint16_t>(config_.target_latency_ms);

    socket_.set_receive_timeout_ms(1000);
    std::vector<uint8_t> recv_buf(4096);

    for (int retry = 0; retry < 5; ++retry) {
        LOG_DEBUG("Sending CONNECT_REQ (attempt " << (retry + 1) << "/5)...");
        socket_.send_to(req_buf.data(), req_buf.size(), server_addr_);

        SocketAddress from;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), from);

        if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader))) {
            const auto* hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
            if (hdr->magic != protocol::MAGIC) continue;

            if (hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_ACK)) {
                if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectAckPayload))) {
                    const auto* ack = reinterpret_cast<const protocol::ConnectAckPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                    
                    audio_config_.sample_rate = ack->sample_rate;
                    audio_config_.channels = ack->channels;
                    audio_config_.format = static_cast<AudioSampleFormat>(ack->format);
                    audio_config_.frames_per_packet = ack->frames_per_packet;

                    LOG_INFO("-------------------------------------------------");
                    LOG_INFO(" CONNECT_ACK Received from Windows Server!");
                    LOG_INFO(" Stream Format: " << audio_config_.to_string());
                    LOG_INFO(" PC Speaker Silenced: " << (ack->pc_speaker_muted ? "YES (Audio routed to Android)" : "NO"));
                    LOG_INFO(" Server Message: " << ack->status_msg);
                    LOG_INFO("-------------------------------------------------");
                    return true;
                }
            } else if (hdr->msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_NAK)) {
                if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::ConnectNakPayload))) {
                    const auto* nak = reinterpret_cast<const protocol::ConnectNakPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                    LOG_ERROR("Server rejected connection: " << nak->reason);
                    return false;
                }
            }
        }

        sleep_ms(300);
    }

    return false;
}

void AudioRouterClient::network_receive_thread() {
    std::vector<uint8_t> recv_buf(65536);
    socket_.set_receive_timeout_ms(200);

    while (is_running_) {
        SocketAddress sender;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), sender);

        if (bytes <= 0) {
            if (!is_running_) break;
            continue;
        }

        if (static_cast<size_t>(bytes) < sizeof(protocol::CommonHeader)) continue;

        const auto* hdr = reinterpret_cast<const protocol::CommonHeader*>(recv_buf.data());
        if (hdr->magic != protocol::MAGIC || hdr->version != protocol::CURRENT_VERSION) continue;

        last_packet_time_ms_ = get_time_ms();

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_received++;
            stats_.bytes_received += bytes;
        }

        auto msg_type = static_cast<protocol::MsgType>(hdr->msg_type);

        if (msg_type == protocol::MsgType::AUDIO_DATA) {
            if (static_cast<size_t>(bytes) >= sizeof(protocol::AudioPacketHeader)) {
                const auto* audio_hdr = reinterpret_cast<const protocol::AudioPacketHeader*>(recv_buf.data());
                const void* pcm_data = recv_buf.data() + sizeof(protocol::AudioPacketHeader);

                jitter_buffer_.push_packet(
                    audio_hdr->common.seq_num,
                    audio_hdr->common.timestamp_us,
                    pcm_data,
                    audio_hdr->num_frames
                );
            }
        } else if (msg_type == protocol::MsgType::HEARTBEAT_PONG) {
            if (static_cast<size_t>(bytes) >= sizeof(protocol::CommonHeader) + sizeof(protocol::HeartbeatPayload)) {
                const auto* pong = reinterpret_cast<const protocol::HeartbeatPayload*>(recv_buf.data() + sizeof(protocol::CommonHeader));
                uint64_t now_us = get_time_us();
                if (now_us > pong->orig_timestamp_us) {
                    uint32_t rtt = static_cast<uint32_t>(now_us - pong->orig_timestamp_us);
                    last_rtt_us_ = rtt;
                }
            }
        } else if (msg_type == protocol::MsgType::DISCONNECT_ACK || msg_type == protocol::MsgType::DISCONNECT_REQ) {
            LOG_INFO("Received disconnect notification from server.");
            is_running_ = false;
            break;
        }
    }
}

void AudioRouterClient::audio_playback_thread() {
    const size_t period_frames = audio_config_.frames_per_packet > 0 ? audio_config_.frames_per_packet : 240;
    std::vector<int16_t> play_buffer(period_frames * audio_config_.channels);

    while (is_running_) {
        // Read frames from jitter buffer
        size_t frames = jitter_buffer_.pop_frames(play_buffer.data(), period_frames);

        if (frames > 0 && player_ && player_->is_open()) {
            size_t written = player_->write_frames(play_buffer.data(), frames);
            if (written > 0) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.frames_played += written;
            }
        } else {
            sleep_ms(2);
        }
    }
}

void AudioRouterClient::heartbeat_thread() {
    std::vector<uint8_t> ping_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::HeartbeatPayload));
    auto* ping_hdr = reinterpret_cast<protocol::CommonHeader*>(ping_buf.data());
    auto* ping_pay = reinterpret_cast<protocol::HeartbeatPayload*>(ping_buf.data() + sizeof(protocol::CommonHeader));

    while (is_running_) {
        sleep_ms(1000);
        if (!is_running_) break;

        auto j_stats = jitter_buffer_.get_stats();

        ping_hdr->magic = protocol::MAGIC;
        ping_hdr->version = protocol::CURRENT_VERSION;
        ping_hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::HEARTBEAT_PING);
        ping_hdr->flags = protocol::FLAG_NONE;
        ping_hdr->seq_num = 0;
        ping_hdr->timestamp_us = get_time_us();
        ping_hdr->payload_size = sizeof(protocol::HeartbeatPayload);

        ping_pay->orig_timestamp_us = get_time_us();
        ping_pay->client_buffer_level_frames = static_cast<uint32_t>(jitter_buffer_.available_frames());
        ping_pay->packets_received = static_cast<uint32_t>(stats_.packets_received);
        ping_pay->packets_lost = static_cast<uint32_t>(j_stats.packets_lost);
        ping_pay->buffer_underruns = static_cast<uint32_t>(j_stats.underruns);
        ping_pay->buffer_overruns = static_cast<uint32_t>(j_stats.overruns);

        socket_.send_to(ping_buf.data(), ping_buf.size(), server_addr_);

        // Watchdog check for server connection timeout
        uint64_t now_ms = get_time_ms();
        uint64_t last_packet = last_packet_time_ms_.load();
        if (last_packet > 0 && (now_ms - last_packet) > config_.reconnect_timeout_ms) {
            LOG_WARN("Server packet stream timeout (" << (now_ms - last_packet) << "ms). Attempting to re-handshake...");
            perform_handshake();
            last_packet_time_ms_ = get_time_ms();
        }
    }
}

bool AudioRouterClient::send_pc_mute_command(bool mute) {
    if (!server_addr_.is_valid()) return false;

    std::vector<uint8_t> cmd_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ControlCmdPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(cmd_buf.data());
    auto* pay = reinterpret_cast<protocol::ControlCmdPayload*>(cmd_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONTROL_CMD);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::ControlCmdPayload);

    pay->cmd_id = mute ? 1 : 2; // 1 = Mute, 2 = Unmute
    pay->param_float = 0.0f;
    pay->param_int = 0;

    return socket_.send_to(cmd_buf.data(), cmd_buf.size(), server_addr_) > 0;
}

bool AudioRouterClient::send_pc_volume_command(float volume_0_to_1) {
    if (!server_addr_.is_valid()) return false;

    std::vector<uint8_t> cmd_buf(sizeof(protocol::CommonHeader) + sizeof(protocol::ControlCmdPayload));
    auto* hdr = reinterpret_cast<protocol::CommonHeader*>(cmd_buf.data());
    auto* pay = reinterpret_cast<protocol::ControlCmdPayload*>(cmd_buf.data() + sizeof(protocol::CommonHeader));

    hdr->magic = protocol::MAGIC;
    hdr->version = protocol::CURRENT_VERSION;
    hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::CONTROL_CMD);
    hdr->flags = protocol::FLAG_NONE;
    hdr->seq_num = 0;
    hdr->timestamp_us = get_time_us();
    hdr->payload_size = sizeof(protocol::ControlCmdPayload);

    pay->cmd_id = 3; // Set Volume
    pay->param_float = volume_0_to_1;
    pay->param_int = 0;

    return socket_.send_to(cmd_buf.data(), cmd_buf.size(), server_addr_) > 0;
}

} // namespace audiorouter
