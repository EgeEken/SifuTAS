#pragma once

#include <cstdint>

#include "state_reference.h"

namespace sifucoop::tas::checkpoint {

struct Status {
    bool exists = false;
    bool incomplete = false;
    CheckpointReference reference = {};
    std::uint32_t enumerated_count = 0;
    std::uint32_t captured_count = 0;
    std::uint32_t restored_count = 0;
    std::uint32_t skipped_count = 0;
    std::uint32_t partial_count = 0;
    char last_result[256] = "No checkpoint saved.";
};

void QueueSave();
void QueueLoad();
void ProcessRequests();
Status GetStatus();

}
