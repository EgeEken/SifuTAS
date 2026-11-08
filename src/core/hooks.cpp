#include "hooks.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../third_party/minhook/include/MinHook.h"

#include "../game/enemies.h"
#include "../game/actors.h"
#include "../game/orders.h"
#include "../game/player2.h"
#include "../game/puppet.h"
#include "../game/replay.h"
#include "../game/runstate.h"
#include "../game/selftest.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "log.h"
#include "offsets.g.h"
#include "trace.h"
#include "../tas/controls.h"
#include "../tas/checkpoint.h"
#include "../tas/input_trace.h"
#include "../ui/overlay.h"

namespace sifucoop::hooks {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using TickFn = void(__fastcall*)(void*, float, bool);
using WorldTickFn = void(__fastcall*)(void*, int, float);
using EffectiveTimeDilationFn = float(__fastcall*)(void*);
using SetTimeDilationFn = float(__fastcall*)(void*, float);
using SetDemoPlayTimeDilationFn = void(__fastcall*)(void*, float);

constexpr float kFrameStepSeconds = 1.f / 60.f;

std::uintptr_t g_base = 0;
std::atomic<float> g_tas_multiplier{1.f};
std::atomic<bool> g_gameplay_supported{false};
std::atomic<bool> g_paused{false};
std::atomic<unsigned long long> g_pause_change_serial{0};
bool g_gui_pause_owned = false;
unsigned long long g_gui_pause_serial = 0;
WorldTickFn g_original_world_tick = nullptr;
EffectiveTimeDilationFn g_original_effective_time_dilation = nullptr;
SetTimeDilationFn g_original_set_time_dilation = nullptr;
SetDemoPlayTimeDilationFn g_original_set_demo_play_time_dilation = nullptr;
void* g_world_tick_target = nullptr;
void* g_effective_time_dilation_target = nullptr;
void* g_set_time_dilation_target = nullptr;
void* g_set_demo_play_time_dilation_target = nullptr;
std::atomic<int> g_steps_requested{0};
std::atomic<bool> g_step_in_progress{false};
std::atomic<float> g_last_world_tick_delta{0.f};
unsigned long long g_steps = 0;
ULONGLONG g_last_diagnostic_sample_ms = 0;
std::atomic<ULONGLONG> g_last_setter_log_ms{0};
std::atomic<float> g_last_demo_setter_value{-1.f};

enum PerfSlot {
    PerfNetwork,
    PerfPuppetLifecycle,
    PerfEnemies,
    PerfPuppet,
    PerfSelfTest,
    PerfCheckpoint,
    PerfDilation,
    PerfTrace,
    PerfRunState,
    PerfFrame,
    PerfSlotCount,
};

struct PerfStat {
    std::uint64_t total = 0;
    std::uint64_t max = 0;
    std::uint64_t count = 0;
};

PerfStat g_perf[PerfSlotCount] = {};
LARGE_INTEGER g_perf_frequency = {};
ULONGLONG g_last_perf_log_ms = 0;
ULONGLONG g_last_memory_log_ms = 0;

constexpr ULONGLONG kDiagnosticSampleIntervalMs = 1000;
constexpr ULONGLONG kSetterLogIntervalMs = 250;
constexpr ULONGLONG kPerfLogIntervalMs = 5000;
constexpr ULONGLONG kMemoryLogIntervalMs = 60000;

LONGLONG PerfNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void AddPerf(PerfSlot slot, LONGLONG ticks) {
    if (ticks < 0) return;
    PerfStat& stat = g_perf[slot];
    const auto elapsed = static_cast<std::uint64_t>(ticks);
    stat.total += elapsed;
    if (elapsed > stat.max) stat.max = elapsed;
    ++stat.count;
}

double PerfAverageUs(PerfSlot slot) {
    const PerfStat& stat = g_perf[slot];
    return stat.count && g_perf_frequency.QuadPart
               ? static_cast<double>(stat.total) * 1000000.0 /
                     static_cast<double>(g_perf_frequency.QuadPart) / stat.count
               : 0.0;
}

double PerfMaxUs(PerfSlot slot) {
    return g_perf_frequency.QuadPart
               ? static_cast<double>(g_perf[slot].max) * 1000000.0 /
                     static_cast<double>(g_perf_frequency.QuadPart)
               : 0.0;
}

void LogPerfIfDue() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_perf_log_ms < kPerfLogIntervalMs) return;
    g_last_perf_log_ms = now;
    SC_LOG("tas: perf interval_ms=%llu samples=%llu avg_us={onframe=%.1f network=%.1f "
           "lifecycle=%.1f enemies=%.1f puppet=%.1f selftest=%.1f checkpoint=%.1f "
           "dilation=%.1f trace=%.1f runstate=%.1f} max_us={onframe=%.1f enemies=%.1f "
           "puppet=%.1f checkpoint=%.1f trace=%.1f}",
           static_cast<unsigned long long>(kPerfLogIntervalMs),
           static_cast<unsigned long long>(g_perf[PerfFrame].count),
           PerfAverageUs(PerfFrame), PerfAverageUs(PerfNetwork),
           PerfAverageUs(PerfPuppetLifecycle), PerfAverageUs(PerfEnemies),
           PerfAverageUs(PerfPuppet), PerfAverageUs(PerfSelfTest),
           PerfAverageUs(PerfCheckpoint), PerfAverageUs(PerfDilation),
           PerfAverageUs(PerfTrace), PerfAverageUs(PerfRunState),
           PerfMaxUs(PerfFrame), PerfMaxUs(PerfEnemies), PerfMaxUs(PerfPuppet),
           PerfMaxUs(PerfCheckpoint), PerfMaxUs(PerfTrace));
    for (PerfStat& stat : g_perf) stat = {};

    if (now - g_last_memory_log_ms >= kMemoryLogIntervalMs) {
        g_last_memory_log_ms = now;
        PROCESS_MEMORY_COUNTERS_EX memory = {};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory))) {
            SC_LOG("tas: memory working_set_kb=%llu private_kb=%llu",
                   static_cast<unsigned long long>(memory.WorkingSetSize / 1024),
                   static_cast<unsigned long long>(memory.PrivateUsage / 1024));
        }
    }
}

struct WorldContextFloat {
    ue::UObject* WorldContextObject = nullptr;
    float ReturnValue = 0.f;
};

struct WorldSettingsOut {
    ue::UObject* ReturnValue = nullptr;
};

bool GameFocused() {
    HWND foreground = GetForegroundWindow();
    DWORD process_id = 0;
    if (!foreground) return false;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

const char* PresetName(float multiplier) {
    if (std::fabs(multiplier - 0.5f) < 0.001f) return "0.5x";
    if (std::fabs(multiplier - 0.2f) < 0.001f) return "0.2x";
    if (std::fabs(multiplier - 0.1f) < 0.001f) return "0.1x";
    return std::fabs(multiplier - 1.f) < 0.001f ? "1.0x" : "custom";
}

bool SetterLogWindowOpen() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_last_setter_log_ms.load(std::memory_order_relaxed);
    while (now - last >= kSetterLogIntervalMs) {
        if (g_last_setter_log_ms.compare_exchange_weak(last, now,
                                                       std::memory_order_relaxed))
            return true;
    }
    return false;
}

bool ReadFloatAt(const void* object, std::uint32_t offset, float* out) {
    if (!object || !offset || !out) return false;
    const auto* address = static_cast<const std::uint8_t*>(object) + offset;
    if (!ue::RangeReadable(address, sizeof(*out))) return false;
    std::memcpy(out, address, sizeof(*out));
    return std::isfinite(*out);
}

float __fastcall EffectiveTimeDilationHook(void* self) {
    const float game_scale = g_original_effective_time_dilation(self);
    const float multiplier = !g_gameplay_supported.load(std::memory_order_relaxed) ||
                                     g_step_in_progress.load(std::memory_order_relaxed)
                                 ? 1.f : g_tas_multiplier.load(std::memory_order_relaxed);
    return game_scale * multiplier;
}

float __fastcall SetTimeDilationHook(void* self, float requested) {
    float before = 0.f;
    ReadFloatAt(self, offsets::M_AWorldSettings_TimeDilation, &before);
    const float result = g_original_set_time_dilation(self, requested);
    float after = 0.f;
    if (ReadFloatAt(self, offsets::M_AWorldSettings_TimeDilation, &after) &&
        std::fabs(after - before) > 0.001f && SetterLogWindowOpen()) {
        const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const std::uint32_t caller_rva = caller >= g_base
                                             ? static_cast<std::uint32_t>(caller - g_base)
                                             : 0;
        SC_LOG("tas: setter=TimeDilation preset=%s requested=%.4f stored=%.4f->%.4f caller_rva=%08X",
               PresetName(g_tas_multiplier.load(std::memory_order_relaxed)), requested,
               before, after, caller_rva);
    }
    return result;
}

void __fastcall SetDemoPlayTimeDilationHook(void* self, float requested) {
    g_original_set_demo_play_time_dilation(self, requested);
    const float previous = g_last_demo_setter_value.exchange(requested,
                                                              std::memory_order_relaxed);
    if (std::fabs(previous - requested) <= 0.001f || !SetterLogWindowOpen()) return;
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const std::uint32_t caller_rva = caller >= g_base
                                         ? static_cast<std::uint32_t>(caller - g_base)
                                         : 0;
    SC_LOG("tas: setter=DemoPlayTimeDilation preset=%s requested=%.4f caller_rva=%08X",
           PresetName(g_tas_multiplier.load(std::memory_order_relaxed)), requested,
           caller_rva);
}

void SampleTimeDilation() {
    const ULONGLONG now = GetTickCount64();
    if (g_last_diagnostic_sample_ms &&
        now - g_last_diagnostic_sample_ms < kDiagnosticSampleIntervalMs)
        return;
    g_last_diagnostic_sample_ms = now;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!world || !player) return;

    WorldSettingsOut settings_out = {};
    ue::CallFunction(world, L"K2_GetWorldSettings", &settings_out);
    ue::UObject* settings = settings_out.ReturnValue;

    float time_dilation = -1.f;
    float matinee_dilation = -1.f;
    float demo_dilation = -1.f;
    float original_effective = -1.f;
    float player_dilation = -1.f;
    const float multiplier = g_tas_multiplier.load(std::memory_order_relaxed);
    ReadFloatAt(settings, offsets::M_AWorldSettings_TimeDilation, &time_dilation);
    ReadFloatAt(settings, offsets::M_AWorldSettings_MatineeTimeDilation,
                 &matinee_dilation);
    ReadFloatAt(settings, offsets::M_AWorldSettings_DemoPlayTimeDilation,
                 &demo_dilation);
    if (settings && g_original_effective_time_dilation)
        original_effective = g_original_effective_time_dilation(settings);
    ReadFloatAt(player, offsets::M_AActor_CustomTimeDilation, &player_dilation);

    float enemy_dilation = -1.f;
    std::uint32_t enemy_hash = 0;
    float closest_distance = INFINITY;
    game::EnemyRow enemies[64] = {};
    const int enemy_count = game::GetEnemyRows(enemies, 64);
    for (int i = 0; i < enemy_count; ++i) {
        if (!enemies[i].active || enemies[i].health <= 0.5f ||
            enemies[i].distance < 0.f || enemies[i].distance >= closest_distance)
            continue;
        ue::UObject* enemy = game::FindEnemyByHash(enemies[i].hash);
        if (!enemy) continue;
        closest_distance = enemies[i].distance;
        enemy_hash = enemies[i].hash;
        enemy_dilation = -1.f;
        ReadFloatAt(enemy, offsets::M_AActor_CustomTimeDilation, &enemy_dilation);
    }

    static ue::UObject* gameplay_statics = nullptr;
    if (!gameplay_statics)
        gameplay_statics = ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
    float world_time = -1.f;
    float real_time = -1.f;
    bool have_world_time = false;
    bool have_real_time = false;
    if (gameplay_statics) {
        WorldContextFloat params = {world, 0.f};
        if (ue::CallFunction(gameplay_statics, L"GetTimeSeconds", &params)) {
            world_time = params.ReturnValue;
            have_world_time = true;
        }
        params.ReturnValue = 0.f;
        if (ue::CallFunction(gameplay_statics, L"GetRealTimeSeconds", &params)) {
            real_time = params.ReturnValue;
            have_real_time = true;
        }
    }

    static bool have_previous = false;
    static float previous_world_time = 0.f;
    static float previous_real_time = 0.f;
    static ULONGLONG previous_wall_ms = 0;
    float world_delta = -1.f;
    float real_delta = -1.f;
    float wall_delta = -1.f;
    if (have_previous && have_world_time && have_real_time) {
        world_delta = world_time - previous_world_time;
        real_delta = real_time - previous_real_time;
        wall_delta = static_cast<float>(now - previous_wall_ms) / 1000.f;
        if (world_delta < 0.f || real_delta < 0.f)
            world_delta = real_delta = wall_delta = -1.f;
    }
    if (have_world_time && have_real_time) {
        previous_world_time = world_time;
        previous_real_time = real_time;
        previous_wall_ms = now;
        have_previous = true;
    }

    const float world_per_real = world_delta >= 0.f && real_delta > 0.001f
                                     ? world_delta / real_delta : -1.f;
    const float world_per_wall = world_delta >= 0.f && wall_delta > 0.001f
                                     ? world_delta / wall_delta : -1.f;
    const float component_product = time_dilation >= 0.f && matinee_dilation >= 0.f &&
                                            demo_dilation >= 0.f
                                        ? time_dilation * matinee_dilation * demo_dilation
                                        : -1.f;
    const float effective = original_effective >= 0.f
                                ? original_effective * multiplier : -1.f;
    SYSTEMTIME timestamp = {};
    GetLocalTime(&timestamp);
    SC_LOG("tas: diag timestamp=%04u-%02u-%02uT%02u:%02u:%02u.%03u preset=%s paused=%u world_tick_dt=%.5f world_time=%.3f real_time=%.3f dt_world=%.4f dt_real=%.4f dt_wall=%.4f world_per_real=%.3f world_per_wall=%.3f dilation={time=%.4f matinee=%.4f demo=%.4f product=%.4f original=%.4f multiplier=%.1f effective=%.4f} actor={player=%.3f enemy_hash=%08X enemy=%.3f}",
           timestamp.wYear, timestamp.wMonth, timestamp.wDay, timestamp.wHour,
           timestamp.wMinute, timestamp.wSecond, timestamp.wMilliseconds,
           PresetName(multiplier), g_paused.load(std::memory_order_relaxed) ? 1u : 0u,
           g_last_world_tick_delta.load(std::memory_order_relaxed), world_time, real_time,
           world_delta, real_delta, wall_delta, world_per_real, world_per_wall,
           time_dilation, matinee_dilation, demo_dilation, component_product,
           original_effective, multiplier, effective, player_dilation, enemy_hash,
           enemy_dilation);
}

void PollTASKeys() {
    const bool focused = GameFocused() && !ui::IsBindingCaptureActive();
    for (int i = 0; i < static_cast<int>(tas::Action::Count); ++i) {
        const auto action = static_cast<tas::Action>(i);
        if (tas::WasActionPressed(action, focused)) ActivateAction(action);
    }
}

void RefreshGameplayControlState() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const char* reason = nullptr;
    const bool supported = game::SupportedTASGameplayState(world, player, &reason);
    const bool previous = g_gameplay_supported.exchange(supported, std::memory_order_relaxed);
    if (supported) {
        if (!previous) SC_LOG("tas: controls enabled: supported gameplay");
        return;
    }

    g_steps_requested.store(0, std::memory_order_relaxed);
    g_gui_pause_owned = false;
    if (g_paused.exchange(false, std::memory_order_relaxed)) {
        g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
        SC_LOG("tas: pause released outside supported gameplay");
    }
    if (previous)
        SC_LOG("tas: controls disabled: %s; effective speed 1.0x",
               reason ? reason : "gameplay state unavailable");
}

void __fastcall WorldTickHook(void* self, int tick_type, float delta_seconds) {
    const bool current_world = self == ue::GetWorld();
    const bool will_execute = !g_paused.load(std::memory_order_relaxed) || !current_world;
    tas::input_trace::WorldTickBegin(self, tick_type, delta_seconds, will_execute);
    if (current_world)
        g_last_world_tick_delta.store(delta_seconds, std::memory_order_relaxed);
    if (!g_paused.load(std::memory_order_relaxed) || !current_world) {
        g_original_world_tick(self, tick_type, delta_seconds);
        tas::input_trace::WorldTickEnd(self, tick_type, delta_seconds, true);
        return;
    }

    int requested = g_steps_requested.load(std::memory_order_relaxed);
    while (requested > 0 && !g_steps_requested.compare_exchange_weak(
                                requested, requested - 1, std::memory_order_relaxed)) {}
    if (requested <= 0) {
        tas::input_trace::WorldTickEnd(self, tick_type, delta_seconds, false);
        return;
    }
    g_step_in_progress.store(true, std::memory_order_relaxed);
    g_original_world_tick(self, tick_type, kFrameStepSeconds);
    g_step_in_progress.store(false, std::memory_order_relaxed);
    tas::input_trace::WorldTickEnd(self, tick_type, kFrameStepSeconds, true);
    ++g_steps;
    SC_LOG("tas: step %llu", g_steps);
}


TickFn g_original_tick = nullptr;
void** g_vtable = nullptr;
int g_tick_slot = -1;

std::atomic<unsigned long long> g_frames{0};

void OnFrame() {
    const auto frame = ++g_frames;
    sifucoop::game::TickReplayRecording();

    // The socket is pumped before anything that needs a world, so menus and
    // level loads keep acknowledging, pinging and receiving.
    LONGLONG start = PerfNow();
    sifucoop::net::PumpNetwork();
    AddPerf(PerfNetwork, PerfNow() - start);

    start = PerfNow();
    sifucoop::game::PreparePuppetLifecycle();
    AddPerf(PerfPuppetLifecycle, PerfNow() - start);

    start = PerfNow();
    sifucoop::game::TickEnemies();
    AddPerf(PerfEnemies, PerfNow() - start);
    start = PerfNow();
    sifucoop::game::TickPuppet();
    AddPerf(PerfPuppet, PerfNow() - start);
    start = PerfNow();
    sifucoop::game::TickSelfTest();
    AddPerf(PerfSelfTest, PerfNow() - start);
    start = PerfNow();
    sifucoop::tas::checkpoint::ProcessRequests();
    AddPerf(PerfCheckpoint, PerfNow() - start);
    sifucoop::tas::checkpoint::TickCameraDiagnostics();
    start = PerfNow();
    SampleTimeDilation();
    AddPerf(PerfDilation, PerfNow() - start);

    // After everything has moved this frame, so the digest describes the state
    // the player actually saw.
    start = PerfNow();
    sifucoop::trace::Tick();
    AddPerf(PerfTrace, PerfNow() - start);

    if (frame % 30 != 0) return;

    ue::UObject* world = ue::GetWorld();
    if (!world) return;

    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    start = PerfNow();
    sifucoop::game::TickRunState(player);
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
    AddPerf(PerfRunState, PerfNow() - start);
}

float g_frame_delta = 1.f / 60.f;

void __fastcall TickHook(void* self, float delta_seconds, bool idle_mode) {
    if (delta_seconds > 0.f && delta_seconds < 0.25f) {
        g_frame_delta = delta_seconds;
    } else {
        g_frame_delta = 1.f / 60.f;
    }

    RefreshGameplayControlState();
    PollTASKeys();
    tas::input_trace::EngineTickBegin(delta_seconds, idle_mode);
    g_original_tick(self, delta_seconds, idle_mode);
    tas::input_trace::EngineTickEnd();
    if (g_paused.load(std::memory_order_relaxed) ||
        std::fabs(g_tas_multiplier.load(std::memory_order_relaxed) - 1.f) > 0.001f)
        RefreshGameplayControlState();

    const LONGLONG on_frame_start = PerfNow();
    OnFrame();
    AddPerf(PerfFrame, PerfNow() - on_frame_start);
    LogPerfIfDue();
}

}

bool InstallTickHook(std::uintptr_t base, void* gengine) {
    g_base = base;
    QueryPerformanceFrequency(&g_perf_frequency);
    g_last_perf_log_ms = GetTickCount64();
    g_last_memory_log_ms = g_last_perf_log_ms;

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
    if (!offsets::AWorldSettings_GetEffectiveTimeDilation && !offsets::UWorld_Tick &&
        !offsets::AWorldSettings_SetTimeDilation &&
        !offsets::UReplaySystem_SetDemoPlayTimeDilation)
        return true;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        SC_LOG("tas: MinHook init failed (%d)", static_cast<int>(init));
        return true;
    }

    if (!game::InstallPlayerControllerInputHook())
        SC_LOG("inputtrace: APlayerController::InputKey hook unavailable");
    tas::input_trace::InstallHooks(base);

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

    if (offsets::AWorldSettings_SetTimeDilation) {
        g_set_time_dilation_target =
            reinterpret_cast<void*>(base + offsets::AWorldSettings_SetTimeDilation);
        MH_STATUS status = MH_CreateHook(
            g_set_time_dilation_target, reinterpret_cast<void*>(&SetTimeDilationHook),
            reinterpret_cast<void**>(&g_original_set_time_dilation));
        if (status == MH_OK) status = MH_EnableHook(g_set_time_dilation_target);
        if (status != MH_OK) {
            SC_LOG("tas: SetTimeDilation diagnostic hook failed (%d)",
                   static_cast<int>(status));
            MH_RemoveHook(g_set_time_dilation_target);
            g_original_set_time_dilation = nullptr;
            g_set_time_dilation_target = nullptr;
        } else {
            SC_LOG("tas: SetTimeDilation diagnostic hook installed");
        }
    }

    if (offsets::UReplaySystem_SetDemoPlayTimeDilation) {
        g_set_demo_play_time_dilation_target =
            reinterpret_cast<void*>(base + offsets::UReplaySystem_SetDemoPlayTimeDilation);
        MH_STATUS status = MH_CreateHook(
            g_set_demo_play_time_dilation_target,
            reinterpret_cast<void*>(&SetDemoPlayTimeDilationHook),
            reinterpret_cast<void**>(&g_original_set_demo_play_time_dilation));
        if (status == MH_OK) status = MH_EnableHook(g_set_demo_play_time_dilation_target);
        if (status != MH_OK) {
            SC_LOG("tas: DemoPlayTimeDilation diagnostic hook failed (%d)",
                   static_cast<int>(status));
            MH_RemoveHook(g_set_demo_play_time_dilation_target);
            g_original_set_demo_play_time_dilation = nullptr;
            g_set_demo_play_time_dilation_target = nullptr;
        } else {
            SC_LOG("tas: DemoPlayTimeDilation diagnostic hook installed");
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
    tas::input_trace::RemoveHooks();
    if (g_set_demo_play_time_dilation_target) {
        MH_DisableHook(g_set_demo_play_time_dilation_target);
        MH_RemoveHook(g_set_demo_play_time_dilation_target);
        g_set_demo_play_time_dilation_target = nullptr;
        g_original_set_demo_play_time_dilation = nullptr;
    }
    if (g_set_time_dilation_target) {
        MH_DisableHook(g_set_time_dilation_target);
        MH_RemoveHook(g_set_time_dilation_target);
        g_set_time_dilation_target = nullptr;
        g_original_set_time_dilation = nullptr;
    }
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

unsigned long long FrameCount() { return g_frames.load(std::memory_order_relaxed); }

void ActivateAction(tas::Action action) {
    switch (action) {
        case tas::Action::PauseResume: {
            if (!g_original_world_tick ||
                !g_gameplay_supported.load(std::memory_order_relaxed)) return;
            bool previous = g_paused.load(std::memory_order_relaxed);
            while (!g_paused.compare_exchange_weak(previous, !previous,
                                                   std::memory_order_relaxed)) {}
            const bool paused = !previous;
            g_steps_requested.store(0, std::memory_order_relaxed);
            g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
            SC_LOG("tas: %s", paused ? "paused" : "resumed");
            break;
        }
        case tas::Action::FrameAdvance:
            if (g_gameplay_supported.load(std::memory_order_relaxed) &&
                g_paused.load(std::memory_order_relaxed) && g_original_world_tick) {
                const int count = tas::GetSettings().frames_per_step;
                int pending = g_steps_requested.load(std::memory_order_relaxed);
                while (!g_steps_requested.compare_exchange_weak(
                    pending, pending + count, std::memory_order_relaxed)) {}
            }
            break;
        case tas::Action::SpeedHalf:
        case tas::Action::SpeedFifth:
        case tas::Action::SpeedTenth: {
            if (!g_gameplay_supported.load(std::memory_order_relaxed)) return;
            const int index = static_cast<int>(action) - static_cast<int>(tas::Action::SpeedHalf);
            const float requested = tas::GetSettings().speeds[index];
            const float current = g_tas_multiplier.load(std::memory_order_relaxed);
            const float next = std::fabs(current - requested) < 0.001f ? 1.f : requested;
            g_tas_multiplier.store(next, std::memory_order_relaxed);
            SC_LOG("tas: multiplier selected %s (%.3fx)", PresetName(next), next);
            break;
        }
        case tas::Action::ToggleGui:
            if (!ui::IsMenuOpen()) {
                g_gui_pause_owned = false;
                if (!g_paused.load(std::memory_order_relaxed)) {
                    if (g_gameplay_supported.load(std::memory_order_relaxed)) {
                        ActivateAction(tas::Action::PauseResume);
                        if (g_paused.load(std::memory_order_relaxed)) {
                            g_gui_pause_owned = true;
                            g_gui_pause_serial =
                                g_pause_change_serial.load(std::memory_order_relaxed);
                        }
                    } else {
                        SC_LOG("tas: GUI opened without auto-pause: gameplay state unavailable");
                    }
                }
            } else {
                const bool resume =
                    g_gui_pause_owned && g_paused.load(std::memory_order_relaxed) &&
                    g_pause_change_serial.load(std::memory_order_relaxed) ==
                        g_gui_pause_serial;
                g_gui_pause_owned = false;
                ui::ToggleMenu();
                if (resume) ActivateAction(tas::Action::PauseResume);
                break;
            }
            ui::ToggleMenu();
            break;
        case tas::Action::SaveCheckpoint:
            tas::checkpoint::QueueSave();
            break;
        case tas::Action::LoadCheckpoint:
            tas::checkpoint::QueueLoad();
            break;
        case tas::Action::ToggleNativeRecording:
            sifucoop::game::QueueReplayRecordingToggle();
            break;
        case tas::Action::Count:
            break;
    }
}

float PlaybackSpeed() { return g_tas_multiplier.load(std::memory_order_relaxed); }

void SetPlaybackSpeed(float speed) {
    if (!g_gameplay_supported.load(std::memory_order_relaxed) ||
        !std::isfinite(speed) || speed < 0.01f || speed > 4.f) return;
    g_tas_multiplier.store(speed, std::memory_order_relaxed);
}

bool IsPaused() { return g_paused.load(std::memory_order_relaxed); }

bool IsGameplaySupported() {
    return g_gameplay_supported.load(std::memory_order_relaxed);
}

bool ReleaseGuiOwnedPauseForRecording() {
    if (!g_paused.load(std::memory_order_relaxed)) return true;
    if (!g_gui_pause_owned ||
        g_pause_change_serial.load(std::memory_order_relaxed) != g_gui_pause_serial)
        return false;
    bool expected = true;
    if (!g_paused.compare_exchange_strong(expected, false, std::memory_order_relaxed))
        return !expected;
    g_steps_requested.store(0, std::memory_order_relaxed);
    g_pause_change_serial.fetch_add(1, std::memory_order_relaxed);
    g_gui_pause_owned = false;
    SC_LOG("tas: GUI-owned pause released for native replay recording");
    return true;
}

}
