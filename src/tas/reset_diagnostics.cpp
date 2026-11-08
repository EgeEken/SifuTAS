#include "reset_diagnostics.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../game/actors.h"
#include "../game/enemies.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::reset_diagnostics {
namespace {

namespace game = sifucoop::game;
namespace ue = sifucoop::ue;
namespace offsets = sifucoop::offsets;

constexpr std::uint32_t kTraceLimit = 2048;
constexpr ULONGLONG kTraceDurationMs = 5000;
constexpr ULONGLONG kStabilityTimeoutMs = 10000;
constexpr std::uint32_t kStableSamplesRequired = 3;
constexpr int kActorLimit = 64;

using ProcessEventFn = void(__fastcall*)(ue::UObject*, ue::UFunction*, void*);
using StaticClassFn = ue::UObject*(__fastcall*)();

struct TraceEvent {
    std::uint64_t order = 0;
    std::uint64_t qpc = 0;
    DWORD thread_id = 0;
    std::uintptr_t object = 0;
    std::uintptr_t klass = 0;
    std::uintptr_t function = 0;
    char class_path[160] = {};
    char function_path[192] = {};
};

struct ActorSample {
    char identity[512] = {};
    char class_path[256] = {};
    char montage_path[256] = {};
    std::uintptr_t object = 0;
    std::uintptr_t attack = 0;
    std::uintptr_t defense = 0;
    std::uintptr_t health_component = 0;
    std::uintptr_t ai = 0;
    std::uintptr_t movement = 0;
    std::uintptr_t order = 0;
    ue::FVector location = {};
    ue::FRotator rotation = {};
    ue::FVector velocity = {};
    float health = 0.f;
    float guard = 0.f;
    float custom_dilation = 1.f;
    float attack_danger_timeout = 0.f;
    float movement_max_speed = 0.f;
    float cinematic_weight = 0.f;
    float spawner_respawn_time = 0.f;
    float spawner_spawn_delay = 0.f;
    std::int32_t ai_phase = -1;
    std::int32_t ai_last_phase = -1;
    std::int32_t situation_last_man_count = -1;
    std::int32_t spawner_kill_charges = -1;
    std::int32_t spawner_death_counter_delta = -1;
    std::uint8_t attack_danger_state = 0xff;
    bool player = false;
    bool situation = false;
    bool spawner = false;
    bool has_location = false;
    bool has_velocity = false;
    bool has_custom_dilation = false;
    bool has_health = false;
    bool has_guard = false;
    bool has_attack_fields = false;
    bool has_ai_fields = false;
    bool has_movement = false;
    bool has_cinematic_weight = false;
    bool has_situation_fields = false;
    bool has_spawner_fields = false;
};

struct ActorSnapshot {
    bool valid = false;
    bool match_state_available = false;
    std::uintptr_t world = 0;
    ue::FName match_state = {};
    char world_path[512] = {};
    std::uint32_t count = 0;
    ActorSample actors[kActorLimit] = {};
};

TraceEvent g_trace[kTraceLimit] = {};
std::atomic<TraceState> g_trace_state{TraceState::Ready};
std::atomic<std::uint32_t> g_trace_reserved{0};
std::atomic<std::uint32_t> g_trace_dropped{0};
std::atomic<std::uint32_t> g_active_writers{0};
std::atomic<bool> g_capture_requested{false};
std::atomic<bool> g_trace_resolving{false};
std::atomic<std::uint64_t> g_trace_start_qpc{0};
std::atomic<ULONGLONG> g_trace_start_ms{0};
std::atomic<std::uint64_t> g_qpc_frequency{0};
std::atomic<void*> g_process_event_target{nullptr};
ProcessEventFn g_process_event = nullptr;
std::uintptr_t g_base = 0;
HANDLE g_writer_thread = nullptr;
SRWLOCK g_status_lock = SRWLOCK_INIT;
SRWLOCK g_writer_lock = SRWLOCK_INIT;
Status g_status = {};
ActorSnapshot g_baseline = {};
ActorSnapshot g_previous_sample = {};
ActorSnapshot g_comparison_sample = {};
bool g_comparison_pending = false;
ULONGLONG g_comparison_start_ms = 0;
std::uint32_t g_stable_samples = 0;
std::atomic<bool> g_baseline_requested{false};
std::atomic<bool> g_compare_requested{false};
thread_local bool g_in_capture_hook = false;

template <typename T>
bool ReadField(ue::UObject* object, std::uint32_t offset, T* out) {
    if (!object || !offset || !out) return false;
    const auto* field = reinterpret_cast<const std::uint8_t*>(object) + offset;
    if (!ue::RangeReadable(field, sizeof(T))) return false;
    std::memcpy(out, field, sizeof(T));
    return true;
}

void SetResult(const char* result) {
    AcquireSRWLockExclusive(&g_status_lock);
    lstrcpynA(g_status.last_result, result, sizeof(g_status.last_result));
    ReleaseSRWLockExclusive(&g_status_lock);
}

void WaitForWriter() {
    AcquireSRWLockExclusive(&g_writer_lock);
    if (!g_writer_thread) {
        ReleaseSRWLockExclusive(&g_writer_lock);
        return;
    }
    WaitForSingleObject(g_writer_thread, INFINITE);
    CloseHandle(g_writer_thread);
    g_writer_thread = nullptr;
    ReleaseSRWLockExclusive(&g_writer_lock);
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.writer_active = false;
    ReleaseSRWLockExclusive(&g_status_lock);
}

void PollWriter() {
    AcquireSRWLockExclusive(&g_writer_lock);
    if (!g_writer_thread) {
        ReleaseSRWLockExclusive(&g_writer_lock);
        return;
    }
    DWORD exit_code = STILL_ACTIVE;
    if (GetExitCodeThread(g_writer_thread, &exit_code) && exit_code != STILL_ACTIVE) {
        CloseHandle(g_writer_thread);
        g_writer_thread = nullptr;
        ReleaseSRWLockExclusive(&g_writer_lock);
        AcquireSRWLockExclusive(&g_status_lock);
        g_status.writer_active = false;
        ReleaseSRWLockExclusive(&g_status_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_writer_lock);
}

void __fastcall ProcessEventHook(ue::UObject* self, ue::UFunction* function,
                                void* params) {
    if (!g_in_capture_hook &&
        g_trace_state.load(std::memory_order_acquire) == TraceState::Capturing) {
        g_in_capture_hook = true;
        g_active_writers.fetch_add(1, std::memory_order_acq_rel);
        if (g_trace_state.load(std::memory_order_acquire) == TraceState::Capturing) {
            const std::uint32_t index = g_trace_reserved.fetch_add(1, std::memory_order_relaxed);
            if (index < kTraceLimit) {
                TraceEvent& event = g_trace[index];
                event.order = static_cast<std::uint64_t>(index) + 1;
                LARGE_INTEGER now = {};
                QueryPerformanceCounter(&now);
                event.qpc = static_cast<std::uint64_t>(now.QuadPart);
                event.thread_id = GetCurrentThreadId();
                event.object = reinterpret_cast<std::uintptr_t>(self);
                event.function = reinterpret_cast<std::uintptr_t>(function);
                event.klass = 0;
                if (self)
                    std::memcpy(&event.klass,
                                reinterpret_cast<const std::uint8_t*>(self) + 0x10,
                                sizeof(event.klass));
            } else {
                g_trace_dropped.fetch_add(1, std::memory_order_relaxed);
                TraceState expected = TraceState::Capturing;
                g_trace_state.compare_exchange_strong(expected, TraceState::Complete,
                                                       std::memory_order_acq_rel);
            }
        }
        g_active_writers.fetch_sub(1, std::memory_order_release);
        g_in_capture_hook = false;
    }
    if (g_process_event) g_process_event(self, function, params);
}

bool InstallProcessEvent(std::uintptr_t base) {
    if (!offsets::UObject_ProcessEvent) return false;
    void* target = reinterpret_cast<void*>(base + offsets::UObject_ProcessEvent);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventHook),
                                     reinterpret_cast<void**>(&g_process_event));
    if (status != MH_OK) {
        SC_LOG("tas: reset diagnostics ProcessEvent hook failed (%d)",
               static_cast<int>(status));
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);
        g_process_event = nullptr;
        SC_LOG("tas: reset diagnostics ProcessEvent hook enable failed (%d)",
               static_cast<int>(status));
        return false;
    }
    g_process_event_target.store(target, std::memory_order_release);
    SC_LOG("tas: reset diagnostics ProcessEvent hook installed; capture is opt-in");
    return true;
}

ue::UObject* StaticClass(std::uint32_t rva) {
    if (!rva) return nullptr;
    auto fn = reinterpret_cast<StaticClassFn>(g_base + rva);
    return fn();
}

ue::UObject* ComponentByClass(ue::UObject* actor, ue::UObject* component_class) {
    if (!ue::IsAliveObject(actor) || !component_class) return nullptr;
    struct Params { ue::UObject* component_class; ue::UObject* result; } params = {};
    params.component_class = component_class;
    if (!ue::CallFunction(actor, L"GetComponentByClass", &params) ||
        !ue::IsAliveObject(params.result))
        return nullptr;
    return params.result;
}

void SampleActor(ActorSample* sample, ue::UObject* actor, bool player,
                 bool situation, bool spawner) {
    if (!sample || !ue::IsAliveObject(actor)) return;
    sample->object = reinterpret_cast<std::uintptr_t>(actor);
    sample->player = player;
    sample->situation = situation;
    sample->spawner = spawner;
    if (!ue::GetObjectPathName(actor, sample->identity, sizeof(sample->identity)))
        lstrcpynA(sample->identity, "unavailable", sizeof(sample->identity));
    if (!ue::GetObjectClassPathName(actor, sample->class_path, sizeof(sample->class_path)))
        lstrcpynA(sample->class_path, "unavailable", sizeof(sample->class_path));

    sample->has_location = ue::GetActorLocation(actor, &sample->location) &&
                           ue::GetActorRotation(actor, &sample->rotation);
    sample->has_velocity = game::GetActorVelocity(actor, &sample->velocity);
    game::Fighter fighter = game::ResolveFighter(actor);
    if (fighter.health) {
        sample->health = game::GetHealth(fighter);
        sample->has_health = std::isfinite(sample->health);
    }
    if (fighter.defense) {
        sample->guard = game::GetGuard(fighter);
        sample->has_guard = std::isfinite(sample->guard);
    }
    sample->attack = reinterpret_cast<std::uintptr_t>(fighter.attack);
    sample->defense = reinterpret_cast<std::uintptr_t>(fighter.defense);
    sample->health_component = reinterpret_cast<std::uintptr_t>(fighter.health);

    sample->has_custom_dilation = ReadField(
        actor, offsets::M_AActor_CustomTimeDilation, &sample->custom_dilation);
    if (fighter.attack) {
        sample->has_attack_fields =
            ReadField(fighter.attack, offsets::M_UAttackComponent_fDangerTimeOut,
                      &sample->attack_danger_timeout) &&
            ReadField(fighter.attack, offsets::M_UAttackComponent_eDangerState,
                      &sample->attack_danger_state);
    }

    if (actor && !situation && !spawner) {
        sample->ai = reinterpret_cast<std::uintptr_t>(ComponentByClass(
            actor, StaticClass(offsets::UAIFightingComponent_StaticClass)));
        sample->movement = reinterpret_cast<std::uintptr_t>(ComponentByClass(
            actor, StaticClass(offsets::UCharacterMovementComponent_StaticClass)));
        sample->order = reinterpret_cast<std::uintptr_t>(ComponentByClass(
            actor, StaticClass(offsets::UOrderComponent_StaticClass)));
        auto* ai = reinterpret_cast<ue::UObject*>(sample->ai);
        sample->has_ai_fields = ai &&
            ReadField(ai, offsets::M_UAIFightingComponent_iCurrentPhaseNodeIndex,
                      &sample->ai_phase) &&
            ReadField(ai, offsets::M_UAIFightingComponent_iLastPhaseNodeIndex,
                      &sample->ai_last_phase);
        auto* movement = reinterpret_cast<ue::UObject*>(sample->movement);
        sample->has_movement = movement && ReadField(
            movement, offsets::M_UCharacterMovementComponent_MaxWalkSpeed,
            &sample->movement_max_speed);
        ue::AnimState animation = {};
        if (ue::ReadAnimState(actor, &animation) && animation.montage &&
            ue::IsAliveObject(animation.montage))
            ue::GetObjectPathName(animation.montage, sample->montage_path,
                                  sizeof(sample->montage_path));
        ue::UObject* anim_instance = ue::GetAnimInstance(actor);
        if (anim_instance && offsets::M_USCAnimInstance_fCinematicOverallWeight)
            sample->has_cinematic_weight = ReadField(
                anim_instance, offsets::M_USCAnimInstance_fCinematicOverallWeight,
                &sample->cinematic_weight);
    }
    if (situation)
        sample->has_situation_fields = ReadField(
            actor, offsets::M_AAISituationActor_iCurrentLastManOccuranceCount,
            &sample->situation_last_man_count);
    if (spawner)
        sample->has_spawner_fields =
            ReadField(actor, offsets::M_AAISpawner_fRespawnTime,
                      &sample->spawner_respawn_time) &&
            ReadField(actor, offsets::M_AAISpawner_fSpawnDelay,
                      &sample->spawner_spawn_delay) &&
            ReadField(actor, offsets::M_AAISpawner_iChargesToAddWhenKilled,
                      &sample->spawner_kill_charges) &&
            ReadField(actor, offsets::M_AAISpawner_iDeathCounterDecreaseWhenKilled,
                      &sample->spawner_death_counter_delta);
}

bool AddActor(ActorSnapshot* snapshot, ue::UObject* actor, bool player,
              bool situation, bool spawner) {
    if (!actor || snapshot->count >= kActorLimit) return false;
    ActorSample& sample = snapshot->actors[snapshot->count];
    SampleActor(&sample, actor, player, situation, spawner);
    if (!sample.object) return false;
    ++snapshot->count;
    return true;
}

ActorSnapshot CaptureActors() {
    ActorSnapshot snapshot = {};
    ue::UObject* world = ue::GetWorld();
    if (!ue::IsAliveObject(world)) return snapshot;
    snapshot.world = reinterpret_cast<std::uintptr_t>(world);
    if (!ue::GetObjectPathName(world, snapshot.world_path, sizeof(snapshot.world_path)))
        lstrcpynA(snapshot.world_path, "unavailable", sizeof(snapshot.world_path));
    snapshot.match_state_available = ue::GetWorldMatchState(world, &snapshot.match_state);

    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    AddActor(&snapshot, player, true, false, false);

    ue::UObject* fighters[32] = {};
    int fighter_count = 0;
    int fighter_total = 0;
    if (game::EnumerateWorldFighters(fighters, 32, &fighter_count, &fighter_total)) {
        for (int i = 0; i < fighter_count; ++i)
            if (fighters[i] != player) AddActor(&snapshot, fighters[i], false, false, false);
    }

    ue::UObject* situations[16] = {};
    int count = 0;
    int total = 0;
    if (game::EnumerateActorsOfClass(StaticClass(offsets::AAISituationActor_StaticClass),
                                     situations, 16, &count, &total)) {
        for (int i = 0; i < count; ++i) AddActor(&snapshot, situations[i], false, true, false);
    }
    ue::UObject* spawners[16] = {};
    if (game::EnumerateActorsOfClass(StaticClass(offsets::AAISpawner_StaticClass),
                                     spawners, 16, &count, &total)) {
        for (int i = 0; i < count; ++i) AddActor(&snapshot, spawners[i], false, false, true);
    }
    snapshot.valid = true;
    return snapshot;
}

void LogSample(const char* stage, const ActorSample& a) {
    SC_LOG("tas: reset-diff stage=%s role=%s%s%s name=%s class=%s object=%p "
           "attack=%p defense=%p health_component=%p ai=%p movement=%p order=%p "
           "loc=%s%.2f,%.2f,%.2f rot=%.2f,%.2f,%.2f vel=%s%.2f,%.2f,%.2f "
           "health=%s%.2f guard=%s%.2f custom_dilation=%s%.3f montage=%s "
           "attack_danger=%s%.2f/%u ai_phase=%s%d/%d max_walk=%s%.2f "
           "cinematic_weight=%s%.3f situation_last_man=%s%d "
           "spawner_timing=%s%.2f/%.2f spawner_kill_values=%d/%d",
           stage, a.player ? "player" : "actor", a.situation ? ":situation" : "",
           a.spawner ? ":spawner" : "", a.identity, a.class_path,
           reinterpret_cast<void*>(a.object), reinterpret_cast<void*>(a.attack),
           reinterpret_cast<void*>(a.defense), reinterpret_cast<void*>(a.health_component),
           reinterpret_cast<void*>(a.ai), reinterpret_cast<void*>(a.movement),
           reinterpret_cast<void*>(a.order), a.has_location ? "" : "n/a ", a.location.X,
           a.location.Y, a.location.Z, a.rotation.Pitch, a.rotation.Yaw, a.rotation.Roll,
           a.has_velocity ? "" : "n/a ", a.velocity.X, a.velocity.Y, a.velocity.Z,
           a.has_health ? "" : "n/a ", a.health, a.has_guard ? "" : "n/a ", a.guard,
           a.has_custom_dilation ? "" : "n/a ", a.custom_dilation,
           a.montage_path[0] ? a.montage_path : "none",
           a.has_attack_fields ? "" : "n/a ", a.attack_danger_timeout,
           static_cast<unsigned>(a.attack_danger_state), a.has_ai_fields ? "" : "n/a ",
           a.ai_phase, a.ai_last_phase, a.has_movement ? "" : "n/a ",
           a.movement_max_speed, a.has_cinematic_weight ? "" : "n/a ",
           a.cinematic_weight, a.has_situation_fields ? "" : "n/a ",
           a.situation_last_man_count, a.has_spawner_fields ? "" : "n/a ",
           a.spawner_respawn_time, a.spawner_spawn_delay, a.spawner_kill_charges,
           a.spawner_death_counter_delta);
}

void CaptureBaseline() {
    g_baseline = CaptureActors();
    if (!g_baseline.valid) {
        SetResult("Baseline refused: current world is unavailable.");
        return;
    }
    SC_LOG("tas: reset-diff baseline world=%s world_object=%p match_state=%s%u:%d actors=%u",
           g_baseline.world_path, reinterpret_cast<void*>(g_baseline.world),
           g_baseline.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(g_baseline.match_state.comparison_index),
           g_baseline.match_state.number,
           g_baseline.count);
    for (std::uint32_t i = 0; i < g_baseline.count; ++i)
        LogSample("before", g_baseline.actors[i]);
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.baseline_saved = true;
    ReleaseSRWLockExclusive(&g_status_lock);
    SetResult("Baseline captured. Reset the situation manually, then compare.");
}

void CompareActors(const ActorSnapshot& current) {
    if (!g_baseline.valid) {
        SetResult("Comparison refused: capture a baseline first.");
        return;
    }
    SC_LOG("tas: reset-diff compare before_world=%s before_world_object=%p "
           "before_match_state=%s%u:%d after_world=%s after_world_object=%p "
           "after_match_state=%s%u:%d same_world=%u before=%u after=%u",
           g_baseline.world_path, reinterpret_cast<void*>(g_baseline.world),
           g_baseline.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(g_baseline.match_state.comparison_index),
           g_baseline.match_state.number,
           current.world_path, reinterpret_cast<void*>(current.world),
           current.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(current.match_state.comparison_index),
           current.match_state.number,
           current.world == g_baseline.world ? 1u : 0u, g_baseline.count, current.count);
    std::uint32_t matched = 0;
    std::uint32_t recreated = 0;
    std::uint32_t removed = 0;
    for (std::uint32_t i = 0; i < g_baseline.count; ++i) {
        const ActorSample& before = g_baseline.actors[i];
        const ActorSample* after = nullptr;
        for (std::uint32_t j = 0; j < current.count; ++j) {
            if (!std::strcmp(before.identity, current.actors[j].identity) &&
                !std::strcmp(before.class_path, current.actors[j].class_path)) {
                after = &current.actors[j];
                break;
            }
        }
        if (!after) {
            ++removed;
            SC_LOG("tas: reset-diff removed name=%s class=%s old_object=%p",
                   before.identity, before.class_path, reinterpret_cast<void*>(before.object));
            continue;
        }
        ++matched;
        LogSample("after", *after);
        if (before.object != after->object) {
            ++recreated;
            SC_LOG("tas: reset-diff identity_recreated name=%s old=%p new=%p",
                   before.identity, reinterpret_cast<void*>(before.object),
                   reinterpret_cast<void*>(after->object));
        }
        SC_LOG("tas: reset-diff matched name=%s object_same=%u "
               "dloc=%.3f,%.3f,%.3f drot=%.3f,%.3f,%.3f dvel=%.3f,%.3f,%.3f "
               "dhealth=%s%.3f dguard=%s%.3f montage_before=%s montage_after=%s "
               "ai_phase_before=%s%d/%d ai_phase_after=%s%d/%d "
               "attack_danger_before=%s%.2f/%u after=%s%.2f/%u "
               "walk_speed_before=%s%.2f after=%s%.2f "
               "cinematic_before=%s%.3f after=%s%.3f "
               "custom_dilation_delta=%s%.3f "
               "situation_count_before=%s%d after=%s%d "
               "spawner_delay_before=%s%.2f/%.2f after=%s%.2f/%.2f",
               before.identity, before.object == after->object ? 1u : 0u,
               after->location.X - before.location.X,
               after->location.Y - before.location.Y,
               after->location.Z - before.location.Z,
               after->rotation.Pitch - before.rotation.Pitch,
               after->rotation.Yaw - before.rotation.Yaw,
               after->rotation.Roll - before.rotation.Roll,
               after->velocity.X - before.velocity.X,
               after->velocity.Y - before.velocity.Y,
               after->velocity.Z - before.velocity.Z,
               before.has_health && after->has_health ? "" : "n/a ",
               after->health - before.health,
               before.has_guard && after->has_guard ? "" : "n/a ",
               after->guard - before.guard,
               before.montage_path[0] ? before.montage_path : "none",
               after->montage_path[0] ? after->montage_path : "none",
               before.has_ai_fields ? "" : "n/a ", before.ai_phase,
               before.ai_last_phase, after->has_ai_fields ? "" : "n/a ",
               after->ai_phase, after->ai_last_phase,
               before.has_attack_fields ? "" : "n/a ", before.attack_danger_timeout,
               static_cast<unsigned>(before.attack_danger_state),
               after->has_attack_fields ? "" : "n/a ", after->attack_danger_timeout,
               static_cast<unsigned>(after->attack_danger_state),
               before.has_movement ? "" : "n/a ", before.movement_max_speed,
               after->has_movement ? "" : "n/a ", after->movement_max_speed,
               before.has_cinematic_weight ? "" : "n/a ", before.cinematic_weight,
               after->has_cinematic_weight ? "" : "n/a ", after->cinematic_weight,
               before.has_custom_dilation && after->has_custom_dilation ? "" : "n/a ",
               after->custom_dilation - before.custom_dilation,
               before.has_situation_fields ? "" : "n/a ",
               before.situation_last_man_count,
               after->has_situation_fields ? "" : "n/a ",
               after->situation_last_man_count,
               before.has_spawner_fields ? "" : "n/a ", before.spawner_respawn_time,
               before.spawner_spawn_delay, after->has_spawner_fields ? "" : "n/a ",
               after->spawner_respawn_time, after->spawner_spawn_delay);
    }
    std::uint32_t added = 0;
    for (std::uint32_t j = 0; j < current.count; ++j) {
        bool found = false;
        for (std::uint32_t i = 0; i < g_baseline.count; ++i)
            if (!std::strcmp(current.actors[j].identity, g_baseline.actors[i].identity) &&
                !std::strcmp(current.actors[j].class_path, g_baseline.actors[i].class_path)) {
                found = true;
                break;
            }
        if (!found) {
            ++added;
            SC_LOG("tas: reset-diff added name=%s class=%s object=%p",
                   current.actors[j].identity, current.actors[j].class_path,
                   reinterpret_cast<void*>(current.actors[j].object));
        }
    }
    SC_LOG("tas: reset-diff summary matched=%u recreated=%u removed=%u added=%u",
           matched, recreated, removed, added);
    SetResult("Read-only comparison complete; see reset-diff records in SifuCoop.log.");
}

bool SameActorRegistry(const ActorSnapshot& a, const ActorSnapshot& b) {
    if (!a.valid || !b.valid || a.world != b.world || a.count != b.count) return false;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        const ActorSample& first = a.actors[i];
        const ActorSample* second = nullptr;
        for (std::uint32_t j = 0; j < b.count; ++j) {
            if (!std::strcmp(first.identity, b.actors[j].identity) &&
                !std::strcmp(first.class_path, b.actors[j].class_path)) {
                second = &b.actors[j];
                break;
            }
        }
        if (!second || first.object != second->object || first.attack != second->attack ||
            first.defense != second->defense || first.health_component != second->health_component ||
            first.ai != second->ai || first.movement != second->movement ||
            first.order != second->order)
            return false;
    }
    return true;
}

void PollComparison() {
    if (!g_comparison_pending) return;
    if (GetTickCount64() - g_comparison_start_ms >= kStabilityTimeoutMs) {
        g_comparison_pending = false;
        AcquireSRWLockExclusive(&g_status_lock);
        g_status.comparison_waiting = false;
        ReleaseSRWLockExclusive(&g_status_lock);
        SetResult("Comparison timed out waiting for a stable world and actor/component registry.");
        return;
    }
    ActorSnapshot current = CaptureActors();
    if (!current.valid) {
        g_stable_samples = 0;
        g_previous_sample = {};
        SetResult("Waiting for current world and actors to become valid after manual reset.");
        return;
    }
    g_stable_samples = SameActorRegistry(g_previous_sample, current)
                           ? g_stable_samples + 1
                           : 1;
    g_previous_sample = current;
    if (g_stable_samples < kStableSamplesRequired) {
        char message[160] = {};
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "Waiting for stable actor/component inventory: %u / %u game-thread samples.",
                    g_stable_samples, kStableSamplesRequired);
        SetResult(message);
        return;
    }
    g_comparison_pending = false;
    g_comparison_sample = current;
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.comparison_waiting = false;
    ReleaseSRWLockExclusive(&g_status_lock);
    CompareActors(g_comparison_sample);
}

void ResolveTraceNames() {
    const std::uint32_t count = (std::min)(g_trace_reserved.load(std::memory_order_acquire),
                                           kTraceLimit);
    for (std::uint32_t i = 0; i < count; ++i) {
        TraceEvent& event = g_trace[i];
        for (std::uint32_t j = 0; j < i; ++j) {
            if (g_trace[j].function == event.function && event.function_path[0]) {
                lstrcpynA(event.function_path, g_trace[j].function_path,
                          sizeof(event.function_path));
                break;
            }
        }
        if (!event.function_path[0]) {
            auto* function = reinterpret_cast<ue::UObject*>(event.function);
            if (ue::IsAliveObject(function))
                ue::GetObjectPathName(function, event.function_path,
                                      sizeof(event.function_path));
            if (!event.function_path[0]) lstrcpynA(event.function_path, "unavailable",
                                                   sizeof(event.function_path));
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (g_trace[j].klass == event.klass && event.class_path[0]) {
                lstrcpynA(event.class_path, g_trace[j].class_path, sizeof(event.class_path));
                break;
            }
        }
        if (!event.class_path[0]) {
            auto* klass = reinterpret_cast<ue::UObject*>(event.klass);
            if (ue::IsAliveObject(klass))
                ue::GetObjectPathName(klass, event.class_path, sizeof(event.class_path));
            if (!event.class_path[0]) lstrcpynA(event.class_path, "unavailable",
                                                sizeof(event.class_path));
        }
    }
}

DWORD WINAPI WriteTrace(LPVOID) {
    const std::uint32_t count = (std::min)(g_trace_reserved.load(std::memory_order_acquire),
                                           kTraceLimit);
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t start = g_trace_start_qpc.load(std::memory_order_relaxed);
    SC_LOG("tas: processevent trace complete events=%u dropped=%u duration_ms=%llu "
           "qpc_frequency=%llu names_resolved_after_capture=1",
           count, g_trace_dropped.load(std::memory_order_relaxed),
           static_cast<unsigned long long>(GetTickCount64() -
               g_trace_start_ms.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(frequency.QuadPart));
    for (std::uint32_t i = 0; i < count; ++i) {
        const TraceEvent& e = g_trace[i];
        SC_LOG("tas: processevent order=%llu qpc_delta=%llu tid=%lu owner=%p class=%s "
               "function=%s function_ptr=%p",
               static_cast<unsigned long long>(e.order),
               static_cast<unsigned long long>(e.qpc >= start ? e.qpc - start : 0),
               e.thread_id, reinterpret_cast<void*>(e.object), e.class_path,
               e.function_path, reinterpret_cast<void*>(e.function));
    }
    return 0;
}

void FinishTrace() {
    if (g_active_writers.load(std::memory_order_acquire) != 0) return;
    bool expected = false;
    if (!g_trace_resolving.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) return;
    ResolveTraceNames();
    WaitForWriter();
    g_writer_thread = CreateThread(nullptr, 0, WriteTrace, nullptr, 0, nullptr);
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.trace_events = (std::min)(g_trace_reserved.load(std::memory_order_relaxed),
                                       kTraceLimit);
    g_status.trace_dropped = g_trace_dropped.load(std::memory_order_relaxed);
    g_status.writer_active = g_writer_thread != nullptr;
    ReleaseSRWLockExclusive(&g_status_lock);
    if (!g_writer_thread) WriteTrace(nullptr);
}

void StartTrace() {
    g_trace_reserved.store(0, std::memory_order_relaxed);
    g_trace_dropped.store(0, std::memory_order_relaxed);
    g_trace_resolving.store(false, std::memory_order_relaxed);
    LARGE_INTEGER frequency = {}, now = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    g_qpc_frequency.store(static_cast<std::uint64_t>(frequency.QuadPart),
                          std::memory_order_relaxed);
    g_trace_start_qpc.store(static_cast<std::uint64_t>(now.QuadPart),
                            std::memory_order_relaxed);
    g_trace_start_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_trace_state.store(TraceState::Capturing, std::memory_order_release);
    SetResult("ProcessEvent trace capturing for up to five seconds.");
}

void LogReflectedInventory() {
    SC_LOG("tas: reflection metadata source=Steam UE4.26 UHT property tables "
           "supported=typed scalar reads only; arrays, maps, object references and writes=excluded");
    struct Property { const char* owner; const char* name; const char* type; std::uint32_t offset; };
    const Property properties[] = {
        {"AActor", "CustomTimeDilation", "float", offsets::M_AActor_CustomTimeDilation},
        {"UHealthComponent", "m_fHealth", "float", offsets::M_UHealthComponent_fHealth},
        {"UDefenseComponent", "m_fCurrentGuard", "float", offsets::M_UDefenseComponent_fCurrentGuard},
        {"UAttackComponent", "m_fDangerTimeOut", "float", offsets::M_UAttackComponent_fDangerTimeOut},
        {"UAttackComponent", "m_eDangerState", "enum8", offsets::M_UAttackComponent_eDangerState},
        {"UAIFightingComponent", "m_iCurrentPhaseNodeIndex", "int32", offsets::M_UAIFightingComponent_iCurrentPhaseNodeIndex},
        {"UAIFightingComponent", "m_iLastPhaseNodeIndex", "int32", offsets::M_UAIFightingComponent_iLastPhaseNodeIndex},
        {"UCharacterMovementComponent", "MaxWalkSpeed", "float", offsets::M_UCharacterMovementComponent_MaxWalkSpeed},
        {"USCAnimInstance", "m_fCinematicOverallWeight", "float", offsets::M_USCAnimInstance_fCinematicOverallWeight},
        {"AAISituationActor", "m_iCurrentLastManOccuranceCount", "int32", offsets::M_AAISituationActor_iCurrentLastManOccuranceCount},
        {"AAISpawner", "m_fRespawnTime", "float", offsets::M_AAISpawner_fRespawnTime},
        {"AAISpawner", "m_fSpawnDelay", "float", offsets::M_AAISpawner_fSpawnDelay},
        {"AAISpawner", "m_iChargesToAddWhenKilled", "int32", offsets::M_AAISpawner_iChargesToAddWhenKilled},
        {"AAISpawner", "m_iDeathCounterDecreaseWhenKilled", "int32", offsets::M_AAISpawner_iDeathCounterDecreaseWhenKilled},
    };
    for (const Property& property : properties)
        SC_LOG("tas: reflection property owner=%s name=%s type=%s offset=0x%X available=%u",
               property.owner, property.name, property.type, property.offset,
               property.offset ? 1u : 0u);
}

}

void InstallHooks(std::uintptr_t module_base) {
    g_base = module_base;
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    g_qpc_frequency.store(static_cast<std::uint64_t>(frequency.QuadPart),
                          std::memory_order_relaxed);
    LogReflectedInventory();
    if (!InstallProcessEvent(module_base)) {
        g_trace_state.store(TraceState::Failed, std::memory_order_release);
        SetResult("ProcessEvent hook unavailable; see SifuCoop.log.");
    }
}

void RemoveHooks() {
    void* target = g_process_event_target.exchange(nullptr, std::memory_order_acq_rel);
    if (target) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    g_process_event = nullptr;
    WaitForWriter();
}

void QueueProcessEventCapture() {
    WaitForWriter();
    TraceState expected = g_trace_state.load(std::memory_order_acquire);
    if (expected == TraceState::Capturing || expected == TraceState::Queued) return;
    g_trace_state.store(TraceState::Queued, std::memory_order_release);
    g_capture_requested.store(true, std::memory_order_release);
}

void QueueBaselineCapture() { g_baseline_requested.store(true, std::memory_order_release); }
void QueueStateComparison() { g_compare_requested.store(true, std::memory_order_release); }

Status GetStatus() {
    Status status = {};
    status.trace_state = g_trace_state.load(std::memory_order_acquire);
    status.trace_events = (std::min)(g_trace_reserved.load(std::memory_order_relaxed),
                                     kTraceLimit);
    status.trace_dropped = g_trace_dropped.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_status_lock);
    status.writer_active = g_status.writer_active;
    status.baseline_saved = g_status.baseline_saved;
    status.comparison_waiting = g_status.comparison_waiting;
    lstrcpynA(status.last_result, g_status.last_result, sizeof(status.last_result));
    ReleaseSRWLockShared(&g_status_lock);
    return status;
}

void Tick() {
    PollWriter();
    if (g_baseline_requested.exchange(false, std::memory_order_acq_rel)) CaptureBaseline();
    if (g_compare_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_baseline.valid) {
            SetResult("Comparison refused: capture a baseline first.");
        } else {
            g_comparison_pending = true;
            g_comparison_start_ms = GetTickCount64();
            g_stable_samples = 0;
            g_previous_sample = {};
            AcquireSRWLockExclusive(&g_status_lock);
            g_status.comparison_waiting = true;
            ReleaseSRWLockExclusive(&g_status_lock);
            SetResult("Sampling a stable post-reset world/actor/component inventory.");
        }
    }
    PollComparison();
    if (g_capture_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_process_event) {
            g_trace_state.store(TraceState::Failed, std::memory_order_release);
            SetResult("ProcessEvent hook unavailable.");
        } else {
            StartTrace();
        }
    }
    if (g_trace_state.load(std::memory_order_acquire) == TraceState::Capturing &&
        (GetTickCount64() - g_trace_start_ms.load(std::memory_order_relaxed) >=
             kTraceDurationMs ||
         g_trace_reserved.load(std::memory_order_relaxed) >= kTraceLimit)) {
        g_trace_state.store(TraceState::Complete, std::memory_order_release);
        FinishTrace();
    } else if (g_trace_state.load(std::memory_order_acquire) == TraceState::Complete &&
               !g_trace_resolving.load(std::memory_order_acquire)) {
        FinishTrace();
    }
}

}
