#include "client.hpp"
#include "alsa_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>

namespace {
    std::atomic<bool> g_shutdown_requested{false};

    void signal_handler(int sig) {
        (void)sig;
        g_shutdown_requested.store(true);
    }
}

void print_banner() {
    std::cout << R"(
  █████╗ ██╗   ██╗██████╗ ██╗ ██████╗ ██████╗  ██████╗ ██╗   ██╗████████╗███████╗██████╗ 
 ██╔══██╗██║   ██║██╔══██╗██║██╔═══██╗██╔══██╗██╔═══██╗██║   ██║╚══██╔══╝██╔════╝██╔══██╗
 ███████║██║   ██║██║  ██║██║██║   ██║██████╔╝██║   ██║██║   ██║   ██║   █████╗  ██████╔╝
 ██╔══██║██║   ██║██║  ██║██║██║   ██║██╔══██╗██║   ██║██║   ██║   ██║   ██╔══╝  ██╔══██╗
 ██║  ██║╚██████╔╝██████╔╝██║╚██████╔╝██║  ██║╚██████╔╝╚██████╔╝   ██║   ███████╗██║  ██║
 ╚═╝  ╚═╝ ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚═╝  ╚═╝ ╚═════╝  ╚═════╝    ╚═╝   ╚══════╝╚═╝  ╚═╝
                       [Android Termux ALSA Client (Rooted)]
    )" << std::endl;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -s, --server <ip>         Windows PC Server IP address (e.g. 192.168.43.45 or 192.168.137.1)\n"
              << "  -p, --port <port>         Server UDP port (default: 44100)\n"
              << "  -d, --device <dev>        ALSA device name (default: 'default', 'hw:0,0', 'direct:/dev/snd/pcmC0D0p')\n"
              << "  -l, --latency <ms>        Target Jitter Buffer latency in ms (default: 35ms)\n"
              << "      --discover            Auto-discover server on local Wi-Fi Hotspot subnet\n"
              << "      --dummy               Use dummy audio player instead of ALSA (for testing/benchmarks)\n"
              << "      --list-devices        List detected ALSA and kernel PCM devices and exit\n"
              << "  -v, --verbose             Enable debug logging\n"
              << "  -h, --help                Show this help message\n\n"
              << "Hotspot Connection Instructions:\n"
              << "  Scenario A: PC connected to Android Mobile Hotspot\n"
              << "    1. Turn on Wi-Fi Hotspot on Android.\n"
              << "    2. Connect your Windows PC to the Hotspot.\n"
              << "    3. On Windows PC, run: audiorouter_server.exe\n"
              << "    4. On Android Termux (run 'su' first for root):\n"
              << "       ./audiorouter_client -s <PC_IP_ADDRESS> -p 44100\n\n"
              << "  Scenario B: Android connected to Windows Mobile Hotspot\n"
              << "    1. Turn on Windows Mobile Hotspot on PC (default gateway is usually 192.168.137.1).\n"
              << "    2. Connect Android phone to the PC Hotspot.\n"
              << "    3. On Android Termux: su && ./audiorouter_client -s 192.168.137.1 -p 44100\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    audiorouter::ClientConfig config;
    bool list_devs = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--list-devices") {
            list_devs = true;
        } else if (arg == "-v" || arg == "--verbose") {
            audiorouter::Logger::instance().set_level(audiorouter::LogLevel::Debug);
        } else if ((arg == "-s" || arg == "--server") && i + 1 < argc) {
            config.server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-d" || arg == "--device") && i + 1 < argc) {
            config.device_name = argv[++i];
        } else if ((arg == "-l" || arg == "--latency") && i + 1 < argc) {
            config.target_latency_ms = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--discover") {
            config.auto_discover = true;
        } else if (arg == "--dummy") {
            config.use_dummy_player = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (list_devs) {
        std::cout << "\nAvailable ALSA Playback Devices:\n";
        auto devs = audiorouter::AlsaPlayer::get_available_devices();
        for (const auto& d : devs) {
            std::cout << "  -> " << d << "\n";
        }
        std::cout << "\nKernel ALSA Nodes in /dev/snd/:\n";
        auto nodes = audiorouter::AndroidHelpers::get_dev_snd_nodes();
        for (const auto& n : nodes) {
            std::cout << "  -> " << n << "\n";
        }
        std::cout << "\n/proc/asound/cards:\n";
        auto cards = audiorouter::AndroidHelpers::get_proc_asound_cards();
        for (const auto& c : cards) {
            std::cout << "  " << c << "\n";
        }
        return 0;
    }

    print_banner();

    // Register signal handlers for clean disconnect
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    audiorouter::AudioRouterClient client(config);

    if (!client.start()) {
        LOG_FATAL("Failed to start AudioRouter Client.");
        return 1;
    }

    // Monitoring Loop
    uint64_t last_stats_time = audiorouter::get_time_ms();

    while (!g_shutdown_requested && client.is_running()) {
        audiorouter::sleep_ms(200);

        uint64_t now_ms = audiorouter::get_time_ms();
        if (now_ms - last_stats_time >= 5000 && client.get_state() == audiorouter::ClientState::STREAMING) {
            last_stats_time = now_ms;
            auto stats = client.get_stats();
            double rtt_ms = static_cast<double>(stats.round_trip_time_us) / 1000.0;

            LOG_INFO("Streaming Status: "
                     << "RTT: " << rtt_ms << " ms | "
                     << "Buffer: " << stats.jitter_stats.current_buffer_ms << " ms | "
                     << "Jitter: " << stats.jitter_stats.avg_jitter_ms << " ms | "
                     << "Lost: " << stats.jitter_stats.packets_lost << " pkts | "
                     << "Underruns: " << stats.jitter_stats.underruns << " | "
                     << "Played: " << stats.frames_played << " frames");
        }
    }

    if (g_shutdown_requested) {
        LOG_INFO("Termination requested, disconnecting gracefully...");
    }

    client.stop();
    return 0;
}
