#pragma once

#include "control_layout.h"

namespace sifucoop::tas {

enum class SlotKind : int { Pause, Speed, FrameOne, FrameMany };

enum Modifier : unsigned int {
    ModifierNone = 0,
    ModifierCtrl = 1,
    ModifierAlt = 2,
    ModifierShift = 4,
};

struct Binding {
    int key = 0;
    unsigned int modifiers = ModifierNone;
};

struct Settings {
    Binding bindings[layout::kSlotCount] = {
        {'1', ModifierNone}, {'2', ModifierNone}, {'4', ModifierNone}, {'5', ModifierNone},
        {'6', ModifierNone}, {'7', ModifierNone}, {0, ModifierNone}, {0, ModifierNone}};
    int preset_steps[layout::kPresetCount] = {8, 5, 2, 10, 10};
    int frames_per_step = 5;
    bool show_speed_indicator = true;
};

void LoadSettings();
void UpdateInputNames();
Settings GetSettings();
bool SetBinding(int slot, Binding binding);
bool SetPresetStep(int preset, int step);
bool SetFramesPerStep(int frames);
bool SetShowSpeedIndicator(bool show);
bool SaveSettings();
bool SettingsSaved();
bool WasGuiTogglePressed(bool focused);
bool WasSlotPressed(int slot, bool focused);
bool IsInputBinding(int key_comparison_index, int slot);
bool IsGuiToggleInput(int key_comparison_index);
bool BindingText(Binding binding, char* out, int out_size);
const char* SlotActionText(int slot);
SlotKind GetSlotKind(int slot);
int PresetIndexForSlot(int slot);
float SpeedForPreset(int preset);
int SpeedStep(float speed);

}
