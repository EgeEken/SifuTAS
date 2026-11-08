#pragma once

#include <cstdint>

namespace sifucoop::tas::reset_diagnostics {

enum class TraceState : std::uint8_t { Ready, Queued, Capturing, Complete, Failed };

struct Status {
    TraceState trace_state = TraceState::Ready;
    std::uint32_t trace_events = 0;
    std::uint32_t trace_dropped = 0;
    bool writer_active = false;
    bool baseline_saved = false;
    bool comparison_waiting = false;
    char last_result[160] = "No encounter baseline captured.";
};

void InstallHooks(std::uintptr_t module_base);
void RemoveHooks();
void QueueProcessEventCapture();
void QueueBaselineCapture();
void QueueStateComparison();
Status GetStatus();
void Tick();

}
