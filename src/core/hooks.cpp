#include "hooks.h"

#include <windows.h>
#include <atomic>
#include <cmath>

#include "../../third_party/minhook/include/MinHook.h"
#include "../game/actors.h"
#include "../tas/controls.h"
#include "../tas/input_hook.h"
#include "../ue/reflection.h"
#include "../ui/overlay.h"
#include "log.h"
#include "offsets.g.h"

namespace sifucoop::hooks {
namespace {

using EngineTickFn = void(__fastcall*)(void*, float, bool);
using WorldTickFn = void(__fastcall*)(void*, int, float);
using EffectiveTimeDilationFn = float(__fastcall*)(void*);
constexpr float kStepDelta = 1.f / 60.f;
void** g_engine_vtable = nullptr;
int g_tick_slot = -1;
EngineTickFn g_engine_tick = nullptr;
WorldTickFn g_world_tick = nullptr;
EffectiveTimeDilationFn g_effective_dilation = nullptr;
void* g_world_tick_target = nullptr;
void* g_effective_target = nullptr;
std::atomic<float> g_speed{1.f};
std::atomic<bool> g_supported{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_step_in_progress{false};
std::atomic<int> g_steps{0};
std::atomic<unsigned long long> g_engine_ticks{0};
std::atomic<ULONGLONG> g_indicator_until{0};
std::atomic<float> g_last_delta{1.f / 60.f};
bool g_gui_owns_pause = false;
unsigned long long g_pause_serial = 0;
std::atomic<unsigned long long> g_pause_change_serial{0};
ue::UObject* g_last_world = nullptr;

bool GameFocused() {
    HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    return foreground && GetWindowThreadProcessId(foreground, &process) &&
           process == GetCurrentProcessId();
}

void TogglePause() {
    if (!g_supported.load(std::memory_order_relaxed) || !g_world_tick) return;
    const bool paused = !g_paused.load(std::memory_order_relaxed);
    g_paused.store(paused, std::memory_order_relaxed);
    g_steps.store(0, std::memory_order_relaxed);
    g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
    SC_LOG("time-control: %s", paused ? "paused" : "resumed");
}

void OpenOrCloseGui() {
    if (!ui::IsMenuOpen()) {
        g_gui_owns_pause = false;
        if (!g_paused.load(std::memory_order_relaxed) &&
            g_supported.load(std::memory_order_relaxed)) {
            TogglePause();
            if (g_paused.load(std::memory_order_relaxed)) {
                g_gui_owns_pause = true;
                g_pause_serial = g_pause_change_serial.load(std::memory_order_relaxed);
            }
        }
        ui::SetMenuOpen(true);
        return;
    }
    const bool resume = g_gui_owns_pause && g_paused.load(std::memory_order_relaxed) &&
        g_pause_change_serial.load(std::memory_order_relaxed) == g_pause_serial;
    g_gui_owns_pause = false;
    ui::SetMenuOpen(false);
    if (resume) TogglePause();
}

void SetSpeedInternal(float speed) {
    if (!g_supported.load(std::memory_order_relaxed) || !std::isfinite(speed)) return;
    int step = tas::SpeedStep(speed);
    const float value = static_cast<float>(step) / 10.f;
    const float previous = g_speed.exchange(value, std::memory_order_relaxed);
    if (std::fabs(previous - value) > 0.001f) {
        g_indicator_until.store(GetTickCount64() + 1800, std::memory_order_relaxed);
        SC_LOG("time-control: speed %.1fx", value);
    }
}

void RefreshGameplayState() {
    ue::UObject* world = ue::GetWorld();
    tas::SetInputController(world ? ue::GetPlayerController(world, 0) : nullptr);
    if (world != g_last_world) {
        g_last_world = world;
        g_steps.store(0, std::memory_order_relaxed);
        g_gui_owns_pause = false;
        if (g_paused.exchange(false, std::memory_order_relaxed))
            g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
    }
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const char* reason = nullptr;
    const bool supported = game::SupportedTASGameplayState(world, player, &reason);
    const bool was_supported = g_supported.exchange(supported, std::memory_order_relaxed);
    if (supported) return;
    g_steps.store(0, std::memory_order_relaxed);
    g_gui_owns_pause = false;
    if (g_paused.exchange(false, std::memory_order_relaxed))
        g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
    if (was_supported) {
        g_speed.store(1.f, std::memory_order_relaxed);
        SC_LOG("time-control: disabled (%s); pause released and speed returned to 1.0x",
               reason ? reason : "unsupported game state");
    }
}

float __fastcall EffectiveTimeDilationHook(void* self) {
    const float native = g_effective_dilation(self);
    const float multiplier = !g_supported.load(std::memory_order_relaxed) ||
                                     g_step_in_progress.load(std::memory_order_relaxed)
                                 ? 1.f : g_speed.load(std::memory_order_relaxed);
    return native * multiplier;
}

void __fastcall WorldTickHook(void* self, int tick_type, float delta_seconds) {
    const bool is_current_world = self == ue::GetWorld();
    if (!is_current_world) { g_world_tick(self, tick_type, delta_seconds); return; }
    g_last_delta.store(delta_seconds, std::memory_order_relaxed);
    if (!g_paused.load(std::memory_order_relaxed)) {
        g_world_tick(self, tick_type, delta_seconds);
        return;
    }
    int count = g_steps.load(std::memory_order_relaxed);
    while (count > 0 && !g_steps.compare_exchange_weak(count, count - 1,
                                                       std::memory_order_relaxed)) {}
    if (count <= 0) return;
    g_step_in_progress.store(true, std::memory_order_relaxed);
    g_world_tick(self, tick_type, kStepDelta);
    g_step_in_progress.store(false, std::memory_order_relaxed);
}

void ActivateConfiguredSlot(int slot) {
    if (slot < 0 || slot >= 9 || !g_supported.load(std::memory_order_relaxed)) return;
    switch (tas::GetSlotKind(slot)) {
        case tas::SlotKind::Pause: TogglePause(); break;
        case tas::SlotKind::Speed: SetSpeedInternal(tas::SpeedForPreset(tas::PresetIndexForSlot(slot))); break;
        case tas::SlotKind::FrameOne: AdvanceFrames(1); break;
        case tas::SlotKind::FrameMany: AdvanceFrames(tas::GetSettings().frames_per_step); break;
    }
}

void __fastcall EngineTickHook(void* self, float delta_seconds, bool idle_mode) {
    if (delta_seconds > 0.f && delta_seconds < 0.25f)
        g_last_delta.store(delta_seconds, std::memory_order_relaxed);
    tas::UpdateInputNames();
    RefreshGameplayState();
    const bool focused = GameFocused();
    tas::SetInputSuppressionState(focused, g_supported.load(std::memory_order_relaxed));
    if (tas::WasGuiTogglePressed(focused && !ui::IsBindingCaptureActive())) OpenOrCloseGui();
    if (focused && !ui::IsMenuOpen()) {
        for (int slot = 0; slot < 9; ++slot)
            if (tas::WasSlotPressed(slot, true)) ActivateConfiguredSlot(slot);
    } else {
        for (int slot = 0; slot < 9; ++slot) tas::WasSlotPressed(slot, false);
    }
    g_engine_tick(self, delta_seconds, idle_mode);
    g_engine_ticks.fetch_add(1, std::memory_order_relaxed);
}

}

bool InstallTickHook(std::uintptr_t base, void* engine) {
    if (!engine || !offsets::UGameEngine_Tick) return false;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (!tas::InstallInputHook(base)) {
        SC_LOG("time-control: stopped because safe local-input suppression is unavailable");
        return false;
    }

    if (!offsets::AWorldSettings_GetEffectiveTimeDilation || !offsets::UWorld_Tick) {
        tas::RemoveInputHook();
        return false;
    }
    g_effective_target = reinterpret_cast<void*>(base + offsets::AWorldSettings_GetEffectiveTimeDilation);
    status = MH_CreateHook(g_effective_target, reinterpret_cast<void*>(&EffectiveTimeDilationHook),
                           reinterpret_cast<void**>(&g_effective_dilation));
    if (status == MH_OK) status = MH_EnableHook(g_effective_target);
    if (status != MH_OK) {
        MH_RemoveHook(g_effective_target); g_effective_target = nullptr; tas::RemoveInputHook();
        SC_LOG("time-control: effective time-dilation hook could not be installed");
        return false;
    }
    g_world_tick_target = reinterpret_cast<void*>(base + offsets::UWorld_Tick);
    status = MH_CreateHook(g_world_tick_target, reinterpret_cast<void*>(&WorldTickHook),
                           reinterpret_cast<void**>(&g_world_tick));
    if (status == MH_OK) status = MH_EnableHook(g_world_tick_target);
    if (status != MH_OK) {
        MH_RemoveHook(g_world_tick_target); g_world_tick_target = nullptr;
        MH_DisableHook(g_effective_target); MH_RemoveHook(g_effective_target); g_effective_target = nullptr;
        tas::RemoveInputHook();
        SC_LOG("time-control: UWorld::Tick hook could not be installed");
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(engine);
    const auto target = reinterpret_cast<void*>(base + offsets::UGameEngine_Tick);
    for (int i = 0; i < 512 && !IsBadReadPtr(&vtable[i], sizeof(void*)); ++i) {
        if (vtable[i] == target) { g_tick_slot = i; break; }
    }
    if (g_tick_slot < 0) {
        SC_LOG("time-control: UGameEngine::Tick not found");
        MH_DisableHook(g_world_tick_target); MH_RemoveHook(g_world_tick_target); g_world_tick_target = nullptr;
        MH_DisableHook(g_effective_target); MH_RemoveHook(g_effective_target); g_effective_target = nullptr;
        tas::RemoveInputHook();
        return false;
    }
    DWORD protection = 0;
    if (!VirtualProtect(&vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &protection)) {
        MH_DisableHook(g_world_tick_target); MH_RemoveHook(g_world_tick_target); g_world_tick_target = nullptr;
        MH_DisableHook(g_effective_target); MH_RemoveHook(g_effective_target); g_effective_target = nullptr;
        tas::RemoveInputHook();
        return false;
    }
    g_engine_tick = reinterpret_cast<EngineTickFn>(vtable[g_tick_slot]);
    vtable[g_tick_slot] = reinterpret_cast<void*>(&EngineTickHook);
    VirtualProtect(&vtable[g_tick_slot], sizeof(void*), protection, &protection);
    g_engine_vtable = vtable;

    SC_LOG("time-control: engine hook installed (effective dilution=%s, world pause/step=%s)",
           g_effective_dilation ? "yes" : "no", g_world_tick ? "yes" : "no");
    return true;
}

void RemoveTickHook() {
    tas::RemoveInputHook();
    if (g_world_tick_target) { MH_DisableHook(g_world_tick_target); MH_RemoveHook(g_world_tick_target); }
    if (g_effective_target) { MH_DisableHook(g_effective_target); MH_RemoveHook(g_effective_target); }
    if (!g_engine_vtable || g_tick_slot < 0 || !g_engine_tick) return;
    DWORD protection = 0;
    if (VirtualProtect(&g_engine_vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &protection)) {
        g_engine_vtable[g_tick_slot] = reinterpret_cast<void*>(g_engine_tick);
        VirtualProtect(&g_engine_vtable[g_tick_slot], sizeof(void*), protection, &protection);
    }
    g_engine_vtable = nullptr;
}

float FrameDeltaSeconds() { return g_last_delta.load(std::memory_order_relaxed); }
unsigned long long FrameCount() { return g_engine_ticks.load(std::memory_order_relaxed); }

void AdvanceFrames(int count) {
    if (!g_supported.load(std::memory_order_relaxed) || !g_paused.load(std::memory_order_relaxed) || !g_world_tick) return;
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    int current = g_steps.load(std::memory_order_relaxed);
    while (!g_steps.compare_exchange_weak(current, current + count > 10 ? 10 : current + count,
                                           std::memory_order_relaxed)) {}
}

void ActivateSlot(int slot) { ActivateConfiguredSlot(slot); }
void ToggleGui() { OpenOrCloseGui(); }
void SetPaused(bool paused) { if (g_paused.load(std::memory_order_relaxed) != paused) TogglePause(); }
void SetPlaybackSpeed(float speed) { SetSpeedInternal(speed); }
float PlaybackSpeed() { return g_speed.load(std::memory_order_relaxed); }
bool IsPaused() { return g_paused.load(std::memory_order_relaxed); }
bool IsGameplaySupported() { return g_supported.load(std::memory_order_relaxed); }
ULONGLONG SpeedIndicatorUntil() { return g_indicator_until.load(std::memory_order_relaxed); }

}
