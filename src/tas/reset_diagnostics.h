#pragma once

#include <cstdint>

namespace sifucoop::tas::reset_diagnostics {

enum class TraceState : std::uint8_t {
    Ready, Queued, Armed, TriggerPending, Triggering, Capturing, Complete, Failed
};

enum class ResetBoundary : std::uint8_t {
    ActorReset,
    ActorK2Reset,
    GameModeReset,
    GameModeResetLevel,
    RestartCurrentMap,
};

struct Status {
    TraceState trace_state = TraceState::Ready;
    std::uint32_t trace_events = 0;
    std::uint32_t trace_pre_events = 0;
    std::uint32_t trace_dropped = 0;
    std::uint32_t trace_pre_overwritten = 0;
    std::uint32_t trace_aggregated_events = 0;
    std::uint32_t trace_aggregate_pairs = 0;
    bool writer_active = false;
    bool baseline_saved = false;
    bool comparison_waiting = false;
    bool reset_begin_observed = false;
    bool reset_return_observed = false;
    bool actor_registry_stable = false;
    bool components_ready = false;
    bool reset_observed_since_baseline = false;
    bool reset_returned_since_baseline = false;
    bool fighter_enumeration_available = false;
    std::uint32_t active_fighters = 0;
    std::uint32_t inactive_fighters = 0;
    std::uint32_t pooled_fighters = 0;
    std::uint32_t components_available_count = 0;
    std::uint32_t components_missing = 0;
    std::uint32_t fighter_enumeration_total = 0;
    std::uint32_t fighter_enumeration_captured = 0;
    std::uint32_t fighter_enumeration_omitted = 0;
    std::uint32_t sampled_actors_omitted = 0;
    char last_result[160] = "No encounter baseline captured.";
};

void InstallHooks(std::uintptr_t module_base);
void RemoveHooks();
void QueueArmResetTrace();
void TriggerFallbackHotkey();
void NotifyResetBoundary(ResetBoundary boundary, bool begin,
                         std::uintptr_t caller_rva);
void QueueBaselineCapture();
void QueueStateComparison();
Status GetStatus();
void Tick();

}
