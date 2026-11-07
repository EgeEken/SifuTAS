#pragma once

#include <cstdint>

#include "state_reference.h"

namespace sifucoop::tas::checkpoint {

struct ActorDiagnostic {
    char identity[512] = {};
    std::uint32_t captured_fields = 0;
    std::uint32_t restored_fields = 0;
    std::uint32_t failed_fields = 0;
    std::uint32_t unsupported_fields = 0;
    std::uint32_t unavailable_fields = 0;
    bool player = false;
    bool skipped = false;
};

struct Status {
    bool exists = false;
    bool incomplete = false;
    bool camera_available = false;
    bool camera_restored = false;
    CheckpointReference reference = {};
    std::uint32_t enumerated_count = 0;
    std::uint32_t world_actor_count = 0;
    std::uint32_t captured_count = 0;
    std::uint32_t restored_count = 0;
    std::uint32_t skipped_count = 0;
    std::uint32_t partial_count = 0;
    std::uint32_t cloth_reset_count = 0;
    std::uint32_t pooled_count = 0;
    std::uint32_t invalid_count = 0;
    std::uint32_t missing_health_count = 0;
    std::uint32_t inactive_count = 0;
    std::uint32_t identity_truncated_count = 0;
    std::uint32_t identity_unavailable_count = 0;
    std::uint32_t capacity_skipped_count = 0;
    std::uint32_t actor_count = 0;
    ActorDiagnostic actors[32] = {};
    char last_result[256] = "No checkpoint saved.";
};

void QueueSave();
void QueueLoad();
void ProcessRequests();
Status GetStatus();
void TickCameraDiagnostics();

}
