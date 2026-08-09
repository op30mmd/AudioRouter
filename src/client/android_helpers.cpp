#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

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

void AndroidHelpers::print_android_troubleshooting_tips() {
    LOG_INFO("=== Android ALSA / Termux Root Tips ===");
    LOG_INFO("1. Make sure to run Termux with root: type 'su' or 'tsu' before launching.");
    LOG_INFO("2. If no sound is heard on the phone speaker:");
    LOG_INFO("   - Check '/proc/asound/cards' to see the audio card name.");
    LOG_INFO("   - Check available nodes in '/dev/snd/'. Usually '/dev/snd/pcmC0D0p' is Card 0 Device 0 Playback.");
    LOG_INFO("   - Some Android Qualcomm/MediaTek devices require routing the mixer to speaker:");
    LOG_INFO("     Run: tinymix | grep -i 'speaker' or tinymix 'RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1' 1");
    LOG_INFO("3. You can pass a specific ALSA device using: -d direct:/dev/snd/pcmC0D0p or -d hw:0,0");
}

} // namespace audiorouter
