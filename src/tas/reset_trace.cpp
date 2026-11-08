#include "reset_trace.h"

#include <windows.h>

#include <cstdint>
#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::reset_trace {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using ResetFn = void(__fastcall*)(void*);

std::uintptr_t g_module_base = 0;
void* g_actor_reset_target = nullptr;
void* g_actor_k2_reset_target = nullptr;
void* g_game_mode_reset_target = nullptr;
void* g_game_mode_reset_level_target = nullptr;
void* g_restart_map_target = nullptr;
ResetFn g_actor_reset = nullptr;
ResetFn g_actor_k2_reset = nullptr;
ResetFn g_game_mode_reset = nullptr;
ResetFn g_game_mode_reset_level = nullptr;
ResetFn g_restart_map = nullptr;

void LogReset(const char* event, void* self, std::uintptr_t caller) {
    char object[512] = {};
    char level[512] = {};
    const bool have_object = ue::GetObjectPathName(static_cast<ue::UObject*>(self),
                                                   object, sizeof(object));
    const bool have_level = ue::GetCurrentLevelPath(level, sizeof(level));
    const std::uintptr_t caller_rva = caller >= g_module_base ? caller - g_module_base : 0;
    SC_LOG("tas: reset trace event=%s object=%s ptr=%p level=%s caller_rva=0x%llX",
           event, have_object ? object : "unavailable", self,
           have_level ? level : "unavailable",
           static_cast<unsigned long long>(caller_rva));
}

void __fastcall ActorResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    LogReset("AActor::Reset", self, caller);
    if (g_actor_reset) g_actor_reset(self);
}

void __fastcall ActorK2ResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    LogReset("AActor::K2_OnReset", self, caller);
    if (g_actor_k2_reset) g_actor_k2_reset(self);
}

void __fastcall GameModeResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    LogReset("AGameModeBase::Reset.begin", self, caller);
    if (g_game_mode_reset) g_game_mode_reset(self);
    LogReset("AGameModeBase::Reset.end", self, caller);
}

void __fastcall GameModeResetLevelHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    LogReset("AGameModeBase::ResetLevel.begin", self, caller);
    if (g_game_mode_reset_level) g_game_mode_reset_level(self);
    LogReset("AGameModeBase::ResetLevel.end", self, caller);
}

void __fastcall RestartMapHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    LogReset("UWGGameFlow::BPF_RestartCurrentMap.begin", self, caller);
    if (g_restart_map) g_restart_map(self);
    LogReset("UWGGameFlow::BPF_RestartCurrentMap.end", self, caller);
}

bool InstallOne(std::uint32_t rva, void* detour, void** original,
                void** target, const char* name) {
    if (!rva) {
        SC_LOG("tas: reset trace %s symbol unavailable", name);
        return false;
    }
    *target = reinterpret_cast<void*>(g_module_base + rva);
    MH_STATUS status = MH_CreateHook(*target, detour, original);
    if (status != MH_OK) {
        SC_LOG("tas: reset trace %s hook failed (%d)", name, static_cast<int>(status));
        *target = nullptr;
        *original = nullptr;
        return false;
    }
    status = MH_EnableHook(*target);
    if (status != MH_OK) {
        SC_LOG("tas: reset trace %s enable failed (%d)", name, static_cast<int>(status));
        MH_RemoveHook(*target);
        *target = nullptr;
        *original = nullptr;
        return false;
    }
    SC_LOG("tas: reset trace hooked %s", name);
    return true;
}

void RemoveOne(void** target, ResetFn* original) {
    if (!*target) return;
    MH_DisableHook(*target);
    MH_RemoveHook(*target);
    *target = nullptr;
    *original = nullptr;
}

}

void InstallHooks(std::uintptr_t module_base) {
    g_module_base = module_base;
    InstallOne(offsets::AActor_Reset, reinterpret_cast<void*>(&ActorResetHook),
               reinterpret_cast<void**>(&g_actor_reset), &g_actor_reset_target,
               "AActor::Reset");
    InstallOne(offsets::AActor_K2_OnReset, reinterpret_cast<void*>(&ActorK2ResetHook),
               reinterpret_cast<void**>(&g_actor_k2_reset), &g_actor_k2_reset_target,
               "AActor::K2_OnReset");
    InstallOne(offsets::AGameModeBase_Reset, reinterpret_cast<void*>(&GameModeResetHook),
               reinterpret_cast<void**>(&g_game_mode_reset), &g_game_mode_reset_target,
               "AGameModeBase::Reset");
    InstallOne(offsets::AGameModeBase_ResetLevel,
               reinterpret_cast<void*>(&GameModeResetLevelHook),
               reinterpret_cast<void**>(&g_game_mode_reset_level),
               &g_game_mode_reset_level_target, "AGameModeBase::ResetLevel");
    InstallOne(offsets::UWGGameFlow_BPF_RestartCurrentMap,
               reinterpret_cast<void*>(&RestartMapHook),
               reinterpret_cast<void**>(&g_restart_map), &g_restart_map_target,
               "UWGGameFlow::BPF_RestartCurrentMap");
}

void RemoveHooks() {
    RemoveOne(&g_restart_map_target, &g_restart_map);
    RemoveOne(&g_game_mode_reset_level_target, &g_game_mode_reset_level);
    RemoveOne(&g_game_mode_reset_target, &g_game_mode_reset);
    RemoveOne(&g_actor_k2_reset_target, &g_actor_k2_reset);
    RemoveOne(&g_actor_reset_target, &g_actor_reset);
}

}
