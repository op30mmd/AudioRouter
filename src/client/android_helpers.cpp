#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace audiorouter {

bool AndroidHelpers::is_running_as_root() {
#if defined(__linux__) || defined(__ANDROID__)
    return (geteuid() == 0);
#else
    return false;
#endif
}

bool AndroidHelpers::fix_snd_permissions() {
#if defined(__linux__) || defined(__ANDROID__)
    if (!is_running_as_root()) {
        LOG_WARN("Cannot fix /dev/snd permissions: not running as root (uid != 0)");
        return false;
    }

    DIR* dir = opendir("/dev/snd");
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string full_path = std::string("/dev/snd/") + entry->d_name;
        chmod(full_path.c_str(), 0666);
    }
    closedir(dir);
    LOG_INFO("Set read/write permissions (0666) on /dev/snd/*");
    return true;
#else
    return true;
#endif
}

std::vector<std::string> AndroidHelpers::get_proc_asound_cards() {
    std::vector<std::string> cards;
    std::ifstream file("/proc/asound/cards");
    if (!file.is_open()) return cards;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            cards.push_back(line);
        }
    }
    return cards;
}

std::vector<std::string> AndroidHelpers::get_dev_snd_nodes() {
    std::vector<std::string> nodes;
#if defined(__linux__) || defined(__ANDROID__)
    DIR* dir = opendir("/dev/snd");
    if (!dir) return nodes;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        nodes.push_back(std::string("/dev/snd/") + entry->d_name);
    }
    closedir(dir);
#endif
    return nodes;
}

bool AndroidHelpers::run_tinymix_command(const std::string& control_name, const std::string& value) {
#if defined(__linux__) || defined(__ANDROID__)
    std::string cmd = "tinymix \"" + control_name + "\" \"" + value + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    return (ret == 0);
#else
    return false;
#endif
}

bool AndroidHelpers::apply_speaker_routing() {
#if defined(__linux__) || defined(__ANDROID__)
    // Best-effort speaker routing covering multiple Qualcomm families:
    // - Samsung A05s / Snapdragon 680 (AIF2_PB -> RX2 -> AUX_RDAC)
    // - Bengal-idp-snd-card (SD662/680, WCD937x + Bolero) with 7 playback nodes
    //   and no pcmC0D0p: needs RX_MACRO MUX -> AIF1_PB/AIF2_PB, RX MIX -> RX0/1/2,
    //   INT MIX routing, RDAC switches, and volume controls.
    // We try many controls; failures are ignored because device may not have them.

    struct Route {
        const char* control;
        const char* value;
    };

    const Route routes[] = {
        // Base Snapdragon 680 path (verified)
        {"RX_MACRO RX2 MUX", "AIF2_PB"},
        {"RX INT2_1 MIX1 INP0", "RX2"},
        {"AUX_RDAC Switch", "1"},

        // Bengal common (bengal-idp-snd-card, 184 controls, no pcmC0D0p)
        {"RX_MACRO RX0 MUX", "AIF1_PB"},
        {"RX_MACRO RX1 MUX", "AIF1_PB"},
        {"RX_MACRO RX2 MUX", "AIF1_PB"}, // fallback AIF1_PB for RX2
        {"RX_MACRO RX0 MUX", "AIF2_PB"}, // try both PB variants
        {"RX_MACRO RX1 MUX", "AIF2_PB"},
        {"RX MIX TX0 MUX", "RX0"},
        {"RX MIX TX1 MUX", "RX1"},
        {"RX MIX TX2 MUX", "RX2"},
        {"RX INT0_1 MIX1 INP0", "RX0"},
        {"RX INT1_1 MIX1 INP0", "RX1"},
        {"RX INT0 MIX2 INP", "RX0"},
        {"RX INT1 MIX2 INP", "RX1"},
        {"RX INT2 MIX2 INP", "RX2"},
        {"RX_RX0 Mix Digital Volume", "84"},
        {"RX_RX1 Mix Digital Volume", "84"},
        {"RX_RX2 Mix Digital Volume", "84"},
        {"RX_RX0 Digital Volume", "84"},
        {"RX_RX1 Digital Volume", "84"},
        {"RX_RX2 Digital Volume", "84"},
        {"HPHL_RDAC Switch", "1"},
        {"HPHR_RDAC Switch", "1"},
        {"EAR_RDAC Switch", "1"},
        // Generic fallbacks for older/newer Qualcomm
        {"RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1", "1"},
        {"PRI_MI2S_RX Audio Mixer MultiMedia1", "1"},
        {"SLIM_0_RX Audio Mixer MultiMedia1", "1"},
    };

    int success_count = 0;
    int total = 0;
    for (const auto& route : routes) {
        ++total;
        if (run_tinymix_command(route.control, route.value)) {
            ++success_count;
        }
    }

    if (success_count > 0) {
        LOG_INFO("AndroidHelpers: speaker routing applied (" << success_count << "/" << total << " controls succeeded, best effort)");
        return true;
    }
    LOG_WARN("AndroidHelpers: speaker routing: no controls succeeded (device may use different names, run android_mixer_setup.sh --list)");
    return false;
#else
    return false;
#endif
}

bool AndroidHelpers::is_bengal_board() {
#if defined(__linux__) || defined(__ANDROID__)
    std::ifstream file("/proc/asound/cards");
    if (!file.is_open()) return false;
    std::string line;
    while (std::getline(file, line)) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("bengal") != std::string::npos) {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

bool AndroidHelpers::should_default_to_agm() {
#if defined(__linux__) || defined(__ANDROID__)
    // Originally: Bengal boards were thought to benefit from AGM FIFO.
    // However, real device testing (bengal-idp-snd-card, 7 playback nodes, no pcmC0D0p)
    // shows agmplay dies immediately with broken pipe for all backends
    // CODEC_DMA-LPAIF_RXTX-RX-0/1/2/3, causing "broken pipe" errors.
    // The device was working before with direct PCM nodes (pcmC0D1p etc.).
    // User reported: "all of them were returned error 'broken pipe'" and
    // "it was working before your changes" when default was direct.
    // For stability, default back to direct PCM (AlsaPlayer/DirectAlsa) and
    // let user explicitly choose -d agm if they want to test AGM.
    // We keep Bengal detection for logging but return false for default.
    // If you want AGM default on Bengal, change this to true and agmplay will
    // be tried first with fallback to direct on failure (now with 300ms early death check).
    return false;
#else
    return false;
#endif
}

std::string AndroidHelpers::get_recommended_default_device() {
    if (should_default_to_agm()) {
        // On Bengal, RX-1 is the most commonly successful speaker backend,
        // but we return generic "agm" which will try RX-1 then fallback.
        // User can override with -d agm:CODEC_DMA-LPAIF_RXTX-RX-0 etc.
        return "agm:CODEC_DMA-LPAIF_RXTX-RX-1";
    }
    return "default";
}

void AndroidHelpers::print_android_troubleshooting_tips() {
    LOG_INFO("=== Android ALSA / Termux Root Tips ===");
    LOG_INFO("1. Make sure to run Termux with root: type 'su' first. Check with 'id' showing uid=0");
    LOG_INFO("2. If you see 'libtermux-exec.so not accessible' for agmplay:");
    LOG_INFO("   This is Termux's LD_PRELOAD blocking vendor binaries. Fixed in latest code via env -i:");
    LOG_INFO("   AgmFifoPlayer uses env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib to spawn agmplay cleanly");
    LOG_INFO("   For manual test, use: env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib PATH=/vendor/bin:/system/bin /vendor/bin/agmplay --help");
    LOG_INFO("3. If no sound on speaker (common on Bengal SD662/680 with 7 PCM nodes, no pcmC0D0p):");
    LOG_INFO("   - Bengal defaults to AGM named pipe (agmplay FIFO) which is more reliable than direct PCM");
    LOG_INFO("   - Check '/proc/asound/cards' (bengal-idp-snd-card has no pcmC0D0p, primary may be pcmC0D1p/D5p)");
    LOG_INFO("   - List nodes: ./bin/audiorouter_client --list-devices");
    LOG_INFO("   - Default on Bengal is now agm:CODEC_DMA-LPAIF_RXTX-RX-1, fallback to agm:RX-0, RX-2 etc.");
    LOG_INFO("   - Try each playback node if AGM fails: -d direct:/dev/snd/pcmC0D1p, D2p, D5p, D6p, D7p, D8p, D14p");
    LOG_INFO("   - Try AGM backends: -d agm:CODEC_DMA-LPAIF_RXTX-RX-0 / RX-1 / RX-2");
    LOG_INFO("   - Run mixer setup: ./scripts/android_mixer_setup.sh --bengal (as root)");
    LOG_INFO("   - Manual tinymix for Bengal:");
    LOG_INFO("     tinymix 'RX_MACRO RX0 MUX' 'AIF1_PB'; tinymix 'RX MIX TX0 MUX' 'RX0'; tinymix 'HPHL_RDAC Switch' 1");
    LOG_INFO("4. VPN tun0 active breaks hotspot audio (MTU 1300, routing through tunnel):");
    LOG_INFO("   - Disable VPN or bypass with: -b auto or -b wlan0");
    LOG_INFO("   - Example: ./bin/audiorouter_client -s <PC_IP> -d agm -b auto -v");
    LOG_INFO("   - Or with runner: ./scripts/termux_run.sh <PC_IP> -- -b auto -d agm");
    LOG_INFO("5. WiFi DOWN (wlan0 DOWN) - AudioRouter needs local hotspot:");
    LOG_INFO("   Scenario A: Android hotspot ON, PC connected to phone (192.168.43.x) - RECOMMENDED");
    LOG_INFO("   Scenario B: Windows hotspot ON, Android connected to PC (192.168.137.1)");
    LOG_INFO("6. If PCM node hangs: Android audioserver may hold it. Try (root): stop audioserver, test, then start audioserver");
    LOG_INFO("7. You can pass device: -d agm (default on Bengal), -d direct:/dev/snd/pcmC0D0p or -d hw:0,0");
}

} // namespace audiorouter
