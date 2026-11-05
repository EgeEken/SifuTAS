#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "../../third_party/imgui/backends/imgui_impl_dx11.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/minhook/include/MinHook.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../tas/controls.h"
#include "overlay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace sifucoop::ui {
namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                            UINT);

PresentFn g_original_present = nullptr;
ResizeBuffersFn g_original_resize_buffers = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
HWND g_window = nullptr;
WNDPROC g_original_wndproc = nullptr;
bool g_initialised = false;
bool g_failed = false;
std::atomic<bool> g_menu_open{false};
std::atomic<int> g_rebind_action{-1};
std::atomic<int> g_settings_status{0};

LRESULT CALLBACK WndProcHook(HWND, UINT, WPARAM, LPARAM);

bool CreateRenderTarget(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&back_buffer))) || !back_buffer)
        return false;
    const HRESULT result = g_device->CreateRenderTargetView(back_buffer, nullptr,
                                                             &g_render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

bool InitialiseFrom(IDXGISwapChain* swap_chain) {
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                                     reinterpret_cast<void**>(&g_device)))) {
        SC_LOG("d3d: swap chain is not D3D11 -- TAS GUI unavailable");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc = {};
    swap_chain->GetDesc(&desc);
    g_window = desc.OutputWindow;
    if (!CreateRenderTarget(swap_chain)) {
        SC_LOG("d3d: render target creation failed -- TAS GUI unavailable");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        SC_LOG("d3d: ImGui backend initialization failed -- TAS GUI unavailable");
        return false;
    }
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook)));
    SC_LOG("d3d: SifuTAS GUI ready; F6 opens it, game cursor hooks remain disabled");
    return true;
}

void CaptureBinding(int key) {
    const int action = g_rebind_action.exchange(-1, std::memory_order_relaxed);
    if (action < 0) return;
    unsigned int modifiers = tas::ModifierNone;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= tas::ModifierCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= tas::ModifierAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= tas::ModifierShift;
    if (tas::SetBinding(static_cast<tas::Action>(action), {key, modifiers})) {
        g_settings_status.store(1, std::memory_order_relaxed);
    } else {
        g_settings_status.store(2, std::memory_order_relaxed);
    }
}

LRESULT CALLBACK WndProcHook(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!g_menu_open.load(std::memory_order_relaxed))
        return CallWindowProcW(g_original_wndproc, window, message, wparam, lparam);

    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        g_rebind_action.load(std::memory_order_relaxed) >= 0) {
        if (wparam == VK_ESCAPE) {
            g_rebind_action.store(-1, std::memory_order_relaxed);
            return 0;
        }
        if (wparam >= VK_F1 && wparam <= VK_F12) {
            CaptureBinding(static_cast<int>(wparam));
            return 0;
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_message = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    const bool keyboard_message = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    const bool capture_mouse = mouse_message && io.WantCaptureMouse;
    const bool capture_keyboard = keyboard_message && (io.WantCaptureKeyboard || io.WantTextInput);
    const bool capture_raw = message == WM_INPUT &&
                             (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput);
    if (capture_mouse || capture_keyboard || capture_raw) return 0;
    return CallWindowProcW(g_original_wndproc, window, message, wparam, lparam);
}

void DrawPlaybackTab() {
    const tas::Settings settings = tas::GetSettings();
    if (ImGui::Button(hooks::IsPaused() ? "Resume" : "Pause", ImVec2(140, 0)))
        hooks::ActivateAction(tas::Action::PauseResume);
    ImGui::SameLine();
    ImGui::BeginDisabled(!hooks::IsPaused());
    if (ImGui::Button("Advance", ImVec2(140, 0)))
        hooks::ActivateAction(tas::Action::FrameAdvance);
    ImGui::EndDisabled();

    ImGui::Text("Playback speed: %.3fx", hooks::PlaybackSpeed());
    ImGui::TextDisabled("Select a speed; selecting it again toggles back to 1x.");
    const tas::Action speed_actions[] = {tas::Action::SpeedHalf, tas::Action::SpeedFifth,
                                         tas::Action::SpeedTenth};
    for (int i = 0; i < 3; ++i) {
        char label[48] = {};
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%.3gx", settings.speeds[i]);
        if (i) ImGui::SameLine();
        if (ImGui::Button(label, ImVec2(112, 0))) hooks::ActivateAction(speed_actions[i]);
    }
    ImGui::SameLine();
    if (ImGui::Button("1x", ImVec2(72, 0))) hooks::SetPlaybackSpeed(1.f);

    ImGui::Spacing();
    int frames = settings.frames_per_step;
    if (ImGui::SliderInt("Frames per advance", &frames, 1, 10))
        tas::SetFramesPerStep(frames);
    if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    ImGui::Text("Engine tick calls: %llu", hooks::FrameCount());
    ImGui::TextDisabled("This is not a verified simulation-frame count.");
}

void DrawSavestatesTab() {
    ImGui::TextDisabled("Checkpoint save/load is not implemented yet.");
    ImGui::Separator();
    for (int i = 1; i <= 5; ++i) {
        ImGui::Text("Slot %d", i);
        ImGui::SameLine(100.f);
        ImGui::TextDisabled("Unavailable");
    }
}

void DrawRecordingTab() {
    ImGui::TextDisabled("Input recording and playback are not implemented yet.");
    ImGui::Separator();
    ImGui::TextDisabled("No recording controls are active.");
}

void DrawSettingsTab() {
    tas::Settings settings = tas::GetSettings();
    ImGui::TextDisabled("Function-key shortcuts support Ctrl, Alt, and Shift.");
    for (int i = 0; i < static_cast<int>(tas::Action::Count); ++i) {
        const auto action = static_cast<tas::Action>(i);
        char shortcut[48] = {};
        tas::BindingText(settings.bindings[i], shortcut, sizeof(shortcut));
        ImGui::TextUnformatted(tas::ActionName(action));
        ImGui::SameLine(225.f);
        if (g_rebind_action.load(std::memory_order_relaxed) == i) {
            ImGui::TextDisabled("Press F1-F12 (Esc cancels)");
        } else {
            ImGui::TextDisabled("%s", shortcut);
            ImGui::SameLine(390.f);
            char button[32] = {};
            _snprintf_s(button, sizeof(button), _TRUNCATE, "Rebind##%d", i);
            if (ImGui::SmallButton(button)) g_rebind_action.store(i, std::memory_order_relaxed);
        }
    }
    ImGui::TextDisabled("Checkpoint shortcuts are reserved and inactive.");
    ImGui::Separator();
    for (int i = 0; i < 3; ++i) {
        char label[32] = {};
        _snprintf_s(label, sizeof(label), _TRUNCATE, "Speed %d multiplier", i + 1);
        float speed = settings.speeds[i];
        if (ImGui::SliderFloat(label, &speed, 0.01f, 2.f, "%.2fx"))
            tas::SetSpeed(i, speed);
        if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    }
    int frames = settings.frames_per_step;
    if (ImGui::SliderInt("Frames per advance", &frames, 1, 10))
        tas::SetFramesPerStep(frames);
    if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    if (!tas::SettingsSaved())
        ImGui::TextWrapped("Settings could not be saved beside dsound.dll. Check folder permissions.");
    const int status = g_settings_status.load(std::memory_order_relaxed);
    if (status == 1) ImGui::TextDisabled("Binding applied.");
    if (status == 2) ImGui::TextDisabled("That shortcut is already assigned.");
}

void DrawMenu() {
    const tas::Settings settings = tas::GetSettings();
    ImGui::SetNextWindowSize(ImVec2(680, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (!ImGui::Begin("SifuTAS", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("sifutas-tabs")) {
        if (ImGui::BeginTabItem("Playback")) {
            DrawPlaybackTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Savestates")) {
            DrawSavestatesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recording")) {
            DrawRecordingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            DrawSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    char close_binding[48] = {};
    tas::BindingText(settings.bindings[static_cast<int>(tas::Action::ToggleGui)],
                     close_binding, sizeof(close_binding));
    ImGui::TextDisabled("%s closes the GUI. Simulation keeps responding to pause and step controls.",
                        close_binding);
    ImGui::End();
}

void FeedMenuInput() {
    ImGuiIO& io = ImGui::GetIO();
    if (GetForegroundWindow() != g_window) {
        io.AddMouseButtonEvent(0, false);
        io.AddMouseButtonEvent(1, false);
        return;
    }
    POINT cursor = {};
    if (GetCursorPos(&cursor) && ScreenToClient(g_window, &cursor))
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
}

void ReleaseRenderTarget() {
    if (g_render_target) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

HRESULT __stdcall ResizeBuffersHook(IDXGISwapChain* swap_chain, UINT count, UINT width,
                                    UINT height, DXGI_FORMAT format, UINT flags) {
    if (swap_chain == g_swap_chain) ReleaseRenderTarget();
    return g_original_resize_buffers(swap_chain, count, width, height, format, flags);
}

HRESULT __stdcall PresentHook(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (!g_failed && !g_initialised) {
        g_initialised = InitialiseFrom(swap_chain);
        if (!g_initialised) g_failed = true;
        if (g_initialised) g_swap_chain = swap_chain;
    }
    if (swap_chain != g_swap_chain) return g_original_present(swap_chain, sync_interval, flags);
    if (g_initialised && !g_render_target && g_device && !CreateRenderTarget(swap_chain))
        return g_original_present(swap_chain, sync_interval, flags);
    if (g_initialised && g_render_target) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().MouseDrawCursor = g_menu_open.load(std::memory_order_relaxed);
        if (g_menu_open.load(std::memory_order_relaxed)) FeedMenuInput();
        ImGui::NewFrame();
        if (g_menu_open.load(std::memory_order_relaxed)) DrawMenu();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_original_present(swap_chain, sync_interval, flags);
}

void* FindPresent(void** out_resize_buffers) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SifuTASProbe";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) return nullptr;
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = dummy;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &desc, &swap_chain, &device, nullptr, &context);
    void* present = nullptr;
    if (SUCCEEDED(result) && swap_chain) {
        void** vtable = *reinterpret_cast<void***>(swap_chain);
        present = vtable[8];
        *out_resize_buffers = vtable[13];
    } else {
        SC_LOG("d3d: probe swap chain creation failed (0x%08lX)", result);
    }
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return present;
}

}

bool StartInGameOverlay() {
    void* resize_buffers = nullptr;
    void* present = FindPresent(&resize_buffers);
    if (!present) return false;
    MH_Initialize();
    if (MH_CreateHook(present, reinterpret_cast<void*>(&PresentHook),
                      reinterpret_cast<void**>(&g_original_present)) != MH_OK ||
        MH_EnableHook(present) != MH_OK) {
        SC_LOG("d3d: Present hook failed -- TAS GUI unavailable");
        return false;
    }
    if (!resize_buffers ||
        MH_CreateHook(resize_buffers, reinterpret_cast<void*>(&ResizeBuffersHook),
                      reinterpret_cast<void**>(&g_original_resize_buffers)) != MH_OK ||
        MH_EnableHook(resize_buffers) != MH_OK) {
        SC_LOG("d3d: ResizeBuffers hook failed -- TAS GUI unavailable");
        MH_DisableHook(present);
        return false;
    }
    SC_LOG("d3d: Present + ResizeBuffers hooked for SifuTAS GUI");
    return true;
}

bool TakeMenuRequests(MenuRequests*) { return false; }
void SetMenuStatus(const MenuStatus&) {}
void SetTailnetRows(const TailnetRow*, int) {}
void SetSyncRows(const SyncRow*, int) {}
bool IsMenuOpen() { return g_menu_open.load(std::memory_order_relaxed); }
void ToggleMenu() { SetMenuOpen(!IsMenuOpen()); }
void SetMenuOpen(bool open) {
    g_menu_open.store(open, std::memory_order_relaxed);
    if (!open) g_rebind_action.store(-1, std::memory_order_relaxed);
    SC_LOG("tas: GUI %s", open ? "opened" : "closed");
}
bool IsBindingCaptureActive() { return g_rebind_action.load(std::memory_order_relaxed) >= 0; }
void SetInGameOverlayText(const char*) {}

}
