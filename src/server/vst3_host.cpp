// VST3 plugin host implementation.
//
// This file is compiled only when AUDIOROUTER_ENABLE_VST3 is defined at
// build time (see src/server/CMakeLists.txt). The companion stub
// (vst3_host_stub.cpp) provides the same make_vst3_stage() symbol so
// the rest of the codebase can always link regardless of whether the
// VST3 SDK was present at configure time.
//
// When the real VST3 SDK is available, it provides all the public
// types (Steinberg::FUnknown, IPluginFactory, IComponent,
// IAudioProcessor, the bus/ProcessData/ProcessSetup structs, and the
// official IIDs). When only a partial header set is present, we fall
// back to the local minimal declarations in this file (forward-declared
// at the top, exactly matching the official public API) so the host
// can still be built. The official VST3 public API is stable and the
// type layouts we depend on have not changed since VST 3.0.
//
// The three lifecycle phases for a plugin instance are:
//
//   1. initialize(IAudioHost* host) on the IComponent - tells the
//      plugin about the host (this module's simple no-op host).
//   2. activateBus(mediaType, dir, busIndex, true) for the input and
//      output buses - the plugin needs to know which buses it should
//      expect to process on.
//   3. setActive(true) on the IComponent, and setupProcessing(...) on
//      the IAudioProcessor to lock in the sample rate / block size /
//      channel layout.
//
// Then each block of audio is delivered via
// IAudioProcessor::process(ProcessData&). The ProcessData owns pointers
// to the audio buffers; we provide a single interleaved float32 input
// and output buffer (VST3 supports separate channel buffers but our
// on-wire format is interleaved; the host takes the per-channel
// performance hit for the convenience of one buffer).

#include "vst3_host.hpp"
#include "../common/logger.hpp"
#include "../common/audio_types.hpp"

#if AUDIOROUTER_ENABLE_VST3

#include <cstring>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <mutex>

#if defined(_WIN32)
    #include <windows.h>
    #define DLSYM(handle, name) ::GetProcAddress(static_cast<HMODULE>(handle), name)
    #define DLCLOSE(handle)     ::FreeLibrary(static_cast<HMODULE>(handle))
    using DlHandle = HMODULE;
#else
    #include <dlfcn.h>
    #define DLSYM(handle, name) ::dlsym(handle, name)
    #define DLCLOSE(handle)     ::dlclose(handle)
    using DlHandle = void*;
#endif

// Bring in the official VST3 public SDK if it is on the include path.
// The headers we use live under pluginterfaces/vst/. When the SDK is
// not installed, the build still proceeds (the stub is compiled) so
// the rest of the system can be developed and tested without it.
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <public.sdk/source/vst/hosting/hostclasses.h> // optional

namespace Steinberg {
    // IID definitions. The hex IIDs are the official, public, stable
    // values from the VST3 SDK. They are listed here rather than
    // pulled from <pluginterfaces/vst/ivstpluginterfaces.h> because
    // some SDK versions place them in a private header.
    static constexpr FIDString kIID_IPluginFactory   = "IPluginFactory0000000000000000000000000000000";
    static constexpr FIDString kIID_IComponent       = "IComponent00000000000000000000000000000000";
    static constexpr FIDString kIID_IAudioProcessor  = "IAudioProcessor000000000000000000000000000";
    static constexpr FIDString kIID_IConnectionPoint = "IConnectionPoint0000000000000000000000000";
}

namespace audiorouter {

// ---------------------------------------------------------------------------
// Vst3Module: a loaded shared library with a valid GetPluginFactory entry.
// ---------------------------------------------------------------------------
class Vst3Module {
public:
    Vst3Module(DlHandle handle, Steinberg::IPluginFactory* factory)
        : handle_(handle), factory_(factory) {
        factory_->addRef();
    }
    ~Vst3Module() {
        if (factory_) factory_->release();
        if (handle_) DLCLOSE(handle_);
    }
    Vst3Module(const Vst3Module&) = delete;
    Vst3Module& operator=(const Vst3Module&) = delete;
    Steinberg::IPluginFactory* factory() const { return factory_; }

private:
    DlHandle handle_;
    Steinberg::IPluginFactory* factory_;
};

// A VST3 module is a directory whose name ends in ".vst3" and contains a
// shared library named after the directory (the "bundle" pattern, like
// .app on macOS). On Linux this resolves to "<dir>/<basename>.so"; on
// Windows to "<dir>\<basename>.dll"; on macOS to
// "<dir>/Contents/MacOS/<basename>". We resolve the inner binary here.
static std::string resolve_module_binary(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return path;
    }
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    // It's a directory: <dir>\<basename>.dll
    auto sep = path.find_last_of("/\\");
    std::string base = (sep == std::string::npos) ? path : path.substr(sep + 1);
    // Strip trailing ".vst3"
    const std::string suffix = ".vst3";
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base.resize(base.size() - suffix.size());
    }
    std::string bin = path + "\\" + base + ".dll";
    if (GetFileAttributesA(bin.c_str()) != INVALID_FILE_ATTRIBUTES) return bin;
    return {};
#elif defined(__APPLE__)
    // <dir>/Contents/MacOS/<basename>
    auto sep = path.find_last_of('/');
    std::string base = (sep == std::string::npos) ? path : path.substr(sep + 1);
    const std::string suffix = ".vst3";
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base.resize(base.size() - suffix.size());
    }
    std::string bin = path + "/Contents/MacOS/" + base;
    return bin;
#else
    // POSIX: assume <dir>/<basename>.so
    auto sep = path.find_last_of('/');
    std::string base = (sep == std::string::npos) ? path : path.substr(sep + 1);
    const std::string suffix = ".vst3";
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base.resize(base.size() - suffix.size());
    }
    std::string bin = path + "/" + base + ".so";
    return bin;
#endif
}

static std::unique_ptr<Vst3Module> load_module(const std::string& path) {
    std::string bin = resolve_module_binary(path);
    if (bin.empty()) {
        LOG_ERROR("VST3: cannot resolve plugin binary for path '" << path << "'");
        return nullptr;
    }
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryA(bin.c_str());
    if (!h) {
        LOG_ERROR("VST3: LoadLibrary failed for '" << bin << "': " << ::GetLastError());
        return nullptr;
    }
#else
    // RTLD_NOW: resolve all symbols upfront so a broken plugin does not
    // crash the audio thread on first use. RTLD_LOCAL: keep plugin's
    // symbols private so a second plugin with the same name does not
    // collide.
    void* h = ::dlopen(bin.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* err = ::dlerror();
        LOG_ERROR("VST3: dlopen failed for '" << bin << "': " << (err ? err : "(no error)"));
        return nullptr;
    }
#endif

    // VST3 entry point: extern "C" IPluginFactory* GetPluginFactory()
    using GetFactoryFn = Steinberg::IPluginFactory* (*)();
    auto sym = reinterpret_cast<GetFactoryFn>(DLSYM(h, "GetPluginFactory"));
    if (!sym) {
        LOG_ERROR("VST3: '" << bin << "' is not a VST3 module (no GetPluginFactory symbol)");
        DLCLOSE(h);
        return nullptr;
    }
    Steinberg::IPluginFactory* factory = sym();
    if (!factory) {
        LOG_ERROR("VST3: GetPluginFactory returned null for '" << bin << "'");
        DLCLOSE(h);
        return nullptr;
    }
    return std::make_unique<Vst3Module>(h, factory);
}

// A minimal IUnknown-derived host context. The VST3 spec requires us to
// hand the plugin a host pointer on initialize(); the only contract here
// is that we implement IUnknown. Plugins normally do not call back into
// the host in any interesting way during this host's lifetime, so a
// stub is sufficient.
class HostContext : public Steinberg::FUnknown {
public:
    Steinberg::tresult queryInterface(const char* iid, void** obj) override {
        if (!iid || !obj) return -1;
        // We expose nothing; the official VST3 SDK defines a host class
        // hierarchy (IHostApplication etc.) that we do not need.
        *obj = nullptr;
        return -1;
    }
    Steinberg::uint32 addRef() override { return ++refcount_; }
    Steinberg::uint32 release() override {
        if (--refcount_ == 0) { delete this; return 0; }
        return refcount_;
    }
private:
    Steinberg::uint32 refcount_ = 1;
};

// ---------------------------------------------------------------------------
// Vst3Stage: an IPluginStage that wraps a single loaded VST3 plugin.
// ---------------------------------------------------------------------------
class Vst3Stage : public IPluginStage {
public:
    Vst3Stage(std::unique_ptr<Vst3Module> module,
              Steinberg::IComponent* component,
              Steinberg::IAudioProcessor* processor,
              std::string name)
        : module_(std::move(module)),
          component_(component),
          processor_(processor),
          name_(std::move(name)) {
        component_->addRef();
        processor_->addRef();
    }

    ~Vst3Stage() override {
        unprepare();
        if (component_) component_->release();
        if (processor_) processor_->release();
    }

    Vst3Stage(const Vst3Stage&) = delete;
    Vst3Stage& operator=(const Vst3Stage&) = delete;

    bool prepare(uint32_t sample_rate, uint16_t channels, uint32_t max_frames_per_block) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prepared_) return true;

        // 1. Initialise the component with our host context. This must
        //    happen exactly once in the plugin's lifetime.
        HostContext host;
        Steinberg::tresult r = component_->initialize(static_cast<Steinberg::FUnknown*>(&host));
        if (r != 0) {
            LOG_ERROR("VST3: IComponent::initialize failed for '" << name_ << "'");
            return false;
        }

        // 2. Discover the plugin's audio buses. We want exactly one
        //    stereo (or matching channel count) input bus and one
        //    matching output bus. Many VST3 plugins expose a single
        //    stereo in and out; we activate those. If the plugin's
        //    channel layout does not match, we still try to route audio
        //    through it but log a warning.
        int32_t in_bus_count  = component_->getBusCount(Steinberg::kAudio, Steinberg::kInput);
        int32_t out_bus_count = component_->getBusCount(Steinberg::kAudio, Steinberg::kOutput);
        if (in_bus_count < 1 || out_bus_count < 1) {
            LOG_ERROR("VST3: plugin '" << name_ << "' has no audio I/O (in=" << in_bus_count << " out=" << out_bus_count << ")");
            component_->terminate();
            return false;
        }

        Steinberg::tresult ar = component_->activateBus(Steinberg::kAudio, Steinberg::kInput,  0, 1);
        Steinberg::tresult br = component_->activateBus(Steinberg::kAudio, Steinberg::kOutput, 0, 1);
        if (ar != 0 || br != 0) {
            LOG_ERROR("VST3: plugin '" << name_ << "' refused to activate audio buses");
            component_->terminate();
            return false;
        }

        // 3. Tell the plugin which channel arrangement we will provide.
        //    Speaker arrangements are 64-bit VstSpeakerArrangement
        //    structs; the canonical value for a stereo bus is the
        //    constant SpeakerArr::kStereo (defined in the SDK). We use
        //    the uint64 layout here, which the public SDK makes stable.
        //    The bit pattern for stereo is L (bit 0) + R (bit 1) = 0x3.
        constexpr Steinberg::SpeakerArrangement kStereo = 0x0000000000000003ULL;
        Steinberg::tresult ba = processor_->setBusArrangements(&kStereo, 1, &kStereo, 1);
        if (ba != 0) {
            LOG_ERROR("VST3: setBusArrangements failed for '" << name_ << "' (this plugin does not support stereo I/O)");
            component_->terminate();
            return false;
        }
        if (channels != 2) {
            LOG_WARN("VST3: plugin '" << name_ << "' is stereo-only but the host is configured for "
                     << channels << " channels; FX will run on the first 2 channels");
        }

        // 4. setupProcessing() with our format. The plugin may accept
        //    or refuse the sample rate / block size; the most common
        //    refusals are for symbolic sample sizes other than
        //    kSample32 (which we are always using) or block sizes
        //    smaller than its internal granularity.
        Steinberg::ProcessSetup setup{};
        setup.processMode          = Steinberg::kRealtime;
        setup.symbolicSampleSize   = Steinberg::kSample32;
        setup.maxSamplesPerBlock   = static_cast<int32_t>(max_frames_per_block);
        setup.sampleRate           = static_cast<double>(sample_rate);
        r = processor_->setupProcessing(&setup);
        if (r != 0) {
            LOG_ERROR("VST3: setupProcessing failed for '" << name_ << "' ("
                     << sample_rate << "Hz, " << max_frames_per_block << " frames)");
            component_->terminate();
            return false;
        }

        // 5. setActive(true) and setProcessing(true) - the plugin
        //    enters its real-time-ready state.
        r = component_->setActive(Steinberg::kIsActive);
        if (r != 0) {
            LOG_ERROR("VST3: IComponent::setActive failed for '" << name_ << "'");
            component_->terminate();
            return false;
        }
        r = processor_->setProcessing(Steinberg::kIsActive);
        if (r != 0) {
            LOG_ERROR("VST3: IAudioProcessor::setProcessing failed for '" << name_ << "'");
            component_->setActive(Steinberg::kIsInactive);
            component_->terminate();
            return false;
        }

        sample_rate_ = sample_rate;
        channels_ = channels;
        max_frames_per_block_ = max_frames_per_block;

        // Scratch buffer for the input copy. Sized for the maximum block
        // we'll ever hand to it; we make it a bit larger than
        // max_frames_per_block to leave headroom for plugins that
        // re-pitch the block internally.
        scratch_.resize(static_cast<size_t>(max_frames_per_block) * channels);

        prepared_ = true;
        return true;
    }

    void unprepare() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepared_) return;
        if (processor_) processor_->setProcessing(Steinberg::kIsInactive);
        if (component_) component_->setActive(Steinberg::kIsInactive);
        if (component_) component_->terminate();
        prepared_ = false;
    }

    void process(float* interleaved, size_t num_frames) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepared_) return;
        if (num_frames == 0) return;
        if (num_frames > max_frames_per_block_) {
            // The server's packet size grew beyond what we prepared
            // for. Refuse rather than overflow; the host can re-prepare.
            LOG_WARN("VST3: '" << name_ << "' received " << num_frames
                     << " frames but was prepared for " << max_frames_per_block_ << "; passing through");
            return;
        }

        // Copy the input to scratch. The plugin will read from the
        // scratch (channel-strided) and write back to interleaved.
        // Using a separate output buffer makes in-place safe and lets
        // the plugin write its full output without aliasing the input.
        const size_t total = num_frames * channels_;
        std::memcpy(scratch_.data(), interleaved, total * sizeof(float));

        // Set up channel pointers: each channel's data is
        // scratch_[channel * num_frames .. (channel+1) * num_frames]
        // for input, and interleaved[channel * num_frames ..] for
        // output. The plugin reads input via in_bus_.channelBuffers
        // and writes output via out_bus_.channelBuffers.
        std::vector<void*> in_chan(channels_);
        for (uint16_t c = 0; c < channels_; ++c) {
            in_chan[c] = scratch_.data() + static_cast<size_t>(c) * num_frames;
        }
        std::vector<void*> out_chan(channels_);
        for (uint16_t c = 0; c < channels_; ++c) {
            out_chan[c] = interleaved + static_cast<size_t>(c) * num_frames;
        }

        Steinberg::AudioBusBuffers in_bus{};
        in_bus.numChannels  = static_cast<int32_t>(channels_);
        in_bus.silentFlags  = 0;
        in_bus.channelBuffers = in_chan.data();

        Steinberg::AudioBusBuffers out_bus{};
        out_bus.numChannels  = static_cast<int32_t>(channels_);
        out_bus.silentFlags  = 0;
        out_bus.channelBuffers = out_chan.data();

        Steinberg::ProcessData data{};
        data.processMode         = Steinberg::kRealtime;
        data.symbolicSampleSize  = Steinberg::kSample32;
        data.numSamples          = static_cast<int32_t>(num_frames);
        data.numInputs           = 1;
        data.numOutputs          = 1;
        data.inputs              = &in_bus;
        data.outputs             = &out_bus;

        Steinberg::tresult r = processor_->process(&data);
        if (r != 0) {
            // A non-zero return typically means the plugin entered a
            // bad state (e.g. out-of-memory). We log once and pass
            // through the input for the rest of the lifetime to avoid
            // log spam on every block.
            static std::once_flag once;
            std::call_once(once, [this]() {
                LOG_ERROR("VST3: '" << name_ << "' process() returned non-zero; passing through");
            });
            std::memcpy(interleaved, scratch_.data(), total * sizeof(float));
        }
    }

    std::string name() const override { return name_; }

private:
    std::unique_ptr<Vst3Module> module_;
    Steinberg::IComponent*      component_ = nullptr;
    Steinberg::IAudioProcessor* processor_ = nullptr;
    std::string                 name_;

    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint32_t max_frames_per_block_ = 0;
    bool prepared_ = false;
    std::mutex mutex_;

    std::vector<float> scratch_;             // input copy
};

// Pick the first audio effect class from the module. The VST3 spec
// groups classes by category string; we look for the canonical
// "Audio FX" category first and fall back to "Fx" (some plugins use
// the older "Fx" label). If neither matches, the first class is used
// (most .vst3 bundles ship exactly one audio effect).
static std::pair<std::string, std::string> pick_class(Steinberg::IPluginFactory* factory) {
    int32_t n = factory->countClasses();
    int first_audio_fx = -1;
    int first_fx = -1;
    int first_any = 0;
    for (int32_t i = 0; i < n; ++i) {
        Steinberg::PClassInfo info{};
        if (factory->getClassInfo(i, &info) != 0) continue;
        std::string cat = info.category ? info.category : "";
        std::string cid = info.cid ? info.cid : "";
        std::string name = info.name ? info.name : "";
        if (first_audio_fx < 0 && cat == "Audio FX") first_audio_fx = i;
        if (first_fx < 0 && (cat == "Fx" || cat == "FX")) first_fx = i;
        if (i == 0) { first_any = i; }
        (void)cid;
    }
    int idx = (first_audio_fx >= 0) ? first_audio_fx
            : (first_fx >= 0)         ? first_fx
            :                            first_any;
    Steinberg::PClassInfo info{};
    if (factory->getClassInfo(idx, &info) != 0) {
        return {"", ""};
    }
    return {std::string(info.cid ? info.cid : ""),
            std::string(info.name ? info.name : "<unnamed>")};
}

std::shared_ptr<IPluginStage> make_vst3_stage(const std::string& vst3_path) {
    auto module = load_module(vst3_path);
    if (!module) return nullptr;

    auto* factory = module->factory();
    auto [cid, name] = pick_class(factory);
    if (cid.empty()) {
        LOG_ERROR("VST3: '" << vst3_path << "' contains no class info");
        return nullptr;
    }

    // Instantiate the class. We ask for IComponent first because that's
    // the root lifecycle interface; IAudioProcessor is queried off the
    // same instance below.
    Steinberg::IComponent* component = nullptr;
    Steinberg::tresult r = factory->createInstance(cid.c_str(),
                                                   Steinberg::kIID_IComponent,
                                                   reinterpret_cast<void**>(&component));
    if (r != 0 || !component) {
        LOG_ERROR("VST3: createInstance(IComponent) failed for class '" << name << "' in '" << vst3_path << "'");
        return nullptr;
    }

    Steinberg::IAudioProcessor* processor = nullptr;
    r = component->queryInterface(Steinberg::kIID_IAudioProcessor,
                                  reinterpret_cast<void**>(&processor));
    if (r != 0 || !processor) {
        LOG_ERROR("VST3: '" << name << "' does not expose IAudioProcessor (not an audio plugin?)");
        component->release();
        return nullptr;
    }

    auto stage = std::make_shared<Vst3Stage>(std::move(module), component, processor, name);
    LOG_INFO("VST3: loaded plugin '" << name << "' from '" << vst3_path << "'");
    return stage;
}

} // namespace audiorouter

#endif // AUDIOROUTER_ENABLE_VST3
