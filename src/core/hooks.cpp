#include "hooks.h"

#include <windows.h>

#include <cmath>

#include "../../third_party/minhook/include/MinHook.h"

#include "../game/enemies.h"
#include "../game/orders.h"
#include "../game/puppet.h"
#include "../game/replay.h"
#include "../game/runstate.h"
#include "../game/selftest.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "log.h"
#include "offsets.g.h"
#include "trace.h"

namespace sifucoop::hooks {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using TickFn = void(__fastcall*)(void*, float, bool);
using WorldTickFn = void(__fastcall*)(void*, int, float);
using EffectiveTimeDilationFn = float(__fastcall*)(void*);

constexpr float kSpeedPresets[] = {0.5f, 0.2f, 0.1f};
constexpr float kFrameStepSeconds = 1.f / 60.f;

float g_tas_multiplier = 1.f;
bool g_paused = false;
WorldTickFn g_original_world_tick = nullptr;
EffectiveTimeDilationFn g_original_effective_time_dilation = nullptr;
void* g_world_tick_target = nullptr;
void* g_effective_time_dilation_target = nullptr;
bool g_step_requested = false;
bool g_step_in_progress = false;
unsigned long long g_steps = 0;
float g_last_logged_game_scale = -1.f;
float g_last_logged_tas_multiplier = -1.f;
float g_last_logged_effective_scale = -1.f;
ULONGLONG g_last_time_scale_log_ms = 0;

bool GameFocused() {
    HWND foreground = GetForegroundWindow();
    DWORD process_id = 0;
    if (!foreground) return false;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

bool Pressed(int key, bool focused) {
    static bool previous[5] = {};
    const int index = key - VK_F1;
    if (index < 0 || index >= 5) return false;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = down && !previous[index];
    previous[index] = down;
    return focused && pressed;
}

void LogTimeScale(float game_scale, float tas_multiplier, float effective_scale) {
    const ULONGLONG now = GetTickCount64();
    const bool changed =
        std::fabs(game_scale - g_last_logged_game_scale) > 0.01f ||
        std::fabs(tas_multiplier - g_last_logged_tas_multiplier) > 0.001f ||
        std::fabs(effective_scale - g_last_logged_effective_scale) > 0.01f;
    if (!changed || (g_last_time_scale_log_ms && now - g_last_time_scale_log_ms < 250))
        return;

    SC_LOG("tas: time scale game=%.3f multiplier=%.1f effective=%.3f", game_scale,
           tas_multiplier, effective_scale);
    g_last_logged_game_scale = game_scale;
    g_last_logged_tas_multiplier = tas_multiplier;
    g_last_logged_effective_scale = effective_scale;
    g_last_time_scale_log_ms = now;
}

float __fastcall EffectiveTimeDilationHook(void* self) {
    const float game_scale = g_original_effective_time_dilation(self);
    const float tas_multiplier = g_step_in_progress ? 1.f : g_tas_multiplier;
    const float effective_scale = game_scale * tas_multiplier;
    if (!g_step_in_progress)
        LogTimeScale(game_scale, tas_multiplier, effective_scale);
    return effective_scale;
}

void PollTASKeys() {
    const bool focused = GameFocused();
    const bool pause = Pressed(VK_F1, focused);
    const bool step = Pressed(VK_F2, focused);
    const bool half = Pressed(VK_F3, focused);
    const bool fifth = Pressed(VK_F4, focused);
    const bool tenth = Pressed(VK_F5, focused);

    if (half) {
        g_tas_multiplier = std::fabs(g_tas_multiplier - kSpeedPresets[0]) < 0.001f
                               ? 1.f
                               : kSpeedPresets[0];
        SC_LOG("tas: multiplier selected %.1f", g_tas_multiplier);
    } else if (fifth) {
        g_tas_multiplier = std::fabs(g_tas_multiplier - kSpeedPresets[1]) < 0.001f
                               ? 1.f
                               : kSpeedPresets[1];
        SC_LOG("tas: multiplier selected %.1f", g_tas_multiplier);
    } else if (tenth) {
        g_tas_multiplier = std::fabs(g_tas_multiplier - kSpeedPresets[2]) < 0.001f
                               ? 1.f
                               : kSpeedPresets[2];
        SC_LOG("tas: multiplier selected %.1f", g_tas_multiplier);
    }

    if (pause && g_original_world_tick) {
        g_paused = !g_paused;
        g_step_requested = false;
        SC_LOG("tas: %s", g_paused ? "paused" : "resumed");
    }

    if (step && g_paused && g_original_world_tick) g_step_requested = true;
}

void __fastcall WorldTickHook(void* self, int tick_type, float delta_seconds) {
    if (!g_paused || self != ue::GetWorld()) {
        g_original_world_tick(self, tick_type, delta_seconds);
        return;
    }

    if (!g_step_requested) return;
    g_step_requested = false;
    g_step_in_progress = true;
    g_original_world_tick(self, tick_type, kFrameStepSeconds);
    g_step_in_progress = false;
    ++g_steps;
    SC_LOG("tas: step %llu", g_steps);
}


TickFn g_original_tick = nullptr;
void** g_vtable = nullptr;
int g_tick_slot = -1;

std::uintptr_t g_base = 0;
unsigned long long g_frames = 0;

void OnFrame() {
    ++g_frames;

    // The socket is pumped before anything that needs a world, so menus and
    // level loads keep acknowledging, pinging and receiving.
    sifucoop::net::PumpNetwork();

    sifucoop::game::PreparePuppetLifecycle();

    sifucoop::game::TickEnemies();
    sifucoop::game::TickPuppet();
    sifucoop::game::TickSelfTest();

    // After everything has moved this frame, so the digest describes the state
    // the player actually saw.
    sifucoop::trace::Tick();

    if (g_frames % 30 != 0) return;

    ue::UObject* world = ue::GetWorld();
    if (!world) return;

    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    sifucoop::game::TickRunState(player);
    sifucoop::game::TickReplayProbe();

    if (!sifucoop::game::IsOrderHookInstalled()) {
        sifucoop::game::InstallOrderHook(g_base, player);

        char path[512] = {};
        if (ue::GetObjectPathName(player, path, sizeof(path))) {
            wchar_t wide[512] = {};
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 512);
            ue::UObject* resolved = ue::FindObjectByPath(wide);
            SC_LOG("objmap: player path='%s' -> %s", path,
                   resolved == player ? "resolves back, MATCH" : "MISMATCH");
        } else {
            SC_LOG("objmap: GetPathName failed");
        }
    }
}

float g_frame_delta = 1.f / 60.f;

void __fastcall TickHook(void* self, float delta_seconds, bool idle_mode) {
    if (delta_seconds > 0.f && delta_seconds < 0.25f) {
        g_frame_delta = delta_seconds;
    } else {
        g_frame_delta = 1.f / 60.f;
    }

    PollTASKeys();
    g_original_tick(self, delta_seconds, idle_mode);

    OnFrame();
}

}

bool InstallTickHook(std::uintptr_t base, void* gengine) {
    g_base = base;

    const auto target = reinterpret_cast<void*>(base + offsets::UGameEngine_Tick);
    auto** vtable = *reinterpret_cast<void***>(gengine);

    for (int i = 0; i < 512; ++i) {
        if (IsBadReadPtr(&vtable[i], sizeof(void*))) break;
        if (vtable[i] == target) {
            g_tick_slot = i;
            break;
        }
    }

    if (g_tick_slot < 0) {
        SC_LOG("hook: UGameEngine::Tick not found in GEngine vtable -- not hooking");
        return false;
    }

    SC_LOG("hook: Tick is vtable slot %d", g_tick_slot);

    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        SC_LOG("hook: VirtualProtect failed (err %lu)", GetLastError());
        return false;
    }

    g_original_tick = reinterpret_cast<TickFn>(vtable[g_tick_slot]);
    vtable[g_tick_slot] = reinterpret_cast<void*>(&TickHook);
    VirtualProtect(&vtable[g_tick_slot], sizeof(void*), old_protect, &old_protect);

    g_vtable = vtable;
    SC_LOG("hook: installed (original at %p)", reinterpret_cast<void*>(g_original_tick));

    if (!offsets::AWorldSettings_GetEffectiveTimeDilation)
        SC_LOG("tas: GetEffectiveTimeDilation symbol missing; speed presets unavailable");
    if (!offsets::UWorld_Tick)
        SC_LOG("tas: UWorld::Tick symbol missing; pause and frame step unavailable");
    if (!offsets::AWorldSettings_GetEffectiveTimeDilation && !offsets::UWorld_Tick)
        return true;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        SC_LOG("tas: MinHook init failed (%d)", static_cast<int>(init));
        return true;
    }

    if (offsets::AWorldSettings_GetEffectiveTimeDilation) {
        g_effective_time_dilation_target =
            reinterpret_cast<void*>(base + offsets::AWorldSettings_GetEffectiveTimeDilation);
        MH_STATUS status = MH_CreateHook(
            g_effective_time_dilation_target,
            reinterpret_cast<void*>(&EffectiveTimeDilationHook),
            reinterpret_cast<void**>(&g_original_effective_time_dilation));
        if (status == MH_OK) status = MH_EnableHook(g_effective_time_dilation_target);
        if (status != MH_OK) {
            SC_LOG("tas: effective time dilation hook failed (%d)",
                   static_cast<int>(status));
            MH_RemoveHook(g_effective_time_dilation_target);
            g_original_effective_time_dilation = nullptr;
            g_effective_time_dilation_target = nullptr;
        } else {
            SC_LOG("tas: effective time dilation hooked; F3 0.5x, F4 0.2x, F5 0.1x");
        }
    }

    if (offsets::UWorld_Tick) {
        g_world_tick_target = reinterpret_cast<void*>(base + offsets::UWorld_Tick);
        MH_STATUS status = MH_CreateHook(g_world_tick_target,
                                         reinterpret_cast<void*>(&WorldTickHook),
                                         reinterpret_cast<void**>(&g_original_world_tick));
        if (status == MH_OK) status = MH_EnableHook(g_world_tick_target);
        if (status != MH_OK) {
            SC_LOG("tas: world tick hook failed (%d)", static_cast<int>(status));
            MH_RemoveHook(g_world_tick_target);
            g_original_world_tick = nullptr;
            g_world_tick_target = nullptr;
        } else {
            SC_LOG("tas: world tick hooked; F1 pause, F2 step");
        }
    }
    return true;
}

void RemoveTickHook() {
    if (g_effective_time_dilation_target) {
        MH_DisableHook(g_effective_time_dilation_target);
        MH_RemoveHook(g_effective_time_dilation_target);
        g_effective_time_dilation_target = nullptr;
        g_original_effective_time_dilation = nullptr;
    }
    if (g_world_tick_target) {
        MH_DisableHook(g_world_tick_target);
        MH_RemoveHook(g_world_tick_target);
        g_world_tick_target = nullptr;
        g_original_world_tick = nullptr;
    }
    if (!g_vtable || g_tick_slot < 0 || !g_original_tick) return;
    DWORD old_protect = 0;
    if (VirtualProtect(&g_vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_vtable[g_tick_slot] = reinterpret_cast<void*>(g_original_tick);
        VirtualProtect(&g_vtable[g_tick_slot], sizeof(void*), old_protect, &old_protect);
    }
    g_vtable = nullptr;
}

float FrameDeltaSeconds() { return g_frame_delta; }

unsigned long long FrameCount() { return g_frames; }

}
