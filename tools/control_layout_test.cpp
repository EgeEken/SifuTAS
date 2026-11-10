#include <cassert>

#include "../src/tas/control_layout.h"
#include "../src/tas/controls.h"

int main() {
    using namespace sifucoop::tas;
    assert(layout::kConfigVersion == 2);
    assert(layout::kSlotCount == 8);
    assert(layout::kPresetCount == 5);
    assert(layout::DefaultKey(0) == '1');
    assert(layout::DefaultKey(1) == '2');
    assert(layout::DefaultKey(2) == '4');
    assert(layout::DefaultKey(3) == '5');
    assert(layout::DefaultKey(4) == '6');
    assert(layout::DefaultKey(5) == '7');
    assert(layout::DefaultKey(6) == 0 && layout::DefaultKey(7) == 0);
    const Settings defaults{};
    for (int slot = 0; slot < layout::kSlotCount; ++slot)
        assert(defaults.bindings[slot].key == layout::DefaultKey(slot));
    assert(layout::PresetForSlot(0) == -1);
    assert(layout::PresetForSlot(1) == 0 && layout::PresetForSlot(2) == 1);
    assert(layout::PresetForSlot(3) == 2 && layout::PresetForSlot(4) == -1);
    assert(layout::PresetForSlot(5) == -1 && layout::PresetForSlot(6) == 3);
    assert(layout::PresetForSlot(7) == 4);
    assert(layout::StepAfterPresetActivation(8, 8) == 10);
    assert(layout::StepAfterPresetActivation(10, 8) == 8);
    assert(defaults.show_speed_indicator);
    assert(defaults.frames_per_step == 5);
    assert(defaults.preset_steps[0] == 8 && defaults.preset_steps[1] == 5);
    assert(defaults.preset_steps[2] == 2 && defaults.preset_steps[3] == 10 && defaults.preset_steps[4] == 10);
}
