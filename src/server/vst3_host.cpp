// VST3 plugin host implementation.
//
// This file is compiled only when AUDIOROUTER_ENABLE_VST3 is defined
// at build time (see src/server/CMakeLists.txt). The companion stub
// (vst3_host_stub.cpp) provides the same make_vst3_stage() symbol so
// the rest of the codebase can always link regardless of whether the
// VST3 SDK was present at configure time.
//
// Namespace and IID conventions used here:
//
//   * The base FUnknown / IPluginFactory / IPluginFactory2 /
//     IPluginFactory3 interfaces are in the Steinberg:: namespace
//     (ipluginbase.h).
//   * All VST-specific interfaces (IComponent, IAudioProcessor,
//     IBStream fwd-declared here, AudioBusBuffers, ProcessData,
//     ProcessSetup, the MediaTypes / BusDirections /
//     SymbolicSampleSizes / ProcessModes enums, and the speaker
//     arrangement types) are in the Steinberg::Vst:: namespace.
//   * Interface IDs are exposed as static class members: IComponent::iid,
//     IAudioProcessor::iid, IPluginFactory::iid, etc. (FUID / TUID
//     types, not FIDString / const char*).
//
// The three lifecycle phases for a plugin instance are:
//
//   1. initialize(FUnknown* hostContext) on the IComponent - tells the
//      plugin about the host (this module's simple no-op host).
//   2. activateBus(mediaType, dir, busIndex, true) for the input and
//      output buses - the plugin needs to know which buses it should
//      expect to process on.
//   3. setActive(true) on the IComponent, and setupProcessing(...) on
//      the IAudioProcessor to lock in the sample rate / block size /
//      channel layout.
//
// Then each block of audio is delivered via
// IAudioProcessor::process(ProcessData&). ProcessData holds separate
// planar channel buffers (channelBuffers[c][i] is the i-th sample of
// channel c), while the host's on-wire format is interleaved stereo
// ([L0, R0, L1, R1, ...]). The host deinterleaves into a private
// scratch buffer (input half), runs the plugin (which writes to the
// scratch's output half), and re-interleaves back into the caller's
// buffer. The plugin thus sees distinct read and write pointers
// arranged planar, which is the standard VST3 setup most plugins are
// written and tested against.

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

// VST3 public SDK headers. The pluginterfaces/ subdirectory holds the
// interface definitions; public.sdk/source/vst/hosting/ has the
// host-side helper types (ProcessData helpers, etc.). We only need
// the public interface definitions; the dynamic loader
// (dlopen/LoadLibrary) resolves the rest at runtime, so no SDK
// libraries are linked.
//
// INIT_CLASS_IID tells the SDK headers to emit the out-of-line static
// `iid` members (e.g. IComponent::iid) inside the headers themselves
// rather than relying on a separate .cpp file (which we have not
// vendored). This is the recommended way to use the SDK headers-only
// in a self-contained host: define INIT_CLASS_IID, then include the
// SDK headers, and the IIDs become available as linkable symbols.
#define INIT_CLASS_IID 1
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/vstspeaker.h>
#undef INIT_CLASS_IID

// Provide a definition for Steinberg::FUnknownPrivate::atomicAdd
// (declared in pluginterfaces/base/funknown.h, normally defined in
// the SDK's funknown.cpp which we have not vendored). The host uses
// it for refcounting on the FUnknown interface; the operation is a
// straightforward atomic add. We use C++11 std::atomic here (the
// SDK's funknown.cpp has a platform-specific fast-path; the std
// version is portable and not on a hot path -- atomicAdd is called
// once per addRef/release, not per audio sample).
//
// Defined at global scope (not inside namespace audiorouter) so it
// lands in the right namespace: Steinberg::FUnknownPrivate, which
// is where the SDK declares it.
namespace Steinberg { namespace FUnknownPrivate {
    ::Steinberg::int32 PLUGIN_API atomicAdd(::Steinberg::int32& value, ::Steinberg::int32 amount) {
        return static_cast<::Steinberg::int32>(
            reinterpret_cast<std::atomic<::Steinberg::int32>&>(value).fetch_add(amount));
    }
}} // namespace Steinberg::FUnknownPrivate

namespace audiorouter {

using namespace Steinberg::Vst;  // kAudio, kInput, kOutput, kRealtime, kSample32, ProcessData, ...

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

// A VST3 module is a directory whose name ends in ".vst3" and contains
// a shared library named after the directory (the "bundle" pattern,
// like .app on macOS). On Linux this resolves to "<dir>/<basename>.so";
// on Windows to "<dir>\<basename>.dll"; on macOS to
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
    // RTLD_NOW: resolve all symbols upfront so a broken plugin does
    // not crash the audio thread on first use. RTLD_LOCAL: keep
    // plugin's symbols private so a second plugin with the same name
    // does not collide.
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

// A minimal IUnknown-derived host context. The VST3 spec requires us
// to hand the plugin a host pointer on initialize(); the only
// contract here is that we implement FUnknown (queryInterface,
// addRef, release). Plugins normally do not call back into the host
// in any interesting way during this host's lifetime, so a stub is
// sufficient.
//
// The SDK's convention for refcounting is to store the refcount in
// a `__funknownRefCount` member (see DECLARE_FUNKNOWN_METHODS /
// IMPLEMENT_REFCOUNT) and `delete this` when it hits zero. The
// destructor does NOT need to be virtual (FUnknown has no virtual
// destructor in the public SDK) -- the SDK design relies on the
// convention that every object derived from FUnknown implements
// the refcount macros correctly so the static-dispatch delete is
// safe. We follow that convention and use the same field name so
// the SDK's macro-based helpers (FUNKNOWN_DTOR, etc.) work.
//
// `final` silences the -Wdelete-non-virtual-dtor warning: it tells
// the compiler that no further derived class can call delete via a
// base pointer, so the static-dispatch `delete this` is safe.
class HostContext final : public Steinberg::FUnknown {
public:
    HostContext() : __funknownRefCount(1) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID /*iid*/, void** obj) SMTG_OVERRIDE {
        if (!obj) return Steinberg::kInvalidArgument;
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
        return ::Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE {
        if (::Steinberg::FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) {
            delete this;
            return 0;
        }
        return static_cast<Steinberg::uint32>(__funknownRefCount);
    }

protected:
    // Field name matches the SDK's DECLARE_FUNKNOWN_METHODS macro so
    // any SDK helper (FUNKNOWN_DTOR assertions, etc.) sees the
    // expected name.
    ::Steinberg::int32 __funknownRefCount;
};

// ---------------------------------------------------------------------------
// Vst3Stage: an IPluginStage that wraps a single loaded VST3 plugin.
// ---------------------------------------------------------------------------
class Vst3Stage : public IPluginStage {
public:
    Vst3Stage(std::unique_ptr<Vst3Module> module,
              Steinberg::Vst::IComponent* component,
              Steinberg::Vst::IAudioProcessor* processor,
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
        if (r != Steinberg::kResultOk) {
            LOG_ERROR("VST3: IComponent::initialize failed for '" << name_ << "'");
            return false;
        }

        // 2. Discover the plugin's audio buses. We want exactly one
        //    stereo (or matching channel count) input bus and one
        //    matching output bus. Many VST3 plugins expose a single
        //    stereo in and out; we activate those.
        int32_t in_bus_count  = component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
        int32_t out_bus_count = component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
        if (in_bus_count < 1 || out_bus_count < 1) {
            LOG_ERROR("VST3: plugin '" << name_ << "' has no audio I/O (in=" << in_bus_count
                     << " out=" << out_bus_count << ")");
            component_->terminate();
            return false;
        }

        Steinberg::tresult ar = component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput,  0, 1);
        Steinberg::tresult br = component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, 1);
        if (ar != Steinberg::kResultOk || br != Steinberg::kResultOk) {
            LOG_ERROR("VST3: plugin '" << name_ << "' refused to activate audio buses");
            component_->terminate();
            return false;
        }

        // 3. Tell the plugin which channel arrangement we will
        //    provide. Speaker arrangements are 64-bit
        //    VstSpeakerArrangement structs. The canonical value for a
        //    stereo bus is SpeakerArr::kStereo (defined in the SDK).
        //    Its bit pattern is L (bit 0) + R (bit 1) = 0x3.
        //    setBusArrangements takes a non-const pointer despite the
        //    logical read-only intent (the SDK API allows the plugin
        //    to overwrite it in some edge cases; we don't, so const_cast
        //    is safe here).
        Steinberg::Vst::SpeakerArrangement kStereo = 0x0000000000000003ULL;
        Steinberg::Vst::SpeakerArrangement kStereoOut = 0x0000000000000003ULL;
        Steinberg::tresult ba = processor_->setBusArrangements(&kStereo, 1, &kStereoOut, 1);
        if (ba != Steinberg::kResultOk) {
            LOG_ERROR("VST3: setBusArrangements failed for '" << name_
                     << "' (this plugin does not support stereo I/O)");
            component_->terminate();
            return false;
        }
        if (channels != 2) {
            LOG_WARN("VST3: plugin '" << name_ << "' is stereo-only but the host is configured for "
                     << channels << " channels; FX will run on the first 2 channels");
        }

        // 4. setupProcessing() with our format. The plugin may accept
        //    or refuse the sample rate / block size. The SDK takes
        //    setup by reference, not pointer.
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode          = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize   = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock   = static_cast<int32_t>(max_frames_per_block);
        setup.sampleRate           = static_cast<double>(sample_rate);
        r = processor_->setupProcessing(setup);
        if (r != Steinberg::kResultOk) {
            LOG_ERROR("VST3: setupProcessing failed for '" << name_ << "' ("
                     << sample_rate << "Hz, " << max_frames_per_block << " frames)");
            component_->terminate();
            return false;
        }

        // 5. setActive(true) and setProcessing(true) - the plugin
        //    enters its real-time-ready state.
        r = component_->setActive(true);
        if (r != Steinberg::kResultOk) {
            LOG_ERROR("VST3: IComponent::setActive failed for '" << name_ << "'");
            component_->terminate();
            return false;
        }
        r = processor_->setProcessing(true);
        if (r != Steinberg::kResultOk) {
            LOG_ERROR("VST3: IAudioProcessor::setProcessing failed for '" << name_ << "'");
            component_->setActive(false);
            component_->terminate();
            return false;
        }

        sample_rate_ = sample_rate;
        channels_ = channels;
        max_frames_per_block_ = max_frames_per_block;

        // Scratch buffer holds two planar copies of the input block:
        // one for the plugin's input, one for the plugin's output.
        // Each planar copy is laid out as
        //   [channel 0 samples | channel 1 samples | ...]
        // with each channel's samples contiguous, so a single
        // channel of `max_frames_per_block` samples lives at offset
        // `channel * max_frames_per_block`. The scratch is sized to
        // hold two such planar copies back-to-back.
        scratch_.resize(static_cast<size_t>(max_frames_per_block) * channels * 2);

        prepared_ = true;
        return true;
    }

    void unprepare() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepared_) return;
        if (processor_) processor_->setProcessing(false);
        if (component_) component_->setActive(false);
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

        // VST3 expects planar channel buffers (channelBuffers[c][i] is
        // the i-th sample of channel c), but the host passes
        // interleaved audio ([L0, R0, L1, R1, ...]). Convert between
        // the two layouts via the scratch buffer:
        //
        //   in_base  = scratch input half  (planar, for the plugin to read)
        //   out_base = scratch output half (planar, for the plugin to write)
        //
        // The halves are disjoint so the plugin sees distinct read
        // and write pointers, which is the standard VST3 setup most
        // plugins are written and tested against. (The VST3 spec
        // permits in-place processing, but plugins generally
        // assume they aren't.) We deinterleave the input into the
        // input half, run the plugin, then re-interleave the output
        // half back into the caller's buffer.
        const size_t total = num_frames * channels_;
        const size_t half = static_cast<size_t>(max_frames_per_block_) * channels_;
        if (scratch_.size() < half * 2) {
            // Defensive: prepare() sized this, so it should never
            // resize here. Resize and continue rather than fail.
            scratch_.resize(half * 2);
        }
        float* in_base  = scratch_.data();
        float* out_base = scratch_.data() + half;

        const bool stereo = (channels_ == 2);
        if (stereo) {
            // Deinterleave: L0, R0, L1, R1, ... -> planar [L0, L1, ...], [R0, R1, ...]
            for (size_t i = 0; i < num_frames; ++i) {
                in_base[0 * max_frames_per_block_ + i] = interleaved[2 * i + 0];
                in_base[1 * max_frames_per_block_ + i] = interleaved[2 * i + 1];
            }
        } else {
            // Mono (or >2 channels handled defensively): copy as-is.
            for (size_t i = 0; i < num_frames; ++i) {
                in_base[0 * max_frames_per_block_ + i] = interleaved[i * channels_];
            }
        }

        // Set up channel pointers. Inputs read from the input half;
        // outputs write to the output half. The plugin sees distinct
        // read and write pointers, which is the standard VST3 setup.
        std::vector<float*> in_chan(channels_);
        std::vector<float*> out_chan(channels_);
        for (uint16_t c = 0; c < channels_; ++c) {
            in_chan[c]  = in_base  + static_cast<size_t>(c) * max_frames_per_block_;
            out_chan[c] = out_base + static_cast<size_t>(c) * max_frames_per_block_;
        }

        Steinberg::Vst::AudioBusBuffers in_bus{};
        in_bus.numChannels         = static_cast<int32_t>(channels_);
        in_bus.silenceFlags        = 0;
        in_bus.channelBuffers32    = in_chan.data();

        Steinberg::Vst::AudioBusBuffers out_bus{};
        out_bus.numChannels        = static_cast<int32_t>(channels_);
        out_bus.silenceFlags       = 0;
        out_bus.channelBuffers32   = out_chan.data();

        Steinberg::Vst::ProcessData data{};
        data.processMode           = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize    = Steinberg::Vst::kSample32;
        data.numSamples            = static_cast<int32_t>(num_frames);
        data.numInputs             = 1;
        data.numOutputs            = 1;
        data.inputs                = &in_bus;
        data.outputs               = &out_bus;

        Steinberg::tresult r = processor_->process(data);
        if (r != Steinberg::kResultOk) {
            // A non-zero return typically means the plugin entered a
            // bad state (e.g. out-of-memory). We log once and pass
            // through the input for the rest of the lifetime to avoid
            // log spam on every block. The re-interleave below sees
            // the original (un-processed) input.
            static std::once_flag once;
            std::call_once(once, [this]() {
                LOG_ERROR("VST3: '" << name_ << "' process() returned non-zero; passing through");
            });
            // Copy the deinterleaved input into the output half so the
            // re-interleave produces the original audio.
            std::memcpy(out_base, in_base, total * sizeof(float));
        }

        // Re-interleave the (now processed) planar output back into
        // the caller's interleaved buffer.
        if (stereo) {
            for (size_t i = 0; i < num_frames; ++i) {
                interleaved[2 * i + 0] = out_base[0 * max_frames_per_block_ + i];
                interleaved[2 * i + 1] = out_base[1 * max_frames_per_block_ + i];
            }
        } else {
            for (size_t i = 0; i < num_frames; ++i) {
                interleaved[i * channels_] = out_base[0 * max_frames_per_block_ + i];
            }
        }
    }

    std::string name() const override { return name_; }

    // Native handle accessors. Used by the server's open_editors
    // plumbing to pass the IComponent and IPluginFactory pointers to
    // the GUI thread without dragging the SDK headers into
    // IPluginStage. The values are only valid while the stage is
    // prepared (i.e. between successful prepare() and unprepare()).
    void* native_handle() const override { return component_; }
    void* plugin_factory() const override {
        return module_ ? module_->factory() : nullptr;
    }

private:
    std::unique_ptr<Vst3Module> module_;
    Steinberg::Vst::IComponent*      component_ = nullptr;
    Steinberg::Vst::IAudioProcessor* processor_ = nullptr;
    std::string                      name_;

    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint32_t max_frames_per_block_ = 0;
    bool prepared_ = false;
    std::mutex mutex_;

    // See process() for layout. Two planar copies of the input block
    // (input + output), each channels_ * max_frames_per_block_ floats.
    std::vector<float> scratch_;
};

// Pick the first audio effect class from the module. The VST3 spec
// groups classes by category string; we look for the canonical
// "Audio FX" category first and fall back to "Fx" (some plugins use
// the older "Fx" label). If neither matches, the first class is used
// (most .vst3 bundles ship exactly one audio effect).
//
// PClassInfo's text fields are fixed-size char8 arrays (never null)
// and may not be NUL-terminated within their declared size, so we
// use strnlen + std::string's (ptr, count) constructor to build a
// bounded string. The class ID (cid) is a 16-byte TUID and is
// returned as a binary string (suitable for reinterpret_cast to
// FIDString when calling createInstance).
static std::pair<std::string, std::string> pick_class(Steinberg::IPluginFactory* factory) {
    int32_t n = factory->countClasses();
    int first_audio_fx = -1;
    int first_fx = -1;
    int first_any = 0;
    for (int32_t i = 0; i < n; ++i) {
        Steinberg::PClassInfo info{};
        if (factory->getClassInfo(i, &info) != Steinberg::kResultOk) continue;
        const std::string cat(info.category,
            strnlen(info.category, sizeof(info.category)));
        if (first_audio_fx < 0 && cat == "Audio FX") first_audio_fx = i;
        if (first_fx < 0 && (cat == "Fx" || cat == "FX")) first_fx = i;
        if (i == 0) { first_any = i; }
    }
    int idx = (first_audio_fx >= 0) ? first_audio_fx
            : (first_fx >= 0)         ? first_fx
            :                            first_any;
    Steinberg::PClassInfo info{};
    if (factory->getClassInfo(idx, &info) != Steinberg::kResultOk) {
        return {"", ""};
    }
    return {std::string(reinterpret_cast<const char*>(info.cid), sizeof(info.cid)),
            std::string(info.name,
                strnlen(info.name, sizeof(info.name)))};
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

    // Instantiate the class. We ask for IComponent first because
    // that's the root lifecycle interface; IAudioProcessor is queried
    // off the same instance below. IIDs are FUID / TUID class-static
    // members, not string constants.
    Steinberg::Vst::IComponent* component = nullptr;
    Steinberg::tresult r = factory->createInstance(reinterpret_cast<Steinberg::FIDString>(cid.data()),
                                                   Steinberg::Vst::IComponent::iid,
                                                   reinterpret_cast<void**>(&component));
    if (r != Steinberg::kResultOk || !component) {
        LOG_ERROR("VST3: createInstance(IComponent) failed for class '" << name << "' in '" << vst3_path << "'");
        return nullptr;
    }

    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    r = component->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                  reinterpret_cast<void**>(&processor));
    if (r != Steinberg::kResultOk || !processor) {
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
