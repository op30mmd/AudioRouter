#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>

// Forward declarations to keep the SDK headers out of the public surface.
namespace Steinberg {
    class IPluginFactory;
    class IBStream;
}

namespace audiorouter {

// Manages a single plugin's editor window on a dedicated UI thread.
//
// The lifecycle is:
//   1. ctor        - construct with the plugin's display name (used as
//                    the window title).
//   2. start()     - launch the UI thread, which creates the Win32
//                    window and attaches the IPlugView. Returns once
//                    the thread is running (the window may still be
//                    being attached when start() returns, but the audio
//                    thread can start processing).
//   3. (audio thread) push_parameter_change()  - queue a parameter
//                    change. The UI thread will apply it at the next
//                    IEditController::setParamNormalized call.
//   4. (UI thread) the plugin's editor renders, the user interacts,
//                    the IComponentHandler callbacks fire on the UI
//                    thread. performEdit() queues a parameter change
//                    for the audio thread.
//   5. stop()      - signal the UI thread to close the window, join.
//
// This class is Windows-only at the implementation level (Win32
// window + message loop). The header is platform-neutral so callers
// don't have to wrap every reference in #ifdefs.
class Vst3PluginEditor {
public:
    Vst3PluginEditor();
    ~Vst3PluginEditor();

    // Non-copyable, non-movable: holds a thread and HWND.
    Vst3PluginEditor(const Vst3PluginEditor&) = delete;
    Vst3PluginEditor& operator=(const Vst3PluginEditor&) = delete;

    // Start the UI thread. The thread will:
    //   1. Create a hidden window and pump messages while it waits.
    //   2. Look up the plugin's IEditController via
    //      IComponent::getControllerClassId and IPluginFactory.
    //   3. Create the IPlugView via IEditController::createView("editor").
    //   4. Create a child window sized to IPlugView::getSize().
    //   5. Attach the IPlugView to the child window with
    //      IPlugView::attached(hwnd, "HWND") and setFrame().
    //   6. Show the window.
    //   7. Run the message loop until stop() is called or the window
    //      receives WM_CLOSE.
    //
    // On error (controller or view not found, attachment fails), the
    // thread logs the error and exits without a window.
    void start(Steinberg::IPluginFactory* factory,
               const std::string& plugin_name,
               void* component_ptr,        // IComponent* (void* to avoid
                                          // pulling the SDK header in)
               Steinberg::IBStream* component_state);  // optional saved state

    // Stop the UI thread. Posts WM_CLOSE to the window (if any) and
    // waits for the thread to exit. Safe to call from any thread.
    void stop();

    // Apply a queued parameter change. Called by the host's
    // IComponentHandler::performEdit on the UI thread, OR by the
    // audio thread when reading a queue of deferred changes.
    //
    // For now, single-threaded (UI thread only); queueing from the
    // audio thread is a future-work item.
    void performEdit(uint32_t param_id, double normalized_value);

    // Whether the editor window is currently up. For status logging
    // only -- the host doesn't gate on this.
    bool is_open() const { return is_open_.load(); }

private:
    // Actual UI thread work. Creates the window, attaches the view,
    // runs the message loop, tears down. Uses void* for the
    // SDK-typed parameters to keep the header SDK-clean; the cpp
    // file (which does include the SDK) casts back to the concrete
    // types.
    void ui_thread_main(Steinberg::IPluginFactory* factory,
                        const std::string& plugin_name,
                        void* component_ptr,
                        Steinberg::IBStream* component_state);

    // The Win32 window procedure. Handles WM_CLOSE, WM_SIZE, WM_DESTROY.
    // On Windows only; declared here so the cpp can register it via
    // SetWindowLongPtr. The HWND-to-Vst3PluginEditor map is keyed by
    // a window property set at creation time.
#if defined(_WIN32)
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
#endif

    std::thread thread_;
    std::atomic<bool> is_open_{false};

    // The HWND of the host window (the outer window with the title
    // bar and close button) and the child HWND that the IPlugView is
    // actually attached to. Set on the UI thread, read on the audio
    // thread for diagnostics. void* to keep the header platform-neutral.
    void* host_hwnd_ = nullptr;     // HWND
    void* view_hwnd_ = nullptr;     // HWND

    // Set on the UI thread, signaled to wake the UI thread when stop
    // is called. The UI thread checks is_stopping_ in the message
    // loop and also wakes when WM_CLOSE arrives.
    std::atomic<bool> is_stopping_{false};

    // Owned VST3 interface pointers, set on the UI thread during
    // start() and released in stop(). void* to keep the header free
    // of the SDK includes; the cpp casts to the appropriate SDK
    // type. (controller is Vst::IEditController; view is
    // Steinberg::IPlugView; handler is our own host-side impl.)
    void* controller_ = nullptr;  // Steinberg::Vst::IEditController*
    void* view_ = nullptr;        // Steinberg::IPlugView*
    void* handler_ = nullptr;     // HostComponentHandler*
};

// Manages a single plugin's editor (one per loaded plugin).
// Currently we have one editor per server, but the design supports
// multiple for the future "load N plugins" case.
class Vst3EditorManager {
public:
    Vst3EditorManager();
    ~Vst3EditorManager();

    // Open the editor for a plugin. The editor runs on its own
    // thread; this method returns once the thread is launched.
    void open_editor(Steinberg::IPluginFactory* factory,
                     const std::string& plugin_name,
                     void* component_ptr,
                     Steinberg::IBStream* component_state);

    // Close any open editor. Safe to call from any thread.
    void close_all();

private:
    std::unique_ptr<Vst3PluginEditor> editor_;
    std::mutex mu_;  // protects editor_ during swap
};

} // namespace audiorouter
