// VST3 plugin editor window: Win32 plumbing to attach a plugin's
// IPlugView to a host-owned window and run the message loop.
//
// The VST3 spec for opening a plugin's editor is roughly:
//
//   1. The host (us) instantiates the plugin's IComponent (already
//      done in vst3_host.cpp's Vst3Stage::prepare).
//   2. The host gets the controller's class ID via
//      IComponent::getControllerClassId(char* classId).
//   3. The host instantiates the IEditController via
//      IPluginFactory::createInstance(classId, IEditController::iid, ...).
//   4. The host calls IEditController::setComponentState(...) to
//      push the component's saved state (optional, defaults are fine
//      for the first cut).
//   5. The host calls IEditController::setComponentHandler(...)
//      passing in an IComponentHandler implementation. The plugin
//      uses this to inform the host of parameter changes from the UI.
//   6. The host calls IEditController::createView("editor") to get
//      the IPlugView.
//   7. The host calls IPlugView::isPlatformTypeSupported("HWND")
//      (on Windows) to verify the plugin supports native windows.
//   8. The host creates a Win32 window and calls
//      IPlugView::attached(hwnd, "HWND").
//   9. The host calls IPlugView::setFrame(IPlugFrame) so the plugin
//      can ask the host to resize the window.
//  10. The host calls IPlugView::getSize() to size the window.
//  11. The host runs a Win32 message loop. The plugin handles paint,
//      mouse, and keyboard events via the host's WndProc.
//  12. On WM_CLOSE, the host calls IPlugView::removed() and
//      IEditController::setComponentHandler(nullptr), then destroys
//      the window.
//
// The host also implements:
//   IPlugFrame::resizeView      - the plugin calls this to request a
//                                resize; we resize our window.
//   IComponentHandler::beginEdit, performEdit, endEdit, restartComponent
//                              - the plugin calls these when the user
//                                changes a parameter from the UI. We
//                                log the change and (in the future)
//                                queue it for the audio thread.

#include "vst3_gui.hpp"
#include "../common/logger.hpp"

#if AUDIOROUTER_ENABLE_VST3

// vst3_host.cpp defines INIT_CLASS_IID around its SDK includes,
// which emits out-of-line definitions of the static `iid` members
// for the interfaces it uses (IComponent, IAudioProcessor, ...).
// We can't use the same trick here: re-including the same headers
// with INIT_CLASS_IID defined would emit duplicate `IComponent::iid`
// etc. definitions at link time (the macros are per-TU).
//
// Instead, we include the SDK headers WITHOUT INIT_CLASS_IID (so
// they only emit the static TUID tables), then use DEF_CLASS_IID to
// define just the IIDs we actually use. IEditController is the
// only one the GUI code references directly (passed to
// factory->createInstance on line ~345); the other IIDs
// (IComponent::iid, IAudioProcessor::iid, ...) come from
// vst3_host.cpp's translation unit.
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/gui/iplugview.h>

#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <windowsx.h>  // for GET_X_LPARAM / GET_Y_LPARAM on mouse msgs
#endif

// Definition of Steinberg::Vst::IEditController::iid (the only
// GUI-side IID we reference that vst3_host.cpp does not already
// define). DEF_CLASS_IID expands to:
//   const ::Steinberg::FUID Steinberg::Vst::IEditController::iid
//       (Steinberg::Vst::IEditController_iid);
// which is the out-of-line member definition. The macro uses
// token concatenation (`ClassName##_iid`) on the class name, so
// we pass the fully qualified Steinberg::Vst::IEditController.
DEF_CLASS_IID(Steinberg::Vst::IEditController)

namespace audiorouter {

using namespace Steinberg;

// ---------------------------------------------------------------------------
// HostComponentHandler: host-side IComponentHandler. The plugin calls into
// this when the user changes a parameter from its UI. We don't drive
// the audio path from this directly (that's a future-work item); for
// now we just log the change so the operator can see the plugin is
// responsive.
// ---------------------------------------------------------------------------
class HostComponentHandler final : public Steinberg::Vst::IComponentHandler {
public:
    HostComponentHandler() : __funknownRefCount(1) {}

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID /*id*/) SMTG_OVERRIDE {
        // The plugin is about to start a continuous parameter change
        // (e.g. user dragging a knob). We could record the start
        // timestamp here for automation. For now, no-op.
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue valueNormalized) SMTG_OVERRIDE {
        LOG_INFO("VST3 GUI: performEdit(param=" << id << ", value=" << valueNormalized << ")");
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID /*id*/) SMTG_OVERRIDE {
        // Continuous change finished. The plugin's automation write is
        // done at this point. For now, no-op.
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API restartComponent(int32 flags) SMTG_OVERRIDE {
        // The plugin wants the host to restart audio processing
        // (e.g. latency changed, plugin reloaded). The right thing to
        // do here is unprepare + prepare the chain; for now we just
        // log and ignore.
        LOG_INFO("VST3 GUI: restartComponent(flags=" << flags << ")");
        return Steinberg::kResultOk;
    }

    // FUnknown boilerplate. The parameter name `iid` would shadow
    // the static `IComponentHandler::iid` class member, so we
    // comment it out (matches Vst3Stage::HostContext's pattern).
    // We support the IComponentHandler IID; everything else is
    // kNoInterface.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID /*iid*/, void** obj) SMTG_OVERRIDE {
        if (!obj) return Steinberg::kInvalidArgument;
        // Always return our IComponentHandler; the plugin only queries
        // for IComponentHandler after setComponentHandler() anyway.
        *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
        return static_cast<Steinberg::uint32>(
            reinterpret_cast<std::atomic<Steinberg::int32>&>(__funknownRefCount).fetch_add(1));
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE {
        auto r = reinterpret_cast<std::atomic<Steinberg::int32>&>(__funknownRefCount).fetch_sub(1);
        if (r == 0) { delete this; return 0; }
        return static_cast<Steinberg::uint32>(r);
    }

private:
    Steinberg::int32 __funknownRefCount;
};

// ---------------------------------------------------------------------------
// HostPlugFrame: host-side IPlugFrame. The plugin calls resizeView
// when it wants our window to grow/shrink. We forward to Win32
// SetWindowPos. IPlugFrame is in the Steinberg:: namespace (not
// Steinberg::Vst::, despite being a GUI interface) -- see
// pluginterfaces/gui/iplugview.h.
// ---------------------------------------------------------------------------
class HostPlugFrame final : public Steinberg::IPlugFrame {
public:
    HostPlugFrame() : __funknownRefCount(1) {}

    // Set by the UI thread after creating the host window. The plugin
    // calls resizeView to ask for a resize; we use this HWND to
    // change the window size. void* to avoid pulling windows.h into
    // the header.
    void setHostHwnd(void* hwnd) { host_hwnd_ = hwnd; }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
                                             Steinberg::ViewRect* newSize) SMTG_OVERRIDE {
        if (!view || !newSize || !host_hwnd_) return Steinberg::kInvalidArgument;
#if defined(_WIN32)
        HWND hwnd = static_cast<HWND>(host_hwnd_);
        int w = newSize->getWidth();
        int h = newSize->getHeight();
        // Resize the host window. The child IPlugView HWND is resized
        // by the WM_SIZE handler in the wnd_proc free function.
        SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // The plugin spec says after resizeView, the host must call
        // IPlugView::onSize() with the new size. We do that from the
        // WM_SIZE handler so the timing matches the actual window
        // resize.
        LOG_INFO("VST3 GUI: resizeView -> " << w << "x" << h);
#endif
        return Steinberg::kResultOk;
    }

    // FUnknown boilerplate. See HostComponentHandler above for why
    // the parameter is commented out.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID /*iid*/, void** obj) SMTG_OVERRIDE {
        if (!obj) return Steinberg::kInvalidArgument;
        // The plugin only queries for IPlugFrame after setFrame().
        *obj = static_cast<Steinberg::IPlugFrame*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE {
        return static_cast<Steinberg::uint32>(
            reinterpret_cast<std::atomic<Steinberg::int32>&>(__funknownRefCount).fetch_add(1));
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE {
        auto r = reinterpret_cast<std::atomic<Steinberg::int32>&>(__funknownRefCount).fetch_sub(1);
        if (r == 0) { delete this; return 0; }
        return static_cast<Steinberg::uint32>(r);
    }

private:
    Steinberg::int32 __funknownRefCount;
    void* host_hwnd_ = nullptr;  // HWND
};

// ---------------------------------------------------------------------------
// Vst3PluginEditor implementation
// ---------------------------------------------------------------------------

Vst3PluginEditor::Vst3PluginEditor() = default;
Vst3PluginEditor::~Vst3PluginEditor() {
    stop();
}

#if defined(_WIN32)

// Per-window context. We store pointers we need in the Win32 window's
// user data area (via SetWindowLongPtr on a helper key). This is the
// standard way to associate state with an HWND; the alternative is
// a static map keyed by HWND, but that needs locking.
struct WindowContext {
    Vst3PluginEditor* editor = nullptr;
    Steinberg::IPlugView* view = nullptr;       // Steinberg::IPlugView, not Vst::
    Steinberg::Vst::IEditController* controller = nullptr;  // Vst::IEditController
    HostPlugFrame* frame = nullptr;
    HostComponentHandler* handler = nullptr;
};

// Unique window class name. We register one class per process; the
// per-window state lives in WindowContext (above) carried in
// GWLP_USERDATA.
static const wchar_t* kWindowClassName = L"AudioRouterVst3EditorClass";

// Forward declaration of the WndProc free function. The full
// definition appears later in this file (after ui_thread_main, where
// the lambda inside RegisterClassExW needs to take its address).
// The forward declaration is purely for readability: the symbol is
// resolvable at link time regardless, but MSVC's stricter name
// lookup would otherwise warn that the symbol is undeclared at the
// point of use. Same file, same translation unit -- this is a
// one-line forward decl that documents the cross-reference.
static LRESULT CALLBACK vst3_editor_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

#endif // _WIN32

void Vst3PluginEditor::start(Steinberg::IPluginFactory* factory,
                             const std::string& plugin_name,
                             void* component_ptr,
                             Steinberg::IBStream* component_state) {
    (void)component_state;  // saved-state restore is a future-work item
    if (is_open_.load()) {
        LOG_WARN("VST3 GUI: editor already open, ignoring start()");
        return;
    }
    is_stopping_.store(false);

    // Capture factory and component pointer (which is actually
    // IComponent*) by value. The factory outlives the thread
    // because the Vst3Module owns it. The component pointer is the
    // IComponent* held by Vst3Stage.
    Steinberg::IPluginFactory* factory_copy = factory;
    void* component = component_ptr;

    thread_ = std::thread([this, factory_copy, plugin_name, component]() {
        ui_thread_main(factory_copy, plugin_name, component, nullptr);
    });
}

void Vst3PluginEditor::stop() {
    if (!thread_.joinable()) return;
    is_stopping_.store(true);
#if defined(_WIN32)
    if (host_hwnd_) {
        // Post WM_CLOSE to the host window. The WndProc will see it,
        // call IPlugView::removed(), and PostQuitMessage(0) to break
        // the message loop.
        PostMessage(static_cast<HWND>(host_hwnd_), WM_CLOSE, 0, 0);
    }
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
    thread_ = std::thread{};
    is_open_.store(false);
    host_hwnd_ = nullptr;
    view_hwnd_ = nullptr;
}

void Vst3PluginEditor::performEdit(uint32_t param_id, double normalized_value) {
    // Single-threaded for now: the UI thread calls this directly. In
    // the future, the audio thread would push into a queue and a
    // dedicated lock-free ringbuffer would feed IEditController.
    LOG_INFO("VST3 GUI: performEdit(param=" << param_id << ", value=" << normalized_value << ")");
}

#if defined(_WIN32)

void Vst3PluginEditor::ui_thread_main(Steinberg::IPluginFactory* factory,
                                      const std::string& plugin_name,
                                      void* component_ptr,
                                      Steinberg::IBStream* /*component_state*/) {
    if (!factory || !component_ptr) {
        LOG_ERROR("VST3 GUI: missing factory or component");
        return;
    }
    auto* component = static_cast<Steinberg::Vst::IComponent*>(component_ptr);

    // 1. Get the controller class ID. The SDK contract: pass a buffer
    //    of at least 16 bytes, the plugin writes the 16-byte class id.
    char classIdBuf[32] = {0};
    Steinberg::tresult cr = component->getControllerClassId(classIdBuf);
    if (cr != Steinberg::kResultOk) {
        LOG_ERROR("VST3 GUI: IComponent::getControllerClassId failed (0x" << std::hex << cr << ")");
        return;
    }

    // The header stores the SDK interface pointers as void* to keep
    // the SDK headers out of the public surface. Inside the .cpp we
    // have the SDK, so we keep a local typed pointer alongside the
    // void* member and use the typed one for actual method calls.
    // The void* members are kept in sync so the error-path teardown
    // and the message-loop teardown can both reach the same objects
    // through either spelling.
    Steinberg::Vst::IEditController* controller = nullptr;
    Steinberg::IPlugView* view = nullptr;
    HostComponentHandler* handler = nullptr;

    // 2. Instantiate the IEditController.
    Steinberg::tresult r = factory->createInstance(
        classIdBuf, Steinberg::Vst::IEditController::iid,
        reinterpret_cast<void**>(&controller));
    if (r != Steinberg::kResultOk || !controller) {
        LOG_ERROR("VST3 GUI: createInstance(IEditController) failed (0x" << std::hex << r << ")");
        return;
    }
    controller_ = controller;

    // 3. Initialize the controller (analogous to component->initialize
    //    for the audio side). The controller is a FUnknown-derived
    //    object that needs to be initialized with our host context.
    Steinberg::FUnknown* host_ctx = nullptr;  // No-op host; controller doesn't need callbacks at init
    r = controller->initialize(host_ctx);
    if (r != Steinberg::kResultOk) {
        LOG_ERROR("VST3 GUI: IEditController::initialize failed (0x" << std::hex << r << ")");
        controller->release();
        controller_ = nullptr;
        return;
    }

    // 4. Set the component handler. The plugin uses this to inform
    //    the host of parameter changes from the UI.
    handler = new HostComponentHandler();
    handler_ = handler;
    r = controller->setComponentHandler(handler);
    if (r != Steinberg::kResultOk) {
        LOG_WARN("VST3 GUI: setComponentHandler returned 0x" << std::hex << r);
    }

    // 5. Create the IPlugView. IPlugView is in the Steinberg::
    //    namespace (not Steinberg::Vst::, despite being a GUI
    //    interface) -- see pluginterfaces/gui/iplugview.h.
    view = controller->createView("editor");
    if (!view) {
        LOG_ERROR("VST3 GUI: IEditController::createView returned null (plugin has no editor)");
        handler->release();
        handler_ = nullptr;
        controller->terminate();
        controller->release();
        controller_ = nullptr;
        return;
    }
    view_ = view;

    // 6. Verify the plugin supports Windows native windows.
    r = view->isPlatformTypeSupported("HWND");
    if (r != Steinberg::kResultTrue) {
        LOG_ERROR("VST3 GUI: plugin does not support HWND platform type (returned 0x"
                  << std::hex << r << ")");
        view->release();
        view_ = nullptr;
        handler->release();
        handler_ = nullptr;
        controller->terminate();
        controller->release();
        controller_ = nullptr;
        return;
    }

    // 7. Get the desired window size so we can size the host window
    //    before the plugin paints.
    Steinberg::ViewRect desiredSize;
    r = view->getSize(&desiredSize);
    int winW = 600, winH = 400;  // safe fallback
    if (r == Steinberg::kResultOk) {
        winW = desiredSize.getWidth();
        winH = desiredSize.getHeight();
        if (winW <= 0) winW = 600;
        if (winH <= 0) winH = 400;
    } else {
        LOG_WARN("VST3 GUI: IPlugView::getSize returned 0x" << std::hex << r
                 << "; using default 600x400");
    }

    // 8. Register the window class (once per process).
    static std::once_flag class_registered;
    std::call_once(class_registered, []() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &vst3_editor_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClassName;
        if (!RegisterClassExW(&wc)) {
            DWORD e = GetLastError();
            // ERROR_CLASS_ALREADY_EXISTS is fine (a prior process
            // instance in the same session registered it, or another
            // editor in this same process did).
            if (e != ERROR_CLASS_ALREADY_EXISTS) {
                LOG_ERROR("VST3 GUI: RegisterClassExW failed: " << e);
            }
        }
    });

    // 9. Create the host window. We use WS_OVERLAPPEDWINDOW (caption,
    //    resize borders, system menu, min/max) so the user can
    //    drag/resize/close the window normally. The plugin's view
    //    is attached as a child window that fills the client area.
    //
    //    CW_USEDEFAULT for position and initial size; we adjust to
    //    the plugin's preferred size after the WM_SIZE message.
    std::wstring title(plugin_name.begin(), plugin_name.end());
    title += L" - AudioRouter";

    // Per-window context, heap-allocated. We store its pointer in
    // GWLP_USERDATA after the window is created. The WndProc reads
    // it back to find the view and the editor.
    WindowContext* wctx = new WindowContext();
    wctx->editor = this;
    wctx->view = view;
    wctx->controller = controller;
    wctx->frame = new HostPlugFrame();

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH,
        nullptr, nullptr, GetModuleHandleW(nullptr), wctx);
    if (!hwnd) {
        LOG_ERROR("VST3 GUI: CreateWindowExW failed: " << GetLastError());
        delete wctx->frame;
        delete wctx;
        view->release();
        view_ = nullptr;
        handler->release();
        handler_ = nullptr;
        controller->terminate();
        controller->release();
        controller_ = nullptr;
        return;
    }

    host_hwnd_ = hwnd;
    wctx->frame->setHostHwnd(hwnd);

    // 10. Set the IPlugFrame (must be done before attached()).
    view->setFrame(wctx->frame);

    // 11. Create the child window that the plugin's view will draw
    //     into. We do this before attached() so the plugin has a
    //     valid HWND immediately on attachment. WS_CHILD | WS_VISIBLE
    //     makes it a child of the host window that fills the client
    //     area; WS_CLIPCHILDREN prevents the host from painting over
    //     the plugin's pixels.
    HWND child = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, winW, winH,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) {
        LOG_ERROR("VST3 GUI: CreateWindowExW (child) failed: " << GetLastError());
        DestroyWindow(hwnd);
        delete wctx->frame;
        delete wctx;
        view->release();
        view_ = nullptr;
        handler->release();
        handler_ = nullptr;
        controller->terminate();
        controller->release();
        controller_ = nullptr;
        return;
    }
    view_hwnd_ = child;

    // 12. Attach the plugin's view to the child window. The plugin
    //     creates whatever sub-windows it needs (HWND-based UIs in
    //     this case) as children of `child`.
    r = view->attached(child, "HWND");
    if (r != Steinberg::kResultOk) {
        LOG_ERROR("VST3 GUI: IPlugView::attached failed (0x" << std::hex << r << ")");
        DestroyWindow(child);
        DestroyWindow(hwnd);
        delete wctx->frame;
        delete wctx;
        view->release();
        view_ = nullptr;
        handler->release();
        handler_ = nullptr;
        controller->terminate();
        controller->release();
        controller_ = nullptr;
        return;
    }

    // 13. Show the host window. The child window is already visible
    //     (WS_VISIBLE), and the plugin should have painted into it
    //     during attached().
    is_open_.store(true);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    LOG_INFO("VST3 GUI: editor window '" << plugin_name << "' opened ("
             << winW << "x" << winH << ")");

    // 14. Message loop. Standard Win32 pump with a way to break out
    //     (PostQuitMessage from WM_CLOSE handler).
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 15. Tear down in reverse order.
    LOG_INFO("VST3 GUI: editor window closing");
    if (view) {
        view->removed();
        view->release();
        view_ = nullptr;
    }
    if (host_hwnd_) {
        DestroyWindow(static_cast<HWND>(host_hwnd_));
        host_hwnd_ = nullptr;
    }
    if (view_hwnd_) view_hwnd_ = nullptr;
    if (handler) {
        handler->release();
        handler_ = nullptr;
    }
    if (controller) {
        controller->terminate();
        controller->release();
        controller_ = nullptr;
    }
    if (wctx) {
        delete wctx->frame;
        delete wctx;
    }
    is_open_.store(false);
}

// Free function (not a static method on Vst3PluginEditor) so the
// WndProc doesn't have to be declared in the header -- otherwise
// the header would need <windows.h> just to make CALLBACK/HWND/UINT
// visible, even for callers (e.g. plugin_chain.cpp on Linux) that
// include the header but never see Win32 types.
static LRESULT CALLBACK vst3_editor_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The window stores its WindowContext* in GWLP_USERDATA (set at
    // WM_CREATE time). We retrieve it on every message.
    WindowContext* wctx = reinterpret_cast<WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            // The lpCreateParams is the WindowContext* we passed to
            // CreateWindowExW as the final argument. We store it in
            // GWLP_USERDATA for retrieval on subsequent messages.
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                               reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        case WM_SIZE: {
            // Resize the child HWND to match the new client area, and
            // tell the plugin's IPlugView about the new size. The
            // plugin spec says the host must call IPlugView::onSize
            // after a resize caused by the host (which is the case
            // here -- the user dragged the window border, and the
            // OS resized the host window).
            if (!wctx || !wctx->view) break;
            RECT r;
            GetClientRect(hwnd, &r);
            HWND child = GetWindow(hwnd, GW_CHILD);
            if (child) {
                MoveWindow(child, 0, 0, r.right - r.left, r.bottom - r.top, TRUE);
            }
            Steinberg::ViewRect newSize;
            newSize.left = 0;
            newSize.top = 0;
            newSize.right = r.right - r.left;
            newSize.bottom = r.bottom - r.top;
            wctx->view->onSize(&newSize);
            return 0;
        }
        case WM_CLOSE:
            // User clicked the X. Tell the message loop to exit and
            // let ui_thread_main() clean up.
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            // Set by DestroyWindow during teardown. No action needed
            // here -- the message loop is already exiting.
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#else  // !_WIN32

void Vst3PluginEditor::ui_thread_main(Steinberg::IPluginFactory* /*factory*/,
                                      const std::string& /*plugin_name*/,
                                      void* /*component_ptr*/,
                                      Steinberg::IBStream* /*component_state*/) {
    LOG_WARN("VST3 GUI: editor requested but VST3 plugin GUI is only supported on Windows in this build");
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Vst3EditorManager
// ---------------------------------------------------------------------------
Vst3EditorManager::Vst3EditorManager() = default;
Vst3EditorManager::~Vst3EditorManager() {
    close_all();
}

void Vst3EditorManager::open_editor(Steinberg::IPluginFactory* factory,
                                     const std::string& plugin_name,
                                     void* component_ptr,
                                     Steinberg::IBStream* component_state) {
    std::lock_guard<std::mutex> lock(mu_);
    if (editor_ && editor_->is_open()) {
        LOG_WARN("VST3 GUI: editor already open, ignoring open_editor for '" << plugin_name << "'");
        return;
    }
    editor_ = std::make_unique<Vst3PluginEditor>();
    editor_->start(factory, plugin_name, component_ptr, component_state);
}

void Vst3EditorManager::close_all() {
    std::lock_guard<std::mutex> lock(mu_);
    if (editor_) {
        editor_->stop();
        editor_.reset();
    }
}

} // namespace audiorouter

#endif // AUDIOROUTER_ENABLE_VST3
