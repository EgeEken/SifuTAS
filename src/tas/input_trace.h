#pragma once

#include <cstdint>

namespace sifucoop::tas::input_trace {

enum class State : std::uint8_t { Ready, Queued, Capturing, Complete, Failed };

struct Status {
    State state = State::Ready;
    std::uint32_t event_count = 0;
    std::uint32_t dropped_count = 0;
    bool player_input_available = false;
    bool writer_active = false;
};

void InstallHooks(std::uintptr_t module_base);
void RemoveHooks();
void QueueCapture();
Status GetStatus();

void EngineTickBegin(float delta_seconds, bool idle_mode);
void EngineTickEnd();
void WorldTickBegin(void* world, int tick_type, float delta_seconds, bool will_execute);
void WorldTickEnd(void* world, int tick_type, float delta_seconds, bool executed);
void RecordControllerKey(void* controller, std::uint32_t key_index,
                         std::uint32_t key_number, std::uint32_t input_event,
                         float amount, bool gamepad);

}
