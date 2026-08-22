// VST3 host stub. Always compiled (alongside or instead of vst3_host.cpp)
// so make_vst3_stage() is always defined, even when the VST3 SDK is not
// available at build time. The "real" implementation lives in
// vst3_host.cpp; this stub simply ensures the linker can always resolve
// the symbol.

#include "vst3_host.hpp"

namespace audiorouter {

std::shared_ptr<IPluginStage> make_vst3_stage(const std::string& vst3_path) {
    (void)vst3_path;
    return nullptr;
}

} // namespace audiorouter
