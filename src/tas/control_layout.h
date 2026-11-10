#pragma once

namespace sifucoop::tas::layout {

inline constexpr int kSlotCount = 8;
inline constexpr int kPresetCount = 5;
inline constexpr int kConfigVersion = 2;

constexpr int DefaultKey(int slot) {
    switch (slot) {
        case 0: return '1';
        case 1: return '2';
        case 2: return '4';
        case 3: return '5';
        case 4: return '6';
        case 5: return '7';
        default: return 0;
    }
}

constexpr int PresetForSlot(int slot) {
    if (slot >= 1 && slot <= 3) return slot - 1;
    if (slot == 6 || slot == 7) return slot - 3;
    return -1;
}

constexpr int StepAfterPresetActivation(int current_step, int preset_step) {
    return current_step == preset_step ? 10 : preset_step;
}

}
