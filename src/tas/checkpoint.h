#pragma once

#include <cstdint>

#include "state_reference.h"

namespace sifucoop::tas::checkpoint {

struct Status {
    bool exists = false;
    CheckpointReference reference = {};
    float player_health = 0.f;
    float player_max_health = 0.f;
    float player_guard = 0.f;
    float enemy_health = 0.f;
    float enemy_max_health = 0.f;
    float enemy_guard = 0.f;
    char last_result[224] = "No checkpoint saved.";
};

void QueueSave();
void QueueLoad();
void ProcessRequests();
Status GetStatus();

}
