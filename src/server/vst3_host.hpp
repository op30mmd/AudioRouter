#pragma once

#include "plugin_chain.hpp"
#include <string>
#include <memory>

namespace audiorouter {

// Load a VST3 module from a .vst3 bundle (directory or .vst3 file path)
// and return an IPluginStage that wraps its single internal plugin. On
// any failure (file not found, module load error, no plugin class, I/O
// setup failure) the function returns nullptr and logs the reason. This
// function is compiled in two flavours:
//
//   * When AUDIOROUTER_ENABLE_VST3 is defined AND the VST3 SDK is
//     available at build time, this is a real host implementation that
//     dlopens the module, enumerates classes, initialises a plugin, sets
//     the bus configuration, and processes audio via the IComponent /
//     IAudioProcessor interfaces.
//
//   * Otherwise, a stub is compiled that always returns nullptr, so the
//     call site can transparently use bypass.
std::shared_ptr<IPluginStage> make_vst3_stage(const std::string& vst3_path);

} // namespace audiorouter
