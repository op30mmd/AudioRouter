#include "client.hpp"
#include "alsa_player.hpp"
#include "dummy_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cstring>
#include <vector>
#include <span>
#include <algorithm>
#include <array>
#include <limits>

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
    if (is_running_.load()) return true;

    if (auto v = config_.validate(); !v) {
        LOG_ERROR("Invalid client config: " << v.error());
        return false;
    }

    LOG_INFO("=================================================");
    LOG_INFO(" Starting AudioRouter Android ALSA Client Engine (C++23)");
    LOG_INFO("=================================================");

    if (AndroidHelpers::is_running_as_root()) {
        LOG_INFO("Running with root privileges (UID 0). Direct ALSA access enabled.");
        (void)AndroidHelpers::fix_snd_permissions();
    } else {
        LOG_WARN("Not running as root. If ALSA device fails to open, run 'su' or 'tsu' in Termux.");
    }

    if (!socket_.open()) {
        LOG_ERROR("Failed to open client UDP socket");
        return false;
    }
    (void)socket_.set_buffer_sizes(1024 * 1024, 1024 * 1024);
    (void)socket_.set_qos_priority(true);

    if (config_.auto_discover) {
        state_.store(ClientState::DISCOVERING);
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

    if (config_.use_dummy_player) {
        player_ = std::make_unique<DummyPlayer>();
    } else {
        player_ = std::make_unique<AlsaPlayer>();
    }

    state_.store(ClientState::CONNECTING);
    if (!perform_handshake()) {
        LOG_ERROR("Failed to connect to Windows AudioRouter Server at " << server_addr_.to_string());
        socket_.close();
        return false;
    }

    jitter_buffer_.configure(audio_config_, config_.target_latency_ms);

    if (!player_->open(audio_config_, config_.device_name)) {
        LOG_WARN("Could not open ALSA device '" << config_.device_name << "'. Trying dummy sink fallback...");
        player_ = std::make_unique<DummyPlayer>();
        (void)player_->open(audio_config_, "dummy_fallback");
        AndroidHelpers::print_android_troubleshooting_tips();
    }

    is_running_.store(true);
    state_.store(ClientState::STREAMING);
    last_packet_time_ms_.store(get_time_ms());

    // Launch cooperative C++23 jthreads — they auto-join and respect stop_token
    net_thread_ = std::jthread([this](std::stop_token st){ this->network_receive_thread(st); });
    playback_thread_ = std::jthread([this](std::stop_token st){ this->audio_playback_thread(st); });
    heartbeat_thread_ = std::jthread([this](std::stop_token st){ this->heartbeat_thread(st); });

    LOG_INFO("AudioRouter Client connected and streaming directly to Android speakers!");
    return true;
}

void AudioRouterClient::stop() noexcept {
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false)) return;

    LOG_INFO("Stopping AudioRouter Client...");
    // Cooperative cancellation
    net_thread_.request_stop();
    playback_thread_.request_stop();
    heartbeat_thread_.request_stop();

    if (server_addr_.is_valid()) {
        std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::DisconnectPayload)> dis_buf{};
        protocol::CommonHeader hdr = protocol::make_header(protocol::MsgType::DISCONNECT_REQ, 0, get_time_us(), sizeof(protocol::DisconnectPayload));
        protocol::DisconnectPayload pay{}; pay.reason_code = 0;
        std::strncpy(pay.reason, "Client closed", sizeof(pay.reason)-1);
        pay.reason[sizeof(pay.reason)-1] = '\0';
        std::memcpy(dis_buf.data(), &hdr, sizeof(hdr));
        std::memcpy(dis_buf.data()+sizeof(hdr), &pay, sizeof(pay));
        (void)socket_.send_to(std::span<const std::byte>(dis_buf), server_addr_);
    }

    socket_.close();

    // jthreads join automatically on destruction, but we want to wait synchronously
    if (net_thread_.joinable()) net_thread_.join();
    if (playback_thread_.joinable()) playback_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

    if (player_) player_->close();

    state_.store(ClientState::STOPPED);
    LOG_INFO("AudioRouter Client stopped successfully.");
}

bool AudioRouterClient::is_running() const noexcept {
    return is_running_.load();
}

ClientState AudioRouterClient::get_state() const noexcept {
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
    (void)socket_.set_broadcast(true);
    (void)socket_.set_receive_timeout_ms(500);

    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::DiscoveryReqPayload)> disc_buf{};
    protocol::CommonHeader hdr = protocol::make_header(protocol::MsgType::DISCOVERY_REQ, 0, get_time_us(), sizeof(protocol::DiscoveryReqPayload));
    protocol::DiscoveryReqPayload pay{}; 
    std::strncpy(pay.client_name, "AudioRouter-Android", sizeof(pay.client_name)-1);
    pay.client_name[sizeof(pay.client_name)-1]='\0';
    pay.client_version = protocol::CURRENT_VERSION;
    std::memcpy(disc_buf.data(), &hdr, sizeof(hdr));
    std::memcpy(disc_buf.data()+sizeof(hdr), &pay, sizeof(pay));

    std::vector<std::string> probe_ips = {
        "255.255.255.255",
        "192.168.43.1",
        "192.168.137.1",
        "192.168.1.1",
        "192.168.0.1",
        "127.0.0.1"
    };
    std::vector<uint8_t> recv_buf(4096);
    for (int attempt=0; attempt<3; ++attempt) {
        for (const auto& ip : probe_ips) {
            SocketAddress target(ip, config_.server_port);
            (void)socket_.send_to(std::span<const std::byte>(disc_buf), target);
        }
        SocketAddress responder;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), responder);
        if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader)+sizeof(protocol::DiscoveryRespPayload))) {
            protocol::CommonHeader rh{}; std::memcpy(&rh, recv_buf.data(), sizeof(rh));
            if (rh.magic != protocol::MAGIC || rh.msg_type != static_cast<uint8_t>(protocol::MsgType::DISCOVERY_RESP)) continue;
            // validate header
            if (!protocol::is_valid_header(rh, static_cast<size_t>(bytes))) continue;
            protocol::DiscoveryRespPayload rp{}; std::memcpy(&rp, recv_buf.data()+sizeof(rh), sizeof(rp));
            out_server_addr = responder;
            LOG_INFO("Discovered server '" << rp.server_name << "' at " << responder.to_string()
                     << " (PC Muted: " << (rp.pc_muted ? "Yes" : "No") << ")");
            return true;
        }
        sleep_ms(200);
    }
    return false;
}

bool AudioRouterClient::perform_handshake() {
    LOG_INFO("Initiating handshake with server at " << server_addr_.to_string() << "...");
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::ConnectReqPayload)> req_buf{};
    protocol::CommonHeader req_hdr = protocol::make_header(protocol::MsgType::CONNECT_REQ, 0, get_time_us(), sizeof(protocol::ConnectReqPayload));
    protocol::ConnectReqPayload req_pay{}; 
    std::strncpy(req_pay.client_name, "AudioRouter-Android-Termux", sizeof(req_pay.client_name)-1);
    req_pay.client_name[sizeof(req_pay.client_name)-1]='\0';
    req_pay.preferred_sample_rate = 48000;
    req_pay.preferred_channels = 2;
    req_pay.preferred_format = static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE);
    req_pay.target_latency_ms = static_cast<uint16_t>(config_.target_latency_ms);
    std::memcpy(req_buf.data(), &req_hdr, sizeof(req_hdr));
    std::memcpy(req_buf.data()+sizeof(req_hdr), &req_pay, sizeof(req_pay));

    (void)socket_.set_receive_timeout_ms(1000);
    std::vector<uint8_t> recv_buf(4096);
    for (int retry=0; retry<5; ++retry) {
        LOG_DEBUG("Sending CONNECT_REQ (attempt " << (retry+1) << "/5)...");
        (void)socket_.send_to(std::span<const std::byte>(req_buf), server_addr_);

        SocketAddress from;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), from);
        if (bytes < static_cast<int>(sizeof(protocol::CommonHeader))) { sleep_ms(300); continue; }
        protocol::CommonHeader hdr{}; std::memcpy(&hdr, recv_buf.data(), sizeof(hdr));
        if (hdr.magic != protocol::MAGIC) { sleep_ms(300); continue; }
        if (!protocol::is_valid_header(hdr, static_cast<size_t>(bytes))) { sleep_ms(300); continue; }

        if (hdr.msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_ACK)) {
            if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader)+sizeof(protocol::ConnectAckPayload))) {
                protocol::ConnectAckPayload ack{}; std::memcpy(&ack, recv_buf.data()+sizeof(hdr), sizeof(ack));
                audio_config_.sample_rate = ack.sample_rate;
                audio_config_.channels = ack.channels;
                audio_config_.format = static_cast<AudioSampleFormat>(ack.format);
                audio_config_.frames_per_packet = ack.frames_per_packet;
                // validate negotiated config
                if (!audio_config_.is_valid()) {
                    LOG_WARN("Server negotiated invalid AudioConfig: " << audio_config_.to_string());
                    // clamp to safe defaults
                    if (audio_config_.sample_rate==0) audio_config_.sample_rate=48000;
                    if (audio_config_.channels==0) audio_config_.channels=2;
                    if (audio_config_.frames_per_packet==0) audio_config_.frames_per_packet=240;
                }
                LOG_INFO("-------------------------------------------------");
                LOG_INFO(" CONNECT_ACK Received from Windows Server!");
                LOG_INFO(" Stream Format: " << audio_config_.to_string());
                LOG_INFO(" PC Speaker Silenced: " << (ack.pc_speaker_muted ? "YES (Audio routed to Android)" : "NO"));
                LOG_INFO(" Server Message: " << ack.status_msg);
                LOG_INFO("-------------------------------------------------");
                return true;
            }
        } else if (hdr.msg_type == static_cast<uint8_t>(protocol::MsgType::CONNECT_NAK)) {
            if (bytes >= static_cast<int>(sizeof(protocol::CommonHeader)+sizeof(protocol::ConnectNakPayload))) {
                protocol::ConnectNakPayload nak{}; std::memcpy(&nak, recv_buf.data()+sizeof(hdr), sizeof(nak));
                LOG_ERROR("Server rejected connection: " << nak.reason);
                return false;
            }
        }
        sleep_ms(300);
    }
    return false;
}

void AudioRouterClient::network_receive_thread(std::stop_token st) {
    std::vector<uint8_t> recv_buf(65536);
    (void)socket_.set_receive_timeout_ms(200);

    while (!st.stop_requested() && is_running_.load()) {
        SocketAddress sender;
        int bytes = socket_.receive_from(recv_buf.data(), recv_buf.size(), sender);
        if (bytes <= 0) {
            if (!is_running_.load() || st.stop_requested()) break;
            continue;
        }
        if (static_cast<size_t>(bytes) < sizeof(protocol::CommonHeader)) continue;

        protocol::CommonHeader hdr{}; std::memcpy(&hdr, recv_buf.data(), sizeof(hdr));
        if (!protocol::is_valid_header(hdr, static_cast<size_t>(bytes))) continue;

        last_packet_time_ms_.store(get_time_ms());

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.packets_received++;
            stats_.bytes_received += static_cast<uint64_t>(bytes);
        }

        auto msg_type = static_cast<protocol::MsgType>(hdr.msg_type);
        if (msg_type == protocol::MsgType::AUDIO_DATA) {
            if (static_cast<size_t>(bytes) < sizeof(protocol::AudioPacketHeader)) continue;
            protocol::AudioPacketHeader ah{}; std::memcpy(&ah, recv_buf.data(), sizeof(ah));
            if (!protocol::validate_audio_header(ah, static_cast<size_t>(bytes)).has_value()) continue;
            const void* pcm = recv_buf.data() + sizeof(protocol::AudioPacketHeader);
            // Additional bounds: ensure pcm size matches num_frames
            size_t expected_pcm_bytes = static_cast<size_t>(ah.num_frames) * ah.channels * 2;
            if (static_cast<size_t>(bytes) < sizeof(protocol::AudioPacketHeader)+expected_pcm_bytes) continue;
            // push via safe span
            size_t total_samples = static_cast<size_t>(ah.num_frames) * ah.channels;
            // avoid overflow: already validated
            (void)jitter_buffer_.push_packet(ah.common.seq_num, ah.common.timestamp_us, pcm, ah.num_frames);
            (void)total_samples;
        } else if (msg_type == protocol::MsgType::HEARTBEAT_PONG) {
            if (static_cast<size_t>(bytes) >= sizeof(protocol::CommonHeader)+sizeof(protocol::HeartbeatPayload)) {
                protocol::HeartbeatPayload pong{}; std::memcpy(&pong, recv_buf.data()+sizeof(protocol::CommonHeader), sizeof(pong));
                uint64_t now_us = get_time_us();
                if (now_us > pong.orig_timestamp_us) {
                    uint32_t rtt = static_cast<uint32_t>(now_us - pong.orig_timestamp_us);
                    last_rtt_us_.store(rtt);
                }
            }
        } else if (msg_type == protocol::MsgType::DISCONNECT_ACK || msg_type == protocol::MsgType::DISCONNECT_REQ) {
            LOG_INFO("Received disconnect notification from server.");
            is_running_.store(false);
            break;
        }
    }
}

void AudioRouterClient::audio_playback_thread(std::stop_token st) {
    const size_t period_frames = audio_config_.frames_per_packet ? audio_config_.frames_per_packet : 240;
    const size_t channels = audio_config_.channels ? audio_config_.channels : 2;
    if (channels == 0 || channels > 32) return;
    std::vector<int16_t> play_buffer(period_frames * channels);
    while (!st.stop_requested() && is_running_.load()) {
        size_t frames = jitter_buffer_.pop_frames(play_buffer.data(), period_frames);
        if (frames > 0 && player_ && player_->is_open()) {
            // Validate that frames <= period_frames
            frames = std::min(frames, period_frames);
            size_t written = player_->write_frames(play_buffer.data(), frames);
            if (written > 0) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.frames_played += written;
            }
        } else {
            // Avoid busy spin, but respect stop request with timed wait
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void AudioRouterClient::heartbeat_thread(std::stop_token st) {
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::HeartbeatPayload)> ping_buf{};
    while (!st.stop_requested() && is_running_.load()) {
        // sleep with stop awareness: check every 100ms
        for (int i=0;i<10 && !st.stop_requested() && is_running_.load(); ++i) sleep_ms(100);
        if (st.stop_requested() || !is_running_.load()) break;

        auto j_stats = jitter_buffer_.get_stats();
        protocol::CommonHeader h = protocol::make_header(protocol::MsgType::HEARTBEAT_PING, 0, get_time_us(), sizeof(protocol::HeartbeatPayload));
        protocol::HeartbeatPayload p{}; 
        p.orig_timestamp_us = get_time_us();
        p.client_buffer_level_frames = static_cast<uint32_t>(jitter_buffer_.available_frames());
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            p.packets_received = static_cast<uint32_t>(stats_.packets_received);
        }
        p.packets_lost = static_cast<uint32_t>(j_stats.packets_lost);
        p.buffer_underruns = static_cast<uint32_t>(j_stats.underruns);
        p.buffer_overruns = static_cast<uint32_t>(j_stats.overruns);
        std::memcpy(ping_buf.data(), &h, sizeof(h));
        std::memcpy(ping_buf.data()+sizeof(h), &p, sizeof(p));
        (void)socket_.send_to(std::span<const std::byte>(ping_buf), server_addr_);

        uint64_t now_ms = get_time_ms();
        uint64_t last = last_packet_time_ms_.load();
        if (last !=0 && (now_ms - last) > config_.reconnect_timeout_ms) {
            LOG_WARN("Server packet stream timeout (" << (now_ms-last) << "ms). Attempting to re-handshake...");
            (void)perform_handshake();
            last_packet_time_ms_.store(get_time_ms());
        }
    }
}

bool AudioRouterClient::send_pc_mute_command(bool mute) {
    if (!server_addr_.is_valid() || !is_running_.load()) return false;
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::ControlCmdPayload)> cmd_buf{};
    protocol::CommonHeader h = protocol::make_header(protocol::MsgType::CONTROL_CMD, 0, get_time_us(), sizeof(protocol::ControlCmdPayload));
    protocol::ControlCmdPayload p{}; p.cmd_id = mute ? 1 : 2;
    std::memcpy(cmd_buf.data(), &h, sizeof(h));
    std::memcpy(cmd_buf.data()+sizeof(h), &p, sizeof(p));
    auto res = socket_.send_to(std::span<const std::byte>(cmd_buf), server_addr_);
    return res.has_value();
}

bool AudioRouterClient::send_pc_volume_command(float volume_0_to_1) {
    if (!server_addr_.is_valid() || !is_running_.load()) return false;
    volume_0_to_1 = std::clamp(volume_0_to_1, 0.0f, 1.0f);
    if (!std::isfinite(volume_0_to_1)) return false;
    std::array<std::byte, sizeof(protocol::CommonHeader)+sizeof(protocol::ControlCmdPayload)> cmd_buf{};
    protocol::CommonHeader h = protocol::make_header(protocol::MsgType::CONTROL_CMD, 0, get_time_us(), sizeof(protocol::ControlCmdPayload));
    protocol::ControlCmdPayload p{}; p.cmd_id = 3; p.param_float = volume_0_to_1;
    std::memcpy(cmd_buf.data(), &h, sizeof(h));
    std::memcpy(cmd_buf.data()+sizeof(h), &p, sizeof(p));
    auto res = socket_.send_to(std::span<const std::byte>(cmd_buf), server_addr_);
    return res.has_value();
}

} // namespace audiorouter
