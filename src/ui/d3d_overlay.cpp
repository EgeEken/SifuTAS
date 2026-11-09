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
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
void* g_present_target = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
IDXGISwapChain* g_chain = nullptr;
HWND g_window = nullptr;
WNDPROC g_old_proc = nullptr;
bool g_ready = false;
bool g_failed = false;
std::atomic<bool> g_open{false};
std::atomic<int> g_capture_slot{-1};
std::atomic<bool> g_ignore_capture_click{false};
std::atomic<bool> g_ignore_mouse_until_release{false};
std::atomic<int> g_capture_result{0};
ImGuiStyle g_base_style = {};
std::atomic<float> g_dpi_scale{1.f};

LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);

unsigned int Modifiers() {
    unsigned int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= tas::ModifierCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= tas::ModifierAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= tas::ModifierShift;
    return mods;
}

bool CaptureBinding(int vk) {
    const int slot = g_capture_slot;
    if (slot < 0 || slot >= 9) return false;
    g_capture_slot = -1;
    const bool accepted = tas::SetBinding(slot, {vk, Modifiers()});
    g_capture_result.store(accepted ? 1 : (vk == VK_F1 ? 3 : 2), std::memory_order_relaxed);
    return accepted;
}

void FeedMouse() {
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

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DPICHANGED) g_dpi_scale.store(static_cast<float>(HIWORD(wparam)) / 96.f, std::memory_order_relaxed);
    if (!g_open.load(std::memory_order_relaxed)) return CallWindowProcW(g_old_proc, window, message, wparam, lparam);
    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    if (g_capture_slot >= 0) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lparam & (1ll << 30))) {
            const int key = static_cast<int>(wparam);
            if (key == VK_ESCAPE) { g_capture_slot = -1; g_capture_result.store(0); }
            else if (!(key >= VK_SHIFT && key <= VK_MENU)) CaptureBinding(key);
        } else if (message == WM_LBUTTONUP) {
            if (g_ignore_capture_click) g_ignore_capture_click = false;
            else if (!g_ignore_mouse_until_release) CaptureBinding(VK_LBUTTON);
        } else if (message == WM_RBUTTONUP && !g_ignore_mouse_until_release) CaptureBinding(VK_RBUTTON);
        else if (message == WM_MBUTTONUP && !g_ignore_mouse_until_release) CaptureBinding(VK_MBUTTON);
        else if (message == WM_XBUTTONUP && !g_ignore_mouse_until_release)
            CaptureBinding(HIWORD(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
    }
    if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP)
        g_ignore_mouse_until_release = false;
    if (message == WM_INPUT) return DefWindowProcW(window, message, wparam, lparam);
    if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_CHAR ||
        message == WM_SYSCHAR)
        return 0;
    return CallWindowProcW(g_old_proc, window, message, wparam, lparam);
}

bool CreateTarget(IDXGISwapChain* chain) {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return false;
    const HRESULT result = g_device->CreateRenderTargetView(buffer, nullptr, &g_target);
    buffer->Release();
    return SUCCEEDED(result);
}

bool Initialise(IDXGISwapChain* chain) {
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) return false;
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc = {};
    chain->GetDesc(&desc);
    g_window = desc.OutputWindow;
    HDC dc = GetDC(g_window);
    if (dc) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0) g_dpi_scale.store(static_cast<float>(dpi) / 96.f, std::memory_order_relaxed);
        ReleaseDC(g_window, dc);
    }
    if (!CreateTarget(chain)) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.f;
    style.ChildRounding = 8.f;
    style.FrameRounding = 5.f;
    style.GrabRounding = 5.f;
    style.WindowBorderSize = 0.f;
    style.ChildBorderSize = 1.f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.04f, 0.06f, 0.70f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.065f, 0.085f, 0.84f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.25f, 0.31f, 0.72f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.17f, 0.23f, 0.95f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.35f, 0.48f, 1.f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.48f, 0.68f, 1.f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.18f, 0.58f, 0.82f, 1.f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.28f, 0.72f, 0.94f, 1.f);
    g_base_style = style;
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context)) return false;
    g_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(&WindowProc)));
    SC_LOG("time-control: fullscreen GUI initialized on game swap chain");
    return g_old_proc != nullptr;
}

void DrawKeybinds(float width) {
    const auto settings = tas::GetSettings();
    ImGui::Text("Keybinds");
    ImGui::Spacing();
    for (int slot = 0; slot < 9; ++slot) {
        ImGui::PushID(slot);
        char label[48] = {};
        if (g_capture_slot == slot) strcpy_s(label, "Press a key...");
        else if (!tas::BindingText(settings.bindings[slot], label, sizeof(label))) strcpy_s(label, "Key");
        const float scale = fminf(1.1f, fminf(g_dpi_scale.load(std::memory_order_relaxed),
            fminf(ImGui::GetIO().DisplaySize.x / 1600.f, ImGui::GetIO().DisplaySize.y / 820.f)));
        const float key_width = (width < 360.f ? 88.f : 118.f) * scale;
        if (ImGui::Button(label, ImVec2(key_width, 33.f * scale))) {
            g_capture_slot = slot;
            const bool ignore_click = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            g_ignore_capture_click.store(ignore_click, std::memory_order_relaxed);
            g_ignore_mouse_until_release.store(ignore_click, std::memory_order_relaxed);
            g_capture_result.store(0);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to rebind; Escape cancels");
        ImGui::SameLine();
        if (ImGui::SmallButton("×")) {
            if (tas::SetBinding(slot, {0, 0})) g_capture_result.store(1);
            else g_capture_result.store(2);
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(tas::SlotActionText(slot));
        const int preset = tas::PresetIndexForSlot(slot);
        if (preset >= 0) {
            int step = settings.preset_steps[preset];
            const float slider_width = (width - key_width - 75.f * scale) > 100.f * scale
                ? width - key_width - 75.f * scale : 100.f * scale;
            ImGui::SetNextItemWidth(slider_width);
            char slider_label[32] = {};
            _snprintf_s(slider_label, sizeof(slider_label), _TRUNCATE, "##preset%d", preset);
            if (ImGui::SliderInt(slider_label, &step, 1, 20, "%d")) tas::SetPresetStep(preset, step);
            if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
            ImGui::SameLine();
            char apply[24] = {};
            _snprintf_s(apply, sizeof(apply), _TRUNCATE, "%.1f× Apply##apply%d", step / 10.f, preset);
            if (ImGui::SmallButton(apply)) hooks::SetPlaybackSpeed(step / 10.f);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Spacing();
    }
    bool show = settings.show_speed_indicator;
    if (ImGui::Checkbox("Show speed indicator", &show)) tas::SetShowSpeedIndicator(show);
    if (!tas::SettingsSaved()) ImGui::TextColored(ImVec4(1.f, .55f, .35f, 1.f), "Settings could not be saved beside the mod.");
    const int result = g_capture_result.load(std::memory_order_relaxed);
    if (result == 2) ImGui::TextColored(ImVec4(1.f, .55f, .35f, 1.f), "That key combination is already assigned.");
    else if (result == 3) ImGui::TextColored(ImVec4(1.f, .55f, .35f, 1.f), "F1 is reserved for the GUI.");
    else if (result == 1) ImGui::TextDisabled("Binding saved.");
}

void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    const float scale = fminf(1.1f, fminf(g_dpi_scale.load(std::memory_order_relaxed), fminf(screen.x / 1600.f, screen.y / 820.f)));
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(screen);
    ImGui::SetNextWindowBgAlpha(0.f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##SifuTASFullscreen", nullptr, flags);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(4, 7, 12, 115));
    const float outer = 36.f * scale;
    const float width = (screen.x - outer * 2.f - 32.f * scale) / 3.f;
    const float height = screen.y - outer * 2.f;
    ImGui::SetCursorPos(ImVec2(outer, outer));
    if (ImGui::BeginChild("##pause", ImVec2(width, height), ImGuiChildFlags_Border)) {
        const float button_h = 58.f * scale;
        ImGui::SetCursorPosY(height * .25f);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Button(hooks::IsPaused() ? ">  Resume" : "||  Pause", ImVec2(-1.f, button_h)))
            hooks::SetPaused(!hooks::IsPaused());
        ImGui::Spacing();
        ImGui::BeginDisabled(!hooks::IsPaused());
        if (ImGui::Button(">  Advance 1 Frame", ImVec2(-1.f, button_h))) hooks::AdvanceFrames(1);
        ImGui::Spacing();
        const int frames = tas::GetSettings().frames_per_step;
        char label[48] = {};
        _snprintf_s(label, sizeof(label), _TRUNCATE, ">>  Advance %d Frames", frames);
        if (ImGui::Button(label, ImVec2(-1.f, button_h))) hooks::AdvanceFrames(frames);
        ImGui::EndDisabled();
        int next_frames = frames;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("##multiFrameStep", &next_frames, 2, 10, "Step count: %d")) tas::SetFramesPerStep(next_frames);
        if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    }
    ImGui::EndChild();
    ImGui::SameLine(0.f, 16.f * scale);
    if (ImGui::BeginChild("##speed", ImVec2(width, height), ImGuiChildFlags_Border)) {
        ImGui::Text("Game Speed");
        ImGui::Spacing();
        ImGui::BeginDisabled(!hooks::IsGameplaySupported());
        int step = tas::SpeedStep(hooks::PlaybackSpeed());
        ImGui::SetCursorPosY(height * .34f);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("##speedSlider", &step, 1, 20, "%d")) hooks::SetPlaybackSpeed(step / 10.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.f);
        ImGui::Text("%.1f×", hooks::PlaybackSpeed());
        ImGui::Spacing();
        const float button_width = 220.f * scale < width - 32.f * scale ? 220.f * scale : width - 32.f * scale;
        if (ImGui::Button("Reset to 1.0×", ImVec2(button_width, 42.f * scale))) hooks::SetPlaybackSpeed(1.f);
        ImGui::EndDisabled();
        if (!hooks::IsGameplaySupported()) ImGui::TextDisabled("Available during active gameplay.");
    }
    ImGui::EndChild();
    ImGui::SameLine(0.f, 16.f * scale);
    if (ImGui::BeginChild("##keybinds", ImVec2(width, height), ImGuiChildFlags_Border)) DrawKeybinds(width - 32.f * scale);
    ImGui::EndChild();
    ImGui::End();
}

void DrawIndicator() {
    const auto settings = tas::GetSettings();
    if (!settings.show_speed_indicator || GetTickCount64() >= hooks::SpeedIndicatorUntil()) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x - 32.f, 28.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(.45f);
    ImGui::Begin("##speedIndicator", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("%.1f×", hooks::PlaybackSpeed());
    ImGui::End();
}

bool FindPresent(void** resize) {
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SifuTASProbe";
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;
    DXGI_SWAP_CHAIN_DESC desc = {}; desc.BufferCount = 1; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = window;
    desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* chain = nullptr; ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, &context);
    if (SUCCEEDED(result) && chain) { void** vtable = *reinterpret_cast<void***>(chain); *resize = vtable[13]; g_present_target = vtable[8]; }
    if (chain) chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_present_target && *resize;
}

HRESULT __stdcall ResizeHook(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (chain == g_chain && g_target) { g_target->Release(); g_target = nullptr; }
    return g_resize(chain, count, width, height, format, flags);
}

HRESULT __stdcall PresentHook(IDXGISwapChain* chain, UINT interval, UINT flags) {
    if (!g_failed && !g_ready) {
        g_ready = Initialise(chain);
        g_failed = !g_ready;
        if (g_ready) g_chain = chain;
    }
    if (chain == g_chain && g_ready) {
        if (!g_target && !CreateTarget(chain)) return g_present(chain, interval, flags);
        const float scale = g_open.load(std::memory_order_relaxed)
            ? fminf(1.1f, fminf(g_dpi_scale.load(std::memory_order_relaxed),
                fminf(ImGui::GetIO().DisplaySize.x / 1600.f, ImGui::GetIO().DisplaySize.y / 820.f))) : 1.f;
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetIO().FontGlobalScale = 1.4f * scale;
        if (g_open.load(std::memory_order_relaxed) || GetTickCount64() < hooks::SpeedIndicatorUntil()) {
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
            if (g_open.load(std::memory_order_relaxed)) FeedMouse();
            if (GetForegroundWindow() == g_window && g_open.load(std::memory_order_relaxed))
                ImGui::GetIO().MouseDrawCursor = true;
            else ImGui::GetIO().MouseDrawCursor = false;
            ImGui::NewFrame();
            if (g_open.load(std::memory_order_relaxed)) DrawMenu(); else DrawIndicator();
            ImGui::Render(); g_context->OMSetRenderTargets(1, &g_target, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    return g_present(chain, interval, flags);
}

}

bool StartInGameOverlay() {
    void* resize = nullptr;
    if (!FindPresent(&resize)) { SC_LOG("time-control: D3D11 swap-chain probe failed"); return false; }
    MH_Initialize();
    if (MH_CreateHook(g_present_target, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&g_present)) != MH_OK ||
        MH_EnableHook(g_present_target) != MH_OK) return false;
    if (MH_CreateHook(resize, reinterpret_cast<void*>(&ResizeHook), reinterpret_cast<void**>(&g_resize)) != MH_OK ||
        MH_EnableHook(resize) != MH_OK) {
        MH_DisableHook(g_present_target);
        MH_RemoveHook(g_present_target);
        g_present = nullptr;
        return false;
    }
    SC_LOG("time-control: D3D11 GUI renderer hooked");
    return true;
}

bool IsMenuOpen() { return g_open.load(std::memory_order_relaxed); }
bool IsBindingCaptureActive() { return g_capture_slot >= 0; }
void SetMenuOpen(bool open) { g_open.store(open, std::memory_order_relaxed); SC_LOG("time-control: GUI %s", open ? "opened" : "closed"); }

}
