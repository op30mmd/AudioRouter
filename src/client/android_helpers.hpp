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
    static void print_android_troubleshooting_tips();
};

} // namespace audiorouter
