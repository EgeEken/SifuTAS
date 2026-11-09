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
#include "../ui/overlay.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::reset_diagnostics {
namespace {

namespace game = sifucoop::game;
namespace ue = sifucoop::ue;
namespace offsets = sifucoop::offsets;

constexpr std::uint32_t kPreEventLimit = 512;
constexpr std::uint32_t kPostEventLimit = 1536;
constexpr std::uint32_t kTraceLimit = kPreEventLimit + kPostEventLimit;
constexpr std::uint32_t kAggregateLimit = 256;
constexpr ULONGLONG kTraceDurationMs = 2000;
constexpr ULONGLONG kStabilityTimeoutMs = 10000;
constexpr std::uint32_t kStableSamplesRequired = 3;
constexpr int kFighterEnumerationLimit = 256;
constexpr int kActiveFighterLimit = 64;
constexpr int kInactiveFighterLimit = 16;
constexpr int kPooledFighterLimit = 16;
constexpr int kActorLimit = 129;

using ProcessEventFn = void(__fastcall*)(ue::UObject*, ue::UFunction*, void*);
using StaticClassFn = ue::UObject*(__fastcall*)();

struct RawTraceEvent {
    std::uint64_t order = 0;
    std::uint64_t qpc = 0;
    DWORD thread_id = 0;
    std::uintptr_t object = 0;
    std::uintptr_t klass = 0;
    std::uintptr_t function = 0;
};

struct TraceEvent {
    RawTraceEvent raw = {};
    char class_path[160] = {};
    char function_path[192] = {};
};

struct PairAggregate {
    std::atomic<std::uint64_t> hash{0};
    std::atomic<std::uintptr_t> function{0};
    std::atomic<std::uintptr_t> klass{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> first_qpc{0};
    std::atomic<std::uint64_t> last_qpc{0};
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
    bool inactive = false;
    bool pooled = false;
    bool situation = false;
    bool spawner = false;
    bool required_components_available = false;
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
    std::uint32_t reset_begin_total = 0;
    std::uint32_t reset_return_total = 0;
    std::uint32_t fighter_total = 0;
    std::uint32_t fighter_enumerated = 0;
    std::uint32_t active_fighter_count = 0;
    std::uint32_t inactive_fighter_count = 0;
    std::uint32_t pooled_fighter_count = 0;
    std::uint32_t active_fighter_truncated = 0;
    std::uint32_t inactive_fighter_truncated = 0;
    std::uint32_t pooled_fighter_truncated = 0;
    std::uint32_t invalid_fighters = 0;
    std::uint32_t missing_health_fighters = 0;
    std::uint32_t situation_total = 0;
    std::uint32_t situation_truncated = 0;
    std::uint32_t spawner_total = 0;
    std::uint32_t spawner_truncated = 0;
    bool situation_enumeration_available = false;
    bool spawner_enumeration_available = false;
    std::uint32_t required_components_available = 0;
    std::uint32_t required_components_missing = 0;
    bool fighter_enumeration_available = false;
    std::uint32_t count = 0;
    ActorSample actors[kActorLimit] = {};
};

RawTraceEvent g_pre_ring[kPreEventLimit] = {};
RawTraceEvent g_post_events[kPostEventLimit] = {};
RawTraceEvent g_candidates[kTraceLimit] = {};
TraceEvent g_trace[kTraceLimit] = {};
PairAggregate g_aggregates[kAggregateLimit] = {};
std::atomic_flag g_aggregate_lock = ATOMIC_FLAG_INIT;
std::atomic<TraceState> g_trace_state{TraceState::Ready};
std::atomic<std::uint64_t> g_event_order{0};
std::atomic<std::uint32_t> g_pre_reserved{0};
std::atomic<std::uint32_t> g_post_reserved{0};
std::atomic<std::uint32_t> g_trace_count{0};
std::atomic<std::uint32_t> g_trace_overflow_events{0};
std::atomic<std::uint32_t> g_aggregate_untracked{0};
std::atomic<std::uint32_t> g_trace_pre_overwritten{0};
std::atomic<std::uint32_t> g_active_writers{0};
std::atomic<bool> g_arm_requested{false};
std::atomic<std::uint64_t> g_trigger_order{0};
std::atomic<std::uint32_t> g_trigger_source{0};
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
std::atomic<std::uint32_t> g_reset_begin_total{0};
std::atomic<std::uint32_t> g_reset_return_total{0};
std::atomic<std::uint32_t> g_reset_last_kind{0};
std::atomic<std::uintptr_t> g_reset_last_caller{0};
bool g_reset_observed_since_baseline = false;
bool g_reset_returned_since_baseline = false;
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

void AggregateOverflow(const RawTraceEvent& event) {
    while (g_aggregate_lock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    const std::uint64_t fn = event.function;
    const std::uint64_t klass = event.klass;
    std::uint64_t hash = (fn * 0x9E3779B185EBCA87ull) ^
                         ((klass << 17) | (klass >> 47));
    if (!hash || hash == 1) hash += 2;
    const std::uint32_t start = static_cast<std::uint32_t>(hash) % kAggregateLimit;
    for (std::uint32_t probe = 0; probe < kAggregateLimit; ++probe) {
        PairAggregate& entry = g_aggregates[(start + probe) % kAggregateLimit];
        std::uint64_t slot_hash = entry.hash.load(std::memory_order_acquire);
        if (!slot_hash) {
            std::uint64_t empty = 0;
            if (entry.hash.compare_exchange_strong(empty, 1, std::memory_order_acq_rel)) {
                entry.function.store(event.function, std::memory_order_relaxed);
                entry.klass.store(event.klass, std::memory_order_relaxed);
                entry.count.store(1, std::memory_order_relaxed);
                entry.first_qpc.store(event.qpc, std::memory_order_relaxed);
                entry.last_qpc.store(event.qpc, std::memory_order_relaxed);
                entry.hash.store(hash, std::memory_order_release);
                g_aggregate_lock.clear(std::memory_order_release);
                return;
            }
            slot_hash = entry.hash.load(std::memory_order_acquire);
        }
        if (slot_hash == hash &&
            entry.function.load(std::memory_order_relaxed) == event.function &&
            entry.klass.load(std::memory_order_relaxed) == event.klass) {
            entry.count.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t first = entry.first_qpc.load(std::memory_order_relaxed);
            const std::uint64_t last = entry.last_qpc.load(std::memory_order_relaxed);
            if (event.qpc < first) entry.first_qpc.store(event.qpc, std::memory_order_relaxed);
            if (event.qpc > last) entry.last_qpc.store(event.qpc, std::memory_order_relaxed);
            g_aggregate_lock.clear(std::memory_order_release);
            return;
        }
    }
    g_aggregate_untracked.fetch_add(1, std::memory_order_relaxed);
    g_aggregate_lock.clear(std::memory_order_release);
}

std::uint64_t QpcNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}

bool TraceWindowExpired(std::uint64_t now) {
    const std::uint64_t start = g_trace_start_qpc.load(std::memory_order_relaxed);
    const std::uint64_t frequency = g_qpc_frequency.load(std::memory_order_relaxed);
    return frequency && now >= start &&
           now - start >= frequency * kTraceDurationMs / 1000;
}

void TriggerTrace(std::uint32_t source) {
    TraceState expected = TraceState::Armed;
    if (!g_trace_state.compare_exchange_strong(expected, TraceState::TriggerPending,
                                                std::memory_order_acq_rel)) return;
    const std::uint64_t marker = g_event_order.fetch_add(1, std::memory_order_relaxed) + 1;
    g_trigger_order.store(marker, std::memory_order_release);
    g_trigger_source.store(source, std::memory_order_relaxed);
    g_trace_start_qpc.store(QpcNow(), std::memory_order_relaxed);
    g_trace_start_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_trace_state.store(TraceState::Triggering, std::memory_order_release);
    ui::NotifyResetTraceTriggered();
    SC_LOG("tas: processevent trace trigger accepted source=%s marker_order=%llu",
           source == 1 ? "native_reset_hook" : "fallback_hotkey",
           static_cast<unsigned long long>(marker));
    SetResult(source == 1 ? "Native reset boundary triggered capture." :
                           "Fallback hotkey triggered capture; activate Reset Situation now.");
}

void __fastcall ProcessEventHook(ue::UObject* self, ue::UFunction* function,
                                void* params) {
    const TraceState initial = g_trace_state.load(std::memory_order_relaxed);
    if (!g_in_capture_hook &&
        (initial == TraceState::Armed || initial == TraceState::TriggerPending ||
         initial == TraceState::Triggering || initial == TraceState::Capturing)) {
        g_in_capture_hook = true;
        g_active_writers.fetch_add(1, std::memory_order_acq_rel);
        TraceState state = g_trace_state.load(std::memory_order_acquire);
        if (state == TraceState::Armed || state == TraceState::TriggerPending ||
            state == TraceState::Triggering || state == TraceState::Capturing) {
            RawTraceEvent event = {};
            event.order = g_event_order.fetch_add(1, std::memory_order_relaxed) + 1;
            event.qpc = QpcNow();
            event.thread_id = GetCurrentThreadId();
            event.object = reinterpret_cast<std::uintptr_t>(self);
            event.function = reinterpret_cast<std::uintptr_t>(function);
            if (self && ue::RangeReadable(
                            reinterpret_cast<const std::uint8_t*>(self) + 0x10,
                            sizeof(event.klass)))
                std::memcpy(&event.klass,
                            reinterpret_cast<const std::uint8_t*>(self) + 0x10,
                            sizeof(event.klass));

            if ((state == TraceState::Triggering || state == TraceState::Capturing) &&
                TraceWindowExpired(event.qpc)) {
                g_trace_state.compare_exchange_strong(state, TraceState::Complete,
                                                      std::memory_order_acq_rel);
                state = g_trace_state.load(std::memory_order_acquire);
            }
            if (state == TraceState::Armed || state == TraceState::TriggerPending) {
                const std::uint32_t index = static_cast<std::uint32_t>(event.order) %
                                            kPreEventLimit;
                g_pre_ring[index] = event;
                g_pre_reserved.fetch_add(1, std::memory_order_relaxed);
            } else if (state == TraceState::Triggering || state == TraceState::Capturing) {
                const std::uint32_t index = g_post_reserved.fetch_add(
                    1, std::memory_order_relaxed);
                if (index < kPostEventLimit) {
                    g_post_events[index] = event;
                } else {
                    AggregateOverflow(event);
                }
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
                 bool situation, bool spawner, bool inactive, bool pooled) {
    if (!sample || !ue::IsAliveObject(actor)) return;
    sample->object = reinterpret_cast<std::uintptr_t>(actor);
    sample->player = player;
    sample->situation = situation;
    sample->spawner = spawner;
    sample->inactive = inactive;
    sample->pooled = pooled;
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

    if (actor && !situation && !spawner && !pooled) {
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
              bool situation, bool spawner, bool inactive = false,
              bool pooled = false) {
    if (!actor || snapshot->count >= kActorLimit) return false;
    ActorSample& sample = snapshot->actors[snapshot->count];
    SampleActor(&sample, actor, player, situation, spawner, inactive, pooled);
    if (!sample.object) return false;
    if (!situation && !spawner && !pooled) {
        sample.required_components_available = sample.attack && sample.defense &&
                                                sample.health_component;
        if (sample.required_components_available)
            ++snapshot->required_components_available;
        else
            ++snapshot->required_components_missing;
    }
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
    snapshot.reset_begin_total = g_reset_begin_total.load(std::memory_order_relaxed);
    snapshot.reset_return_total = g_reset_return_total.load(std::memory_order_relaxed);

    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    AddActor(&snapshot, player, true, false, false);

    ue::UObject* fighters[kFighterEnumerationLimit] = {};
    int fighter_count = 0;
    int fighter_total = 0;
    snapshot.fighter_enumeration_available = game::EnumerateWorldFighters(
        fighters, kFighterEnumerationLimit, &fighter_count, &fighter_total);
    if (snapshot.fighter_enumeration_available) {
        snapshot.fighter_total = static_cast<std::uint32_t>((std::max)(fighter_total, 0));
        snapshot.fighter_enumerated = static_cast<std::uint32_t>((std::max)(fighter_count, 0));
        ue::UObject* active[kActiveFighterLimit] = {};
        ue::UObject* inactive[kFighterEnumerationLimit] = {};
        ue::UObject* pooled[kFighterEnumerationLimit] = {};
        std::uint32_t active_count = 0;
        std::uint32_t active_total = 0;
        std::uint32_t inactive_count = 0;
        std::uint32_t pooled_count = 0;
        for (int i = 0; i < fighter_count; ++i) {
            ue::UObject* actor = fighters[i];
            if (!actor || actor == player) continue;
            if (!ue::IsAliveObject(actor)) {
                ++snapshot.invalid_fighters;
                continue;
            }
            ue::FVector location = {};
            if (!ue::GetActorLocation(actor, &location)) {
                ++snapshot.invalid_fighters;
                continue;
            }
            if (game::IsPooled(location)) {
                if (pooled_count < kFighterEnumerationLimit)
                    pooled[pooled_count++] = actor;
                continue;
            }
            const game::Fighter fighter = game::ResolveFighter(actor);
            if (!fighter.health || !ue::IsAliveObject(fighter.health)) {
                ++snapshot.missing_health_fighters;
                if (inactive_count < kFighterEnumerationLimit)
                    inactive[inactive_count++] = actor;
                continue;
            }
            const float health = game::GetHealth(fighter);
            if (!std::isfinite(health) || health <= 0.5f) {
                if (inactive_count < kFighterEnumerationLimit)
                    inactive[inactive_count++] = actor;
                continue;
            }
            ++active_total;
            if (active_count < kActiveFighterLimit) active[active_count++] = actor;
        }
        snapshot.active_fighter_count = active_count;
        snapshot.inactive_fighter_count = inactive_count;
        snapshot.pooled_fighter_count = pooled_count;
        snapshot.active_fighter_truncated = active_total - active_count;
        snapshot.inactive_fighter_truncated = inactive_count > kInactiveFighterLimit
                                                  ? inactive_count - kInactiveFighterLimit
                                                  : 0;
        snapshot.pooled_fighter_truncated = pooled_count > kPooledFighterLimit
                                                ? pooled_count - kPooledFighterLimit
                                                : 0;
        for (std::uint32_t i = 0; i < active_count; ++i)
            AddActor(&snapshot, active[i], false, false, false);
        const std::uint32_t inactive_sampled =
            (std::min)(inactive_count, static_cast<std::uint32_t>(kInactiveFighterLimit));
        for (std::uint32_t i = 0; i < inactive_sampled; ++i)
            AddActor(&snapshot, inactive[i], false, false, false, true, false);
        const std::uint32_t pooled_sampled =
            (std::min)(pooled_count, static_cast<std::uint32_t>(kPooledFighterLimit));
        for (std::uint32_t i = 0; i < pooled_sampled; ++i)
            AddActor(&snapshot, pooled[i], false, false, false, false, true);
    }

    ue::UObject* situations[16] = {};
    int count = 0;
    int total = 0;
    snapshot.situation_enumeration_available = game::EnumerateActorsOfClass(
        StaticClass(offsets::AAISituationActor_StaticClass), situations, 16,
        &count, &total);
    if (snapshot.situation_enumeration_available) {
        snapshot.situation_total = static_cast<std::uint32_t>((std::max)(total, 0));
        snapshot.situation_truncated = static_cast<std::uint32_t>(
            (std::max)(total - count, 0));
        for (int i = 0; i < count; ++i) AddActor(&snapshot, situations[i], false, true, false);
    }
    ue::UObject* spawners[16] = {};
    snapshot.spawner_enumeration_available = game::EnumerateActorsOfClass(
        StaticClass(offsets::AAISpawner_StaticClass), spawners, 16, &count, &total);
    if (snapshot.spawner_enumeration_available) {
        snapshot.spawner_total = static_cast<std::uint32_t>((std::max)(total, 0));
        snapshot.spawner_truncated = static_cast<std::uint32_t>(
            (std::max)(total - count, 0));
        for (int i = 0; i < count; ++i) AddActor(&snapshot, spawners[i], false, false, true);
    }
    snapshot.valid = true;
    return snapshot;
}

void LogSample(const char* stage, const ActorSample& a) {
    const char* role = a.player ? "player" : a.situation ? "situation" :
                       a.spawner ? "spawner" : a.pooled ? "pooled" :
                       a.inactive ? "inactive" : "active";
    SC_LOG("tas: reset-diff stage=%s role=%s name=%s class=%s object=%p "
           "attack=%p defense=%p health_component=%p ai=%p movement=%p order=%p "
           "loc=%s%.2f,%.2f,%.2f rot=%.2f,%.2f,%.2f vel=%s%.2f,%.2f,%.2f "
           "health=%s%.2f guard=%s%.2f custom_dilation=%s%.3f montage=%s "
           "attack_danger=%s%.2f/%u ai_phase=%s%d/%d max_walk=%s%.2f "
           "cinematic_weight=%s%.3f situation_last_man=%s%d "
           "spawner_timing=%s%.2f/%.2f spawner_kill_values=%d/%d",
           stage, role, a.identity, a.class_path,
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
    g_reset_observed_since_baseline = false;
    g_reset_returned_since_baseline = false;
    SC_LOG("tas: reset-diff baseline world=%s world_object=%p match_state=%s%u:%d "
           "actors=%u fighters_total=%u enumerated=%u active=%u active_truncated=%u "
           "inactive=%u inactive_truncated=%u pooled=%u pooled_truncated=%u "
           "components_available=%u components_missing=%u fighter_enumeration_available=%u enumeration_omitted=%u "
           "invalid=%u missing_health=%u "
           "situations=%u situations_truncated=%u situation_enumeration_available=%u "
           "spawners=%u spawners_truncated=%u spawner_enumeration_available=%u",
           g_baseline.world_path, reinterpret_cast<void*>(g_baseline.world),
           g_baseline.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(g_baseline.match_state.comparison_index),
           g_baseline.match_state.number,
           g_baseline.count, g_baseline.fighter_total, g_baseline.fighter_enumerated,
           g_baseline.active_fighter_count, g_baseline.active_fighter_truncated,
           g_baseline.inactive_fighter_count, g_baseline.inactive_fighter_truncated,
           g_baseline.pooled_fighter_count, g_baseline.pooled_fighter_truncated,
           g_baseline.required_components_available,
           g_baseline.required_components_missing,
           g_baseline.fighter_enumeration_available ? 1u : 0u,
           g_baseline.fighter_total > g_baseline.fighter_enumerated
               ? g_baseline.fighter_total - g_baseline.fighter_enumerated : 0u,
           g_baseline.invalid_fighters, g_baseline.missing_health_fighters,
           g_baseline.situation_total, g_baseline.situation_truncated,
           g_baseline.situation_enumeration_available ? 1u : 0u,
           g_baseline.spawner_total, g_baseline.spawner_truncated,
           g_baseline.spawner_enumeration_available ? 1u : 0u);
    for (std::uint32_t i = 0; i < g_baseline.count; ++i)
        LogSample("before", g_baseline.actors[i]);
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.baseline_saved = true;
    g_status.reset_begin_observed = false;
    g_status.reset_return_observed = false;
    g_status.actor_registry_stable = false;
    g_status.components_ready = false;
    g_status.reset_observed_since_baseline = false;
    g_status.reset_returned_since_baseline = false;
    g_status.fighter_enumeration_available = false;
    g_status.active_fighters = 0;
    g_status.inactive_fighters = 0;
    g_status.pooled_fighters = 0;
    g_status.components_available_count = 0;
    g_status.components_missing = 0;
    g_status.fighter_enumeration_total = 0;
    g_status.fighter_enumeration_captured = 0;
    g_status.fighter_enumeration_omitted = 0;
    g_status.sampled_actors_omitted = 0;
    ReleaseSRWLockExclusive(&g_status_lock);
    SetResult("Stage 1/3: baseline captured. Arm the reset trace next.");
}

void CompareActors(const ActorSnapshot& current) {
    if (!g_baseline.valid) {
        SetResult("Comparison refused: capture a baseline first.");
        return;
    }
    g_reset_observed_since_baseline = current.reset_begin_total >
                                      g_baseline.reset_begin_total;
    g_reset_returned_since_baseline = current.reset_return_total >
                                      g_baseline.reset_return_total;
    const std::uint32_t reset_kind = g_reset_last_kind.load(std::memory_order_relaxed);
    const std::uintptr_t reset_caller = g_reset_last_caller.load(std::memory_order_relaxed);
    SC_LOG("tas: reset-diff compare before_world=%s before_world_object=%p "
           "before_match_state=%s%u:%d after_world=%s after_world_object=%p "
           "after_match_state=%s%u:%d same_world=%u before=%u after=%u "
           "registry_stable=1 components_available=%u components_missing=%u fighter_enumeration_available=%u "
           "reset_begin_since_baseline=%u reset_return_since_baseline=%u "
           "last_reset_kind=%u last_reset_caller_rva=0x%llX "
           "fighters_total=%u enumerated=%u active=%u active_truncated=%u "
           "inactive=%u inactive_truncated=%u pooled=%u pooled_truncated=%u "
           "enumeration_omitted=%u invalid=%u missing_health=%u "
           "situations=%u situations_truncated=%u situation_enumeration_available=%u "
           "spawners=%u spawners_truncated=%u spawner_enumeration_available=%u",
           g_baseline.world_path, reinterpret_cast<void*>(g_baseline.world),
           g_baseline.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(g_baseline.match_state.comparison_index),
           g_baseline.match_state.number,
           current.world_path, reinterpret_cast<void*>(current.world),
           current.match_state_available ? "" : "n/a ",
           static_cast<unsigned>(current.match_state.comparison_index),
           current.match_state.number,
           current.world == g_baseline.world ? 1u : 0u, g_baseline.count, current.count,
           current.required_components_available, current.required_components_missing,
           current.fighter_enumeration_available ? 1u : 0u,
           g_reset_observed_since_baseline ? 1u : 0u,
           g_reset_returned_since_baseline ? 1u : 0u, reset_kind,
           static_cast<unsigned long long>(reset_caller), current.fighter_total,
           current.fighter_enumerated, current.active_fighter_count,
           current.active_fighter_truncated, current.inactive_fighter_count,
           current.inactive_fighter_truncated, current.pooled_fighter_count,
           current.pooled_fighter_truncated,
           current.fighter_total > current.fighter_enumerated
               ? current.fighter_total - current.fighter_enumerated : 0u,
           current.invalid_fighters, current.missing_health_fighters,
           current.situation_total, current.situation_truncated,
           current.situation_enumeration_available ? 1u : 0u,
           current.spawner_total, current.spawner_truncated,
           current.spawner_enumeration_available ? 1u : 0u);
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
    SetResult("Stage 3/3: stable observed inventory captured. Reset completion and pending callbacks remain unverified; see SifuCoop.log.");
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
        if (!second || first.object != second->object)
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
        SetResult("Comparison timed out waiting for an unchanged observed actor registry.");
        return;
    }
    ActorSnapshot current = CaptureActors();
    if (!current.valid) {
        g_stable_samples = 0;
        g_previous_sample = {};
        SetResult("Stage 3/3: waiting for current world and actors to become valid.");
        return;
    }
    g_stable_samples = SameActorRegistry(g_previous_sample, current)
                           ? g_stable_samples + 1
                           : 1;
    g_previous_sample = current;
    if (g_stable_samples < kStableSamplesRequired) {
        char message[160] = {};
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "Stage 3/3: observed actor registry unchanged for %u / %u samples; component readiness is tracked separately.",
                    g_stable_samples, kStableSamplesRequired);
        SetResult(message);
        return;
    }
    g_comparison_pending = false;
    g_comparison_sample = current;
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.comparison_waiting = false;
    g_status.actor_registry_stable = true;
    g_status.components_ready = current.fighter_enumeration_available &&
                                current.required_components_missing == 0;
    g_status.fighter_enumeration_available = current.fighter_enumeration_available;
    g_status.reset_observed_since_baseline = g_reset_observed_since_baseline;
    g_status.reset_returned_since_baseline = g_reset_returned_since_baseline;
    g_status.active_fighters = current.active_fighter_count;
    g_status.inactive_fighters = current.inactive_fighter_count;
    g_status.pooled_fighters = current.pooled_fighter_count;
    g_status.components_available_count = current.required_components_available;
    g_status.components_missing = current.required_components_missing;
    g_status.fighter_enumeration_total = current.fighter_total;
    g_status.fighter_enumeration_captured = current.fighter_enumerated;
    g_status.sampled_actors_omitted = current.active_fighter_truncated +
        current.inactive_fighter_truncated + current.pooled_fighter_truncated +
        current.situation_truncated + current.spawner_truncated;
    g_status.fighter_enumeration_omitted = current.fighter_total > current.fighter_enumerated
        ? current.fighter_total - current.fighter_enumerated : 0u;
    ReleaseSRWLockExclusive(&g_status_lock);
    CompareActors(g_comparison_sample);
}

void ResolveTraceNames() {
    const std::uint32_t count = g_trace_count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count; ++i) {
        TraceEvent& event = g_trace[i];
        for (std::uint32_t j = 0; j < i; ++j) {
            if (g_trace[j].raw.function == event.raw.function &&
                g_trace[j].function_path[0]) {
                lstrcpynA(event.function_path, g_trace[j].function_path,
                          sizeof(event.function_path));
                break;
            }
        }
        if (!event.function_path[0]) {
            auto* function = reinterpret_cast<ue::UObject*>(event.raw.function);
            if (ue::IsAliveObject(function))
                ue::GetObjectPathName(function, event.function_path,
                                      sizeof(event.function_path));
            if (!event.function_path[0]) lstrcpynA(event.function_path, "unavailable",
                                                   sizeof(event.function_path));
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (g_trace[j].raw.klass == event.raw.klass &&
                g_trace[j].class_path[0]) {
                lstrcpynA(event.class_path, g_trace[j].class_path, sizeof(event.class_path));
                break;
            }
        }
        if (!event.class_path[0]) {
            auto* klass = reinterpret_cast<ue::UObject*>(event.raw.klass);
            if (ue::IsAliveObject(klass))
                ue::GetObjectPathName(klass, event.class_path, sizeof(event.class_path));
            if (!event.class_path[0]) lstrcpynA(event.class_path, "unavailable",
                                                sizeof(event.class_path));
        }
    }
    for (std::uint32_t i = 0; i < kAggregateLimit; ++i) {
        PairAggregate& entry = g_aggregates[i];
        const std::uint64_t hash = entry.hash.load(std::memory_order_acquire);
        if (!hash || hash == 1) continue;
        bool duplicate = false;
        for (std::uint32_t j = 0; j < i; ++j) {
            if (g_aggregates[j].hash.load(std::memory_order_acquire) > 1 &&
                g_aggregates[j].function.load(std::memory_order_relaxed) ==
                    entry.function.load(std::memory_order_relaxed) &&
                g_aggregates[j].klass.load(std::memory_order_relaxed) ==
                    entry.klass.load(std::memory_order_relaxed)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        auto* function = reinterpret_cast<ue::UObject*>(
            entry.function.load(std::memory_order_relaxed));
        auto* klass = reinterpret_cast<ue::UObject*>(
            entry.klass.load(std::memory_order_relaxed));
        if (ue::IsAliveObject(function))
            ue::GetObjectPathName(function, entry.function_path,
                                  sizeof(entry.function_path));
        if (ue::IsAliveObject(klass))
            ue::GetObjectPathName(klass, entry.class_path, sizeof(entry.class_path));
        if (!entry.function_path[0])
            lstrcpynA(entry.function_path, "unavailable", sizeof(entry.function_path));
        if (!entry.class_path[0])
            lstrcpynA(entry.class_path, "unavailable", sizeof(entry.class_path));
    }
}

std::uint32_t AggregatePairCount();

void PrepareTraceOutput() {
    std::uint32_t candidate_count = 0;
    for (const RawTraceEvent& event : g_pre_ring)
        if (event.order) g_candidates[candidate_count++] = event;
    const std::uint32_t post_count = (std::min)(
        g_post_reserved.load(std::memory_order_acquire), kPostEventLimit);
    for (std::uint32_t i = 0; i < post_count; ++i)
        if (g_post_events[i].order) g_candidates[candidate_count++] = g_post_events[i];
    std::sort(g_candidates, g_candidates + candidate_count,
              [](const RawTraceEvent& a, const RawTraceEvent& b) {
                  return a.order < b.order;
              });

    const std::uint64_t marker = g_trigger_order.load(std::memory_order_relaxed);
    std::uint32_t before_count = 0;
    for (std::uint32_t i = 0; i < candidate_count; ++i)
        if (g_candidates[i].order < marker) ++before_count;
    const std::uint32_t before_skip = before_count > kPreEventLimit
                                          ? before_count - kPreEventLimit
                                          : 0;
    const std::uint32_t pre_total = g_pre_reserved.load(std::memory_order_relaxed);
    g_trace_pre_overwritten.store(pre_total > kPreEventLimit
                                      ? pre_total - kPreEventLimit : 0u,
                                  std::memory_order_relaxed);
    std::uint32_t written = 0;
    std::uint32_t before_seen = 0;
    for (std::uint32_t i = 0; i < candidate_count; ++i) {
        const RawTraceEvent& event = g_candidates[i];
        if (event.order >= marker) continue;
        if (before_seen++ < before_skip) continue;
        g_trace[written++].raw = event;
    }
    std::uint32_t after_written = 0;
    for (std::uint32_t i = 0; i < candidate_count && after_written < kPostEventLimit; ++i) {
        const RawTraceEvent& event = g_candidates[i];
        if (event.order < marker) continue;
        g_trace[written++].raw = event;
        ++after_written;
    }
    if (candidate_count > written)
        g_trace_overflow_events.fetch_add(candidate_count - written,
                                          std::memory_order_relaxed);
    g_trace_count.store(written, std::memory_order_release);
}

DWORD WINAPI WriteTrace(LPVOID) {
    const std::uint32_t count = g_trace_count.load(std::memory_order_acquire);
    const std::uint64_t start = g_trace_start_qpc.load(std::memory_order_relaxed);
    const std::uint64_t frequency = g_qpc_frequency.load(std::memory_order_relaxed);
    const char* source = g_trigger_source.load(std::memory_order_relaxed) == 1
                             ? "native_reset_hook"
                             : "fallback_hotkey";
    const std::uint32_t pre_total = g_pre_reserved.load(std::memory_order_relaxed);
    const std::uint32_t pre_overwritten =
        g_trace_pre_overwritten.load(std::memory_order_relaxed);
    const std::uint32_t post_total = g_post_reserved.load(std::memory_order_relaxed);
    const std::uint32_t aggregated_events = post_total > kPostEventLimit
                                                ? post_total - kPostEventLimit
                                                : 0;
    SC_LOG("tas: processevent trace complete trigger=%s marker_order=%llu detail=%u "
           "pre_ring_total=%u pre_overwritten=%u post_detail=%u aggregated_events=%u "
           "aggregate_pairs=%u overflow_events=%u aggregate_untracked=%u duration_ms=%llu "
           "reset_begin=%u reset_return=%u",
           source, static_cast<unsigned long long>(g_trigger_order.load()), count,
           pre_total, pre_overwritten, (std::min)(post_total, kPostEventLimit),
           aggregated_events, AggregatePairCount(),
           g_trace_overflow_events.load(), g_aggregate_untracked.load(),
           static_cast<unsigned long long>(GetTickCount64() -
               g_trace_start_ms.load(std::memory_order_relaxed)),
           g_reset_begin_total.load(), g_reset_return_total.load());
    for (std::uint32_t i = 0; i < count; ++i) {
        const TraceEvent& e = g_trace[i];
        const RawTraceEvent& r = e.raw;
        const std::uint64_t us = frequency && r.qpc >= start
                                     ? (r.qpc - start) * 1000000ull / frequency
                                     : 0;
        SC_LOG("tas: processevent order=%llu delta_us=%llu tid=%lu owner=%p class=%s "
               "function=%s function_ptr=%p",
               static_cast<unsigned long long>(r.order),
               static_cast<unsigned long long>(us), r.thread_id,
               reinterpret_cast<void*>(r.object), e.class_path, e.function_path,
               reinterpret_cast<void*>(r.function));
    }
    for (const PairAggregate& entry : g_aggregates) {
        if (entry.hash.load(std::memory_order_acquire) <= 1) continue;
        const std::uint64_t first = entry.first_qpc.load(std::memory_order_relaxed);
        const std::uint64_t last = entry.last_qpc.load(std::memory_order_relaxed);
        SC_LOG("tas: processevent aggregate class=%s function=%s function_ptr=%p count=%llu "
               "first_delta_us=%llu last_delta_us=%llu",
               entry.class_path, entry.function_path,
               reinterpret_cast<void*>(entry.function.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(entry.count.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(frequency && first >= start
                    ? (first - start) * 1000000ull / frequency : 0),
               static_cast<unsigned long long>(frequency && last >= start
                    ? (last - start) * 1000000ull / frequency : 0));
    }
    return 0;
}

void FinishTrace() {
    if (g_active_writers.load(std::memory_order_acquire) != 0) return;
    bool expected = false;
    if (!g_trace_resolving.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) return;
    PrepareTraceOutput();
    ResolveTraceNames();
    WaitForWriter();
    g_writer_thread = CreateThread(nullptr, 0, WriteTrace, nullptr, 0, nullptr);
    AcquireSRWLockExclusive(&g_status_lock);
    g_status.trace_events = g_trace_count.load(std::memory_order_relaxed);
    g_status.trace_dropped = g_trace_overflow_events.load(std::memory_order_relaxed) +
                             g_aggregate_untracked.load(std::memory_order_relaxed);
    g_status.writer_active = g_writer_thread != nullptr;
    ReleaseSRWLockExclusive(&g_status_lock);
    if (!g_writer_thread) WriteTrace(nullptr);
}

void ArmTrace() {
    WaitForWriter();
    for (RawTraceEvent& event : g_pre_ring) event = {};
    for (RawTraceEvent& event : g_post_events) event = {};
    for (RawTraceEvent& event : g_candidates) event = {};
    for (TraceEvent& event : g_trace) event = {};
    for (PairAggregate& entry : g_aggregates) {
        entry.hash.store(0, std::memory_order_relaxed);
        entry.function.store(0, std::memory_order_relaxed);
        entry.klass.store(0, std::memory_order_relaxed);
        entry.count.store(0, std::memory_order_relaxed);
        entry.first_qpc.store(0, std::memory_order_relaxed);
        entry.last_qpc.store(0, std::memory_order_relaxed);
        entry.class_path[0] = 0;
        entry.function_path[0] = 0;
    }
    g_event_order.store(0, std::memory_order_relaxed);
    g_pre_reserved.store(0, std::memory_order_relaxed);
    g_post_reserved.store(0, std::memory_order_relaxed);
    g_trace_count.store(0, std::memory_order_relaxed);
    g_trace_overflow_events.store(0, std::memory_order_relaxed);
    g_aggregate_untracked.store(0, std::memory_order_relaxed);
    g_trace_pre_overwritten.store(0, std::memory_order_relaxed);
    g_trigger_order.store(0, std::memory_order_relaxed);
    g_trigger_source.store(0, std::memory_order_relaxed);
    g_trace_resolving.store(false, std::memory_order_relaxed);
    g_trace_state.store(TraceState::Armed, std::memory_order_release);
    SetResult("Stage 2/3: armed. Close GUI, press the configured marker, then Reset Situation; native reset hooks are also monitored.");
}

std::uint32_t AggregatePairCount() {
    std::uint32_t count = 0;
    for (const PairAggregate& entry : g_aggregates)
        if (entry.hash.load(std::memory_order_acquire) > 1) ++count;
    return count;
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

void QueueArmResetTrace() {
    TraceState state = g_trace_state.load(std::memory_order_acquire);
    if (state == TraceState::Armed || state == TraceState::TriggerPending ||
        state == TraceState::Triggering || state == TraceState::Capturing ||
        state == TraceState::Queued)
        return;
    AcquireSRWLockShared(&g_status_lock);
    const bool writer_active = g_status.writer_active;
    ReleaseSRWLockShared(&g_status_lock);
    if (writer_active) return;
    g_trace_state.store(TraceState::Queued, std::memory_order_release);
    g_arm_requested.store(true, std::memory_order_release);
    SetResult("Arming reset trace on the next game-thread tick.");
}

void TriggerFallbackHotkey() { TriggerTrace(2); }

void NotifyResetBoundary(ResetBoundary boundary, bool begin,
                         std::uintptr_t caller_rva) {
    const std::uint32_t kind = static_cast<std::uint32_t>(boundary) + 1;
    g_reset_last_kind.store(kind, std::memory_order_relaxed);
    g_reset_last_caller.store(caller_rva, std::memory_order_relaxed);
    if (begin) {
        g_reset_begin_total.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_status_lock);
        g_status.reset_begin_observed = true;
        ReleaseSRWLockExclusive(&g_status_lock);
        TriggerTrace(1);
    } else {
        g_reset_return_total.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_status_lock);
        g_status.reset_return_observed = true;
        ReleaseSRWLockExclusive(&g_status_lock);
    }
}

void QueueBaselineCapture() { g_baseline_requested.store(true, std::memory_order_release); }
void QueueStateComparison() { g_compare_requested.store(true, std::memory_order_release); }

Status GetStatus() {
    Status status = {};
    status.trace_state = g_trace_state.load(std::memory_order_acquire);
    status.trace_events = g_trace_count.load(std::memory_order_relaxed);
    if (status.trace_state == TraceState::Armed ||
        status.trace_state == TraceState::TriggerPending ||
        status.trace_state == TraceState::Triggering ||
        status.trace_state == TraceState::Capturing) {
        status.trace_events = (std::min)(g_post_reserved.load(std::memory_order_relaxed),
                                         kPostEventLimit);
        status.trace_pre_events = (std::min)(
            g_pre_reserved.load(std::memory_order_relaxed), kPreEventLimit);
    }
    status.trace_pre_overwritten = g_trace_pre_overwritten.load(std::memory_order_relaxed);
    const std::uint32_t post_total = g_post_reserved.load(std::memory_order_relaxed);
    status.trace_aggregated_events = post_total > kPostEventLimit
                                         ? post_total - kPostEventLimit
                                         : 0;
    status.trace_aggregate_pairs = AggregatePairCount();
    status.trace_dropped = g_trace_overflow_events.load(std::memory_order_relaxed) +
                           g_aggregate_untracked.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_status_lock);
    status.writer_active = g_status.writer_active;
    status.baseline_saved = g_status.baseline_saved;
    status.comparison_waiting = g_status.comparison_waiting;
    status.reset_begin_observed = g_status.reset_begin_observed;
    status.reset_return_observed = g_status.reset_return_observed;
    status.actor_registry_stable = g_status.actor_registry_stable;
    status.components_ready = g_status.components_ready;
    status.reset_observed_since_baseline = g_status.reset_observed_since_baseline;
    status.reset_returned_since_baseline = g_status.reset_returned_since_baseline;
    status.fighter_enumeration_available = g_status.fighter_enumeration_available;
    status.active_fighters = g_status.active_fighters;
    status.inactive_fighters = g_status.inactive_fighters;
    status.pooled_fighters = g_status.pooled_fighters;
    status.components_available_count = g_status.components_available_count;
    status.components_missing = g_status.components_missing;
    status.fighter_enumeration_total = g_status.fighter_enumeration_total;
    status.fighter_enumeration_captured = g_status.fighter_enumeration_captured;
    status.fighter_enumeration_omitted = g_status.fighter_enumeration_omitted;
    status.sampled_actors_omitted = g_status.sampled_actors_omitted;
    lstrcpynA(status.last_result, g_status.last_result, sizeof(status.last_result));
    ReleaseSRWLockShared(&g_status_lock);
    return status;
}

void Tick() {
    PollWriter();
    if (g_arm_requested.exchange(false, std::memory_order_acq_rel)) {
        if (!g_process_event) {
            g_trace_state.store(TraceState::Failed, std::memory_order_release);
            SetResult("ProcessEvent hook unavailable; reset trace was not armed.");
        } else {
            ArmTrace();
        }
    }
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
            g_status.actor_registry_stable = false;
            g_status.components_ready = false;
            ReleaseSRWLockExclusive(&g_status_lock);
            SetResult("Stage 3/3: waiting for an unchanged observed actor registry; component readiness and reset-hook observations are reported separately.");
        }
    }
    PollComparison();
    TraceState trace_state = g_trace_state.load(std::memory_order_acquire);
    if (trace_state == TraceState::Triggering &&
        g_active_writers.load(std::memory_order_acquire) == 0) {
        g_trace_state.store(TraceState::Capturing, std::memory_order_release);
        SetResult("Reset trigger observed; capturing the bounded post-trigger interval.");
        trace_state = TraceState::Capturing;
    }
    if (trace_state == TraceState::Capturing &&
        GetTickCount64() - g_trace_start_ms.load(std::memory_order_relaxed) >=
            kTraceDurationMs) {
        g_trace_state.store(TraceState::Complete, std::memory_order_release);
        FinishTrace();
    } else if (trace_state == TraceState::Complete &&
               !g_trace_resolving.load(std::memory_order_acquire)) {
        FinishTrace();
    }
}

}
