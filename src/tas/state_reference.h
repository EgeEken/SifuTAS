#pragma once

#include <cstdint>

namespace sifucoop::tas {

struct CheckpointReference {
    char level_path[192] = {};
    char player_identity[128] = {};
    char enemy_identity[128] = {};
    std::uint64_t engine_tick = 0;
    std::uint64_t simulation_frame = 0;
    bool simulation_frame_verified = false;
    char recording_branch_id[40] = {};
};

}
