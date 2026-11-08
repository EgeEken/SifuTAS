#pragma once

#include <cstdint>

namespace sifucoop::game {

enum class ReplayRecordingState : std::uint8_t {
    Ready,
    Requested,
    Starting,
    Active,
    Finalizing,
    Finalized,
    SystemUnavailable,
    UnsupportedMode,
    DisabledByGame,
    Failed,
};

struct ReplayRecordingStatus {
    ReplayRecordingState state = ReplayRecordingState::Ready;
    char message[192] = "Native Sifu replay is ready.";
    float elapsed_seconds = 0.f;
    bool replay_system_available = false;
    bool demo_driver_available = false;
};

void QueueReplayRecordingToggle();
void TickReplayRecording();
ReplayRecordingStatus GetReplayRecordingStatus();

}
