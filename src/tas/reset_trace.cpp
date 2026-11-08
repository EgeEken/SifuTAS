#include "reset_trace.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::reset_trace {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using ResetFn = void(__fastcall*)(void*);
constexpr std::uint32_t kActorDetailLimit = 32;
constexpr ULONGLONG kActorDetailWindowMs = 5000;
constexpr ULONGLONG kSummaryIntervalMs = 1000;

struct ResetIdentity {
    std::uintptr_t object = 0;
    std::uintptr_t caller_rva = 0;
    char object_path[512] = {};
    char level_path[512] = {};
};

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
std::atomic<ULONGLONG> g_actor_window_start{0};
std::atomic<std::uint32_t> g_actor_detail_count{0};
std::atomic<std::uint64_t> g_actor_suppressed_pending{0};
std::atomic<std::uint64_t> g_actor_suppressed_total{0};
std::atomic<ULONGLONG> g_last_summary_ms{0};

ResetIdentity CaptureIdentity(void* self, std::uintptr_t caller) {
    ResetIdentity identity = {};
    identity.object = reinterpret_cast<std::uintptr_t>(self);
    identity.caller_rva = caller >= g_module_base ? caller - g_module_base : 0;
    if (!ue::GetObjectPathName(static_cast<ue::UObject*>(self), identity.object_path,
                               sizeof(identity.object_path)))
        lstrcpynA(identity.object_path, "unavailable", sizeof(identity.object_path));
    if (!ue::GetCurrentLevelPath(identity.level_path, sizeof(identity.level_path)))
        lstrcpynA(identity.level_path, "unavailable", sizeof(identity.level_path));
    return identity;
}

void LogReset(const char* event, const char* phase, const ResetIdentity& identity) {
    SC_LOG("tas: reset trace event=%s phase=%s object=%s ptr=%p level=%s "
           "identity=captured_before_call caller_rva=0x%llX",
           event, phase, identity.object_path, reinterpret_cast<void*>(identity.object),
           identity.level_path, static_cast<unsigned long long>(identity.caller_rva));
}

bool ShouldLogActorDetail() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG window_start = g_actor_window_start.load(std::memory_order_relaxed);
    if (now - window_start >= kActorDetailWindowMs &&
        g_actor_window_start.compare_exchange_strong(window_start, now,
                                                     std::memory_order_relaxed))
        g_actor_detail_count.store(0, std::memory_order_relaxed);
    if (g_actor_detail_count.fetch_add(1, std::memory_order_relaxed) < kActorDetailLimit)
        return true;
    g_actor_suppressed_pending.fetch_add(1, std::memory_order_relaxed);
    g_actor_suppressed_total.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void FlushActorSummary() {
    const std::uint64_t pending = g_actor_suppressed_pending.load(std::memory_order_relaxed);
    if (!pending) return;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_last_summary_ms.load(std::memory_order_relaxed);
    if (now - last < kSummaryIntervalMs ||
        !g_last_summary_ms.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
    const std::uint64_t reported =
        g_actor_suppressed_pending.exchange(0, std::memory_order_relaxed);
    if (reported)
        SC_LOG("tas: reset trace actor_detail_limit=%u per %llu ms suppressed=%llu "
               "suppressed_total=%llu",
               kActorDetailLimit, static_cast<unsigned long long>(kActorDetailWindowMs),
               static_cast<unsigned long long>(reported),
               static_cast<unsigned long long>(g_actor_suppressed_total.load(
                   std::memory_order_relaxed)));
}

void __fastcall ActorResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if (ShouldLogActorDetail())
        LogReset("AActor::Reset", "callback", CaptureIdentity(self, caller));
    if (g_actor_reset) g_actor_reset(self);
}

void __fastcall ActorK2ResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if (ShouldLogActorDetail())
        LogReset("AActor::K2_OnReset", "callback", CaptureIdentity(self, caller));
    if (g_actor_k2_reset) g_actor_k2_reset(self);
}

void __fastcall GameModeResetHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const ResetIdentity identity = CaptureIdentity(self, caller);
    LogReset("AGameModeBase::Reset", "begin", identity);
    if (g_game_mode_reset) g_game_mode_reset(self);
    LogReset("AGameModeBase::Reset", "end", identity);
    FlushActorSummary();
}

void __fastcall GameModeResetLevelHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const ResetIdentity identity = CaptureIdentity(self, caller);
    LogReset("AGameModeBase::ResetLevel", "begin", identity);
    if (g_game_mode_reset_level) g_game_mode_reset_level(self);
    LogReset("AGameModeBase::ResetLevel", "end", identity);
    FlushActorSummary();
}

void __fastcall RestartMapHook(void* self) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const ResetIdentity identity = CaptureIdentity(self, caller);
    LogReset("UWGGameFlow::BPF_RestartCurrentMap", "begin", identity);
    if (g_restart_map) g_restart_map(self);
    LogReset("UWGGameFlow::BPF_RestartCurrentMap", "end", identity);
    FlushActorSummary();
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

void Tick() { FlushActorSummary(); }

}
