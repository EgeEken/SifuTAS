#pragma once

namespace sifucoop::tas {

enum class Action : int {
    PauseResume,
    FrameAdvance,
    SpeedHalf,
    SpeedFifth,
    SpeedTenth,
    ToggleGui,
    SaveCheckpoint,
    LoadCheckpoint,
    Count,
};

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
    Binding bindings[static_cast<int>(Action::Count)] = {};
    float speeds[3] = {0.5f, 0.2f, 0.1f};
    int frames_per_step = 1;
    float gui_scale = 1.4f;
};

void LoadSettings();
Settings GetSettings();
bool SetBinding(Action action, Binding binding);
bool SetSpeed(int index, float speed);
bool SetFramesPerStep(int frames);
bool SetGuiScale(float scale);
bool SaveSettings();
bool SettingsSaved();
bool WasActionPressed(Action action, bool focused);
void ConsumeBindingKeyUntilRelease(int key);
void ReleaseBindingKey(int key);
const char* ActionName(Action action);
bool BindingText(Binding binding, char* out, int out_size);

}
