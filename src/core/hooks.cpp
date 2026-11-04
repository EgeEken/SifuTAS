#include "hooks.h"

#include <windows.h>

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

enum class TASMode { Normal, Slow, Paused };

TASMode g_mode = TASMode::Normal;
TASMode g_resume_mode = TASMode::Normal;
WorldTickFn g_original_world_tick = nullptr;
void* g_world_tick_target = nullptr;
bool g_step_requested = false;
unsigned long long g_steps = 0;

bool Pressed(int key) {
    static bool previous[4] = {};
    const int index = key - VK_F6;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = down && !previous[index];
    previous[index] = down;
    return pressed;
}

void SetSpeed(float speed) {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* gameplay_statics =
        ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
    if (!world || !gameplay_statics) return;

    struct Params {
        ue::UObject* WorldContextObject;
        float TimeDilation;
    } params = {world, speed};

    if (!ue::CallFunction(gameplay_statics, L"SetGlobalTimeDilation", &params))
        SC_LOG("tas: SetGlobalTimeDilation failed");
}

void PollTASKeys() {
    const bool slow = Pressed(VK_F6);
    const bool pause = Pressed(VK_F7);
    const bool step = Pressed(VK_F8);
    const bool normal = Pressed(VK_F9);

    if (normal) {
        g_mode = TASMode::Normal;
        g_resume_mode = TASMode::Normal;
        g_step_requested = false;
        SetSpeed(1.f);
        SC_LOG("tas: normal speed");
    } else if (slow) {
        if (g_mode == TASMode::Paused) {
            g_resume_mode = g_resume_mode == TASMode::Slow ? TASMode::Normal : TASMode::Slow;
        } else {
            g_mode = g_mode == TASMode::Slow ? TASMode::Normal : TASMode::Slow;
            SetSpeed(g_mode == TASMode::Slow ? 0.2f : 1.f);
        }
        SC_LOG("tas: slow mode toggled");
    }

    if (pause && g_original_world_tick) {
        if (g_mode == TASMode::Paused) {
            g_mode = g_resume_mode;
            SetSpeed(g_mode == TASMode::Slow ? 0.2f : 1.f);
        } else {
            g_resume_mode = g_mode;
            g_mode = TASMode::Paused;
            SetSpeed(1.f);
        }
        g_step_requested = false;
        SC_LOG("tas: %s", g_mode == TASMode::Paused ? "paused" : "resumed");
    }

    if (step && g_mode == TASMode::Paused) g_step_requested = true;
}

void __fastcall WorldTickHook(void* self, int tick_type, float delta_seconds) {
    if (g_mode != TASMode::Paused || self != ue::GetWorld()) {
        g_original_world_tick(self, tick_type, delta_seconds);
        return;
    }

    if (!g_step_requested) return;
    g_step_requested = false;
    g_original_world_tick(self, tick_type, 1.f / 60.f);
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

    if (!offsets::UWorld_Tick) {
        SC_LOG("tas: UWorld::Tick symbol missing; only slowdown available");
        return true;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        SC_LOG("tas: MinHook init failed (%d)", static_cast<int>(init));
        return true;
    }

    g_world_tick_target = reinterpret_cast<void*>(base + offsets::UWorld_Tick);
    MH_STATUS status = MH_CreateHook(g_world_tick_target, reinterpret_cast<void*>(&WorldTickHook),
                                     reinterpret_cast<void**>(&g_original_world_tick));
    if (status == MH_OK) status = MH_EnableHook(g_world_tick_target);
    if (status != MH_OK) {
        SC_LOG("tas: world tick hook failed (%d)", static_cast<int>(status));
        MH_RemoveHook(g_world_tick_target);
        g_original_world_tick = nullptr;
        g_world_tick_target = nullptr;
    } else {
        SC_LOG("tas: world tick hooked; F6 slow, F7 pause, F8 step, F9 normal");
    }
    return true;
}

void RemoveTickHook() {
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
