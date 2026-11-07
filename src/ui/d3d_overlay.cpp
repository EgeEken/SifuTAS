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
#include "../tas/checkpoint.h"
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
ImGuiStyle g_base_style = {};
float g_applied_scale = -1.f;
bool g_scale_changed = false;
LARGE_INTEGER g_ui_perf_frequency = {};
ULONGLONG g_ui_perf_last_ms = 0;
ULONGLONG g_ui_perf_total = 0;
ULONGLONG g_ui_perf_max = 0;
ULONGLONG g_ui_perf_count = 0;

LRESULT CALLBACK WndProcHook(HWND, UINT, WPARAM, LPARAM);

void ApplyGuiScale() {
    const float scale = tas::GetSettings().gui_scale;
    if (std::fabs(scale - g_applied_scale) < 0.001f) return;
    ImGui::GetStyle() = g_base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
    g_applied_scale = scale;
    g_scale_changed = true;
}

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
    QueryPerformanceFrequency(&g_ui_perf_frequency);
    g_ui_perf_last_ms = GetTickCount64();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    g_base_style = ImGui::GetStyle();
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        SC_LOG("d3d: ImGui backend initialization failed -- TAS GUI unavailable");
        return false;
    }
    ApplyGuiScale();
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook)));
    SC_LOG("d3d: SifuTAS GUI ready; F6 opens it, game cursor hooks remain disabled");
    return true;
}

void CaptureBinding(int key) {
    tas::ConsumeBindingKeyUntilRelease(key);
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
    if (message == WM_KEYUP || message == WM_SYSKEYUP)
        tas::ReleaseBindingKey(static_cast<int>(wparam));
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
    ImGui::BeginDisabled(!hooks::IsPaused());
    if (ImGui::Button("Frame advance", ImVec2(220.f * settings.gui_scale, 0)))
        hooks::ActivateAction(tas::Action::FrameAdvance);
    ImGui::EndDisabled();

    float speed = hooks::PlaybackSpeed();
    if (ImGui::SliderFloat("Playback speed", &speed, 0.01f, 4.f, "%.2fx"))
        hooks::SetPlaybackSpeed(speed);
    ImGui::Text("Current speed: %.3fx", hooks::PlaybackSpeed());
    ImGui::TextDisabled("F3-F5 select the configured speed presets.");

    ImGui::Spacing();
    int frames = settings.frames_per_step;
    if (ImGui::SliderInt("Frames per advance", &frames, 1, 10))
        tas::SetFramesPerStep(frames);
    if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    ImGui::Text("Engine tick calls: %llu", hooks::FrameCount());
    ImGui::TextDisabled("This is not a verified simulation-frame count.");
}

void DrawSavestatesTab() {
    const tas::checkpoint::Status status = tas::checkpoint::GetStatus();
    ImGui::TextDisabled("One in-memory checkpoint. Combat AI, actions, and timers remain live.");
    if (!ImGui::CollapsingHeader("Advanced / diagnostics")) return;
    ImGui::Text("World actors: %u; active candidates: %u; captured: %u; capacity: 32",
                status.world_actor_count, status.enumerated_count, status.captured_count);
    ImGui::Text("Excluded: pooled %u, invalid %u, missing health %u, inactive %u, identity too long %u / unavailable %u, capacity %u",
                status.pooled_count, status.invalid_count, status.missing_health_count,
                status.inactive_count, status.identity_truncated_count,
                status.identity_unavailable_count, status.capacity_skipped_count);
    ImGui::Text("Restore: fields complete %u, partial %u, skipped %u; snapshot %s",
                status.restored_count, status.partial_count, status.skipped_count,
                status.incomplete ? "incomplete" : "complete for supported fields");
    if (!status.exists) {
        ImGui::TextDisabled("No checkpoint is saved.");
        return;
    }
    ImGui::TextWrapped("Level: %s", status.reference.level_path);
    ImGui::Text("Saved engine tick: %llu (simulation frame unverified)",
                static_cast<unsigned long long>(status.reference.engine_tick));
    ImGui::Text("Camera: %s; last restore: %s; cloth reset queued: %u",
                status.camera_available ? "captured" : "unavailable",
                status.camera_restored ? "verified" : "not verified",
                status.cloth_reset_count);
    ImGui::TextDisabled("Cloth particle history, blend-tree pose, combat actions, AI, timers, and callbacks are not restored.");
    for (std::uint32_t i = 0; i < status.actor_count; ++i) {
        const auto& actor = status.actors[i];
        ImGui::TextWrapped("%s: %s", actor.player ? "Player" : "Actor", actor.identity);
        ImGui::Text("  fields captured=%03X restored=%03X failed=%03X unsupported=%03X unavailable=%03X%s",
                    actor.captured_fields, actor.restored_fields, actor.failed_fields,
                    actor.unsupported_fields, actor.unavailable_fields,
                    actor.skipped ? " skipped" : "");
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
        ImGui::SameLine();
        if (g_rebind_action.load(std::memory_order_relaxed) == i) {
            ImGui::TextDisabled("Press F1-F12 (Esc cancels)");
        } else {
            ImGui::TextDisabled("%s", shortcut);
            ImGui::SameLine();
            char button[32] = {};
            _snprintf_s(button, sizeof(button), _TRUNCATE, "Rebind##%d", i);
            if (ImGui::SmallButton(button)) g_rebind_action.store(i, std::memory_order_relaxed);
        }
    }
    ImGui::TextDisabled("F7 saves and F8 loads the in-memory checkpoint.");
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
    float gui_scale = settings.gui_scale;
    if (ImGui::SliderFloat("GUI scale", &gui_scale, 0.8f, 2.f, "%.2fx"))
        tas::SetGuiScale(gui_scale);
    if (ImGui::IsItemDeactivatedAfterEdit()) tas::SaveSettings();
    if (!tas::SettingsSaved())
        ImGui::TextWrapped("Settings could not be saved beside dsound.dll. Check folder permissions.");
    const int status = g_settings_status.load(std::memory_order_relaxed);
    if (status == 1) ImGui::TextDisabled("Binding applied.");
    if (status == 2) ImGui::TextDisabled("That shortcut is already assigned.");
}

void DrawMenu() {
    const tas::Settings settings = tas::GetSettings();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    float width = 680.f * settings.gui_scale;
    float height = 500.f * settings.gui_scale;
    if (display.x > 80.f && width > display.x - 40.f) width = display.x - 40.f;
    if (display.y > 80.f && height > display.y - 40.f) height = display.y - 40.f;
    ImGui::SetNextWindowSize(ImVec2(width, height),
                             g_scale_changed ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    g_scale_changed = false;
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (!ImGui::Begin("SifuTAS", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const float scale = settings.gui_scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = (ImGui::GetContentRegionAvail().x - spacing * 2.f) / 3.f;
    if (ImGui::Button(hooks::IsPaused() ? "Resume" : "Pause",
                      ImVec2(button_width, 38.f * scale)))
        hooks::ActivateAction(tas::Action::PauseResume);
    ImGui::SameLine();
    if (ImGui::Button("Save checkpoint",
                      ImVec2(button_width, 38.f * scale)))
        hooks::ActivateAction(tas::Action::SaveCheckpoint);
    ImGui::SameLine();
    if (ImGui::Button("Load checkpoint",
                      ImVec2(button_width, 38.f * scale)))
        hooks::ActivateAction(tas::Action::LoadCheckpoint);
    const auto checkpoint_status = tas::checkpoint::GetStatus();
    ImGui::TextWrapped("%s", checkpoint_status.last_result);
    ImGui::Separator();
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
    if (g_initialised && g_render_target &&
        g_menu_open.load(std::memory_order_relaxed)) {
        LARGE_INTEGER started = {};
        LARGE_INTEGER ended = {};
        QueryPerformanceCounter(&started);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ApplyGuiScale();
        ImGui::GetIO().MouseDrawCursor = true;
        FeedMenuInput();
        ImGui::NewFrame();
        DrawMenu();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        QueryPerformanceCounter(&ended);
        const ULONGLONG elapsed = static_cast<ULONGLONG>(ended.QuadPart - started.QuadPart);
        const ULONGLONG now = GetTickCount64();
        if (g_ui_perf_count == 0) g_ui_perf_last_ms = now;
        g_ui_perf_total += elapsed;
        if (elapsed > g_ui_perf_max) g_ui_perf_max = elapsed;
        ++g_ui_perf_count;
        if (now - g_ui_perf_last_ms >= 5000 && g_ui_perf_frequency.QuadPart) {
            SC_LOG("tas: perf ui_open avg_us=%.1f max_us=%.1f samples=%llu",
                   1000000.0 * static_cast<double>(g_ui_perf_total) /
                       static_cast<double>(g_ui_perf_frequency.QuadPart) /
                       static_cast<double>(g_ui_perf_count),
                   1000000.0 * static_cast<double>(g_ui_perf_max) /
                       static_cast<double>(g_ui_perf_frequency.QuadPart),
                   static_cast<unsigned long long>(g_ui_perf_count));
            g_ui_perf_total = 0;
            g_ui_perf_max = 0;
            g_ui_perf_count = 0;
            g_ui_perf_last_ms = now;
        }
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
