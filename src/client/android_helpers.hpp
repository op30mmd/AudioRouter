#pragma once

#include <string>
#include <vector>

namespace audiorouter {

class AndroidHelpers {
public:
    static bool is_running_as_root();
    static bool fix_snd_permissions();
    static std::vector<std::string> get_proc_asound_cards();
    static std::vector<std::string> get_dev_snd_nodes();
    static bool run_tinymix_command(const std::string& control_name, const std::string& value);
    static bool apply_speaker_routing();
    static void print_android_troubleshooting_tips();

    // Bengal (SD662/680, WCD937x) detection - returns true if /proc/asound/cards contains "bengal"
    static bool is_bengal_board();

    // Whether AGM FIFO (agmplay named pipe) should be the default playback backend on this device
    // Currently true for Bengal boards where agmplay exists and direct PCM nodes are non-standard (no pcmC0D0p, 7 playback nodes)
    static bool should_default_to_agm();

    // Returns recommended default device string for this hardware
    // e.g., "agm:CODEC_DMA-LPAIF_RXTX-RX-1" on Bengal, "default" otherwise
    static std::string get_recommended_default_device();
};

} // namespace audiorouter
