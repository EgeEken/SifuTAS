#include "controls.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/log.h"
#include "../ue/reflection.h"

namespace sifucoop::tas {
namespace {

constexpr int kSlotCount = 9;
constexpr int kPresetCount = 6;
constexpr unsigned int kModifierMask = ModifierCtrl | ModifierAlt | ModifierShift;
SRWLOCK g_settings_lock = SRWLOCK_INIT;
Settings g_settings = {};
int g_game_key_names[kSlotCount] = {};
int g_gui_key_name = 0;
std::atomic<bool> g_input_names_dirty{true};
bool g_previous[kSlotCount] = {};
bool g_gui_previous = false;
std::atomic<bool> g_settings_saved{true};
bool g_save_error_logged = false;

int DefaultKey(int slot) {
    return slot >= 0 && slot < 6 ? '1' + slot : 0;
}

Binding DefaultBinding(int slot) { return {DefaultKey(slot), ModifierNone}; }

bool ValidKey(int key) {
    if (key == 0) return true;
    return (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
           (key >= VK_F1 && key <= VK_F24) || (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) ||
           key == VK_MULTIPLY || key == VK_ADD || key == VK_SEPARATOR ||
           key == VK_SUBTRACT || key == VK_DECIMAL || key == VK_DIVIDE ||
           (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_SPACE || key == VK_TAB ||
           key == VK_BACK || key == VK_RETURN || key == VK_ESCAPE || key == VK_INSERT ||
           key == VK_DELETE || key == VK_HOME || key == VK_END || key == VK_SNAPSHOT ||
           key == VK_PAUSE || key == VK_CAPITAL || key == VK_NUMLOCK ||
           key == VK_SCROLL || key == VK_OEM_1 || key == VK_OEM_PLUS ||
           key == VK_OEM_COMMA || key == VK_OEM_MINUS || key == VK_OEM_PERIOD ||
           key == VK_OEM_2 || key == VK_OEM_3 || key == VK_OEM_4 ||
           key == VK_OEM_5 || key == VK_OEM_6 || key == VK_OEM_7 ||
           key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON ||
           key == VK_XBUTTON1 || key == VK_XBUTTON2;
}

bool ValidBinding(Binding binding) {
    return ValidKey(binding.key) && (!binding.key ? binding.modifiers == ModifierNone : true) &&
           (binding.modifiers & ~kModifierMask) == 0 &&
           !(binding.key == VK_F1 && binding.key != 0);
}

bool Same(Binding a, Binding b) { return a.key == b.key && a.modifiers == b.modifiers; }

bool ConfigPath(char* out, int size) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ConfigPath), &module)) return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, path, MAX_PATH)) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    slash[1] = '\0';
    return _snprintf_s(out, size, _TRUNCATE, "%sSifuTAS.ini", path) >= 0;
}

void SetDefaults(Settings* settings) {
    *settings = Settings{};
    for (int i = 0; i < kSlotCount; ++i) settings->bindings[i] = DefaultBinding(i);
}

const wchar_t* UnrealName(int key) {
    static wchar_t name[2] = {};
    if (key >= 'A' && key <= 'Z') { name[0] = static_cast<wchar_t>(key); return name; }
    if (key >= '0' && key <= '9') {
        static const wchar_t* digits[] = {L"Zero", L"One", L"Two", L"Three", L"Four", L"Five", L"Six", L"Seven", L"Eight", L"Nine"};
        return digits[key - '0'];
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        static const wchar_t* digits[] = {L"NumPadZero", L"NumPadOne", L"NumPadTwo", L"NumPadThree", L"NumPadFour", L"NumPadFive", L"NumPadSix", L"NumPadSeven", L"NumPadEight", L"NumPadNine"};
        return digits[key - VK_NUMPAD0];
    }
    if (key >= VK_F1 && key <= VK_F24) {
        static wchar_t function[5] = {};
        _snwprintf_s(function, _TRUNCATE, L"F%d", key - VK_F1 + 1);
        return function;
    }
    switch (key) {
        case VK_SPACE: return L"SpaceBar"; case VK_TAB: return L"Tab";
        case VK_BACK: return L"BackSpace"; case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Escape"; case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down";
        case VK_PRIOR: return L"PageUp"; case VK_NEXT: return L"PageDown";
        case VK_HOME: return L"Home"; case VK_END: return L"End";
        case VK_INSERT: return L"Insert"; case VK_DELETE: return L"Delete";
        case VK_CAPITAL: return L"CapsLock"; case VK_SNAPSHOT: return L"PrintScreen";
        case VK_PAUSE: return L"Pause"; case VK_NUMLOCK: return L"NumLock";
        case VK_SCROLL: return L"ScrollLock";
        case VK_MULTIPLY: return L"Multiply"; case VK_ADD: return L"Add";
        case VK_SEPARATOR: return L"Separator"; case VK_SUBTRACT: return L"Subtract";
        case VK_DECIMAL: return L"Decimal"; case VK_DIVIDE: return L"Divide";
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return L"LeftShift";
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return L"LeftControl";
        case VK_LMENU: case VK_RMENU: case VK_MENU: return L"LeftAlt";
        case VK_LBUTTON: return L"LeftMouseButton"; case VK_RBUTTON: return L"RightMouseButton";
        case VK_MBUTTON: return L"MiddleMouseButton"; case VK_XBUTTON1: return L"ThumbMouseButton";
        case VK_XBUTTON2: return L"ThumbMouseButton2";
        case VK_OEM_PLUS: return L"Equals"; case VK_OEM_MINUS: return L"Hyphen";
        case VK_OEM_COMMA: return L"Comma"; case VK_OEM_PERIOD: return L"Period";
        case VK_OEM_1: return L"Semicolon"; case VK_OEM_2: return L"Slash";
        case VK_OEM_3: return L"Tilde"; case VK_OEM_4: return L"LeftBracket";
        case VK_OEM_5: return L"Backslash"; case VK_OEM_6: return L"RightBracket";
        case VK_OEM_7: return L"Apostrophe";
        default: return nullptr;
    }
}

void RefreshInputNames() {
    ue::FName name = {};
    if (ue::MakeFName(L"F1", &name)) g_gui_key_name = name.comparison_index;
    for (int i = 0; i < kSlotCount; ++i) {
        g_game_key_names[i] = 0;
        const wchar_t* text = UnrealName(g_settings.bindings[i].key);
        if (text && ue::MakeFName(text, &name)) g_game_key_names[i] = name.comparison_index;
    }
}

void WriteValue(const char* key, const char* value, const char* path, bool* result) {
    *result = WritePrivateProfileStringA("time_control", key, value, path) != FALSE && *result;
}

bool SaveLocked() {
    char path[MAX_PATH] = {};
    if (!ConfigPath(path, sizeof(path))) return g_settings_saved.exchange(false);
    bool saved = true;
    char key[32] = {}, value[48] = {};
    WriteValue("version", "1", path, &saved);
    for (int i = 0; i < kSlotCount; ++i) {
        _snprintf_s(key, sizeof(key), _TRUNCATE, "slot_%d", i + 1);
        _snprintf_s(value, sizeof(value), _TRUNCATE, "%d,%u", g_settings.bindings[i].key,
                    g_settings.bindings[i].modifiers);
        WriteValue(key, value, path, &saved);
    }
    for (int i = 0; i < kPresetCount; ++i) {
        _snprintf_s(key, sizeof(key), _TRUNCATE, "preset_%d", i + 1);
        _snprintf_s(value, sizeof(value), _TRUNCATE, "%d", g_settings.preset_steps[i]);
        WriteValue(key, value, path, &saved);
    }
    _snprintf_s(value, sizeof(value), _TRUNCATE, "%d", g_settings.frames_per_step);
    WriteValue("frames_per_step", value, path, &saved);
    WriteValue("show_speed_indicator", g_settings.show_speed_indicator ? "1" : "0", path,
               &saved);
    g_settings_saved.store(saved, std::memory_order_relaxed);
    if (!saved && !g_save_error_logged) {
        SC_LOG("time-control: couldn't save SifuTAS.ini (error %lu)", GetLastError());
        g_save_error_logged = true;
    }
    if (saved) g_save_error_logged = false;
    return saved;
}

bool IsDown(int key) { return key && (GetAsyncKeyState(key) & 0x8000) != 0; }

unsigned int CurrentModifiers() {
    unsigned int modifiers = 0;
    if (IsDown(VK_CONTROL)) modifiers |= ModifierCtrl;
    if (IsDown(VK_MENU)) modifiers |= ModifierAlt;
    if (IsDown(VK_SHIFT)) modifiers |= ModifierShift;
    return modifiers;
}

bool ConfigUnique(const Settings& settings, int slot, Binding binding) {
    if (!binding.key) return true;
    for (int i = 0; i < kSlotCount; ++i)
        if (i != slot && settings.bindings[i].key && Same(settings.bindings[i], binding)) return false;
    return true;
}

}

void LoadSettings() {
    AcquireSRWLockExclusive(&g_settings_lock);
    SetDefaults(&g_settings);
    char path[MAX_PATH] = {};
    if (ConfigPath(path, sizeof(path))) {
        const int version = GetPrivateProfileIntA("time_control", "version", 0, path);
        if (version == 1) {
            Settings loaded = g_settings;
            char key[32] = {}, value[64] = {};
            bool duplicate = false;
            Binding candidates[kSlotCount] = {};
            for (int i = 0; i < kSlotCount; ++i) {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "slot_%d", i + 1);
                GetPrivateProfileStringA("time_control", key, "", value, sizeof(value), path);
                int vk = 0; unsigned int mods = 0;
                if (sscanf_s(value, "%d,%u", &vk, &mods) == 2 && ValidBinding({vk, mods}))
                    candidates[i] = {vk, mods};
                else candidates[i] = DefaultBinding(i);
            }
            for (int i = 0; i < kSlotCount; ++i)
                for (int j = i + 1; j < kSlotCount; ++j)
                    if (candidates[i].key && Same(candidates[i], candidates[j])) duplicate = true;
            if (duplicate) {
                for (int i = 0; i < kSlotCount; ++i) loaded.bindings[i] = DefaultBinding(i);
                SC_LOG("time-control: duplicate shortcut in SifuTAS.ini; defaults loaded");
            } else {
                for (int i = 0; i < kSlotCount; ++i) loaded.bindings[i] = candidates[i];
            }
            g_settings = loaded;
            for (int i = 0; i < kPresetCount; ++i) {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "preset_%d", i + 1);
                const int step = GetPrivateProfileIntA("time_control", key, loaded.preset_steps[i], path);
                if (step >= 1 && step <= 20) g_settings.preset_steps[i] = step;
            }
            const int frames = GetPrivateProfileIntA("time_control", "frames_per_step", 5, path);
            if (frames >= 2 && frames <= 10) g_settings.frames_per_step = frames;
            const int indicator = GetPrivateProfileIntA("time_control", "show_speed_indicator", 1, path);
            if (indicator == 0 || indicator == 1) g_settings.show_speed_indicator = indicator != 0;
            if (duplicate) SC_LOG("time-control: invalid or duplicate binding; safe defaults retained for affected slots");
        } else {
            SC_LOG("time-control: applying version 1 defaults; legacy TAS bindings preserved");
        }
    }
    g_input_names_dirty.store(true, std::memory_order_relaxed);
    SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
}

Settings GetSettings() {
    AcquireSRWLockShared(&g_settings_lock);
    const Settings copy = g_settings;
    ReleaseSRWLockShared(&g_settings_lock);
    return copy;
}

bool SetBinding(int slot, Binding binding) {
    if (slot < 0 || slot >= kSlotCount || !ValidBinding(binding)) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    if (!ConfigUnique(g_settings, slot, binding)) { ReleaseSRWLockExclusive(&g_settings_lock); return false; }
    g_settings.bindings[slot] = binding;
    g_input_names_dirty.store(true, std::memory_order_relaxed);
    SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetPresetStep(int preset, int step) {
    if (preset < 0 || preset >= kPresetCount || step < 1 || step > 20) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.preset_steps[preset] = step;
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetFramesPerStep(int frames) {
    if (frames < 2 || frames > 10) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.frames_per_step = frames;
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetShowSpeedIndicator(bool show) {
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.show_speed_indicator = show;
    SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SaveSettings() { AcquireSRWLockExclusive(&g_settings_lock); const bool ok = SaveLocked(); ReleaseSRWLockExclusive(&g_settings_lock); return ok; }
bool SettingsSaved() { return g_settings_saved.load(std::memory_order_relaxed); }

bool WasGuiTogglePressed(bool focused) {
    const bool down = IsDown(VK_F1);
    const bool pressed = down && !g_gui_previous;
    g_gui_previous = down;
    return focused && pressed;
}

bool WasSlotPressed(int slot, bool focused) {
    if (slot < 0 || slot >= kSlotCount) return false;
    const Settings settings = GetSettings();
    const Binding binding = settings.bindings[slot];
    const bool down = binding.key && IsDown(binding.key) && CurrentModifiers() == binding.modifiers;
    const bool pressed = down && !g_previous[slot];
    g_previous[slot] = down;
    return focused && pressed;
}

bool IsGuiToggleInput(int key_name) {
    AcquireSRWLockShared(&g_settings_lock);
    const bool found = key_name && key_name == g_gui_key_name;
    ReleaseSRWLockShared(&g_settings_lock);
    return found;
}

bool IsInputBinding(int key_name, int slot) {
    if (!key_name || slot < 0 || slot >= kSlotCount) return false;
    AcquireSRWLockShared(&g_settings_lock);
    const bool found = g_game_key_names[slot] && g_game_key_names[slot] == key_name;
    ReleaseSRWLockShared(&g_settings_lock);
    return found;
}

void UpdateInputNames() {
    if (!g_input_names_dirty.exchange(false, std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_settings_lock);
    RefreshInputNames();
    ReleaseSRWLockExclusive(&g_settings_lock);
}

SlotKind GetSlotKind(int slot) {
    if (slot == 0) return SlotKind::Pause;
    if (slot == 4) return SlotKind::FrameOne;
    if (slot == 5) return SlotKind::FrameMany;
    return SlotKind::Speed;
}

int PresetIndexForSlot(int slot) {
    if (slot >= 1 && slot <= 3) return slot - 1;
    if (slot >= 6 && slot <= 8) return slot - 3;
    return -1;
}

float SpeedForPreset(int preset) {
    const Settings settings = GetSettings();
    if (preset < 0 || preset >= kPresetCount) return 1.f;
    return static_cast<float>(settings.preset_steps[preset]) / 10.f;
}

int SpeedStep(float speed) {
    if (!std::isfinite(speed)) return 10;
    if (speed < 0.1f) speed = 0.1f;
    if (speed > 2.f) speed = 2.f;
    return static_cast<int>(std::lround(speed * 10.f));
}

const char* SlotActionText(int slot) {
    switch (GetSlotKind(slot)) {
        case SlotKind::Pause: return "Pause / Resume";
        case SlotKind::Speed: return "Speed preset";
        case SlotKind::FrameOne: return "Advance 1 frame";
        case SlotKind::FrameMany: return "Advance N frames";
    }
    return "";
}

bool BindingText(Binding binding, char* out, int size) {
    if (!out || size <= 0) return false;
    if (!binding.key) return _snprintf_s(out, size, _TRUNCATE, "Unbound") >= 0;
    char key[32] = {};
    if (binding.key >= 'A' && binding.key <= 'Z') _snprintf_s(key, sizeof(key), _TRUNCATE, "%c", binding.key);
    else if (binding.key >= '0' && binding.key <= '9') _snprintf_s(key, sizeof(key), _TRUNCATE, "%c", binding.key);
    else if (binding.key >= VK_F1 && binding.key <= VK_F24) _snprintf_s(key, sizeof(key), _TRUNCATE, "F%d", binding.key - VK_F1 + 1);
    else if (binding.key >= VK_NUMPAD0 && binding.key <= VK_NUMPAD9) _snprintf_s(key, sizeof(key), _TRUNCATE, "Num %d", binding.key - VK_NUMPAD0);
    else if (binding.key == VK_ADD) strcpy_s(key, "Num +");
    else if (binding.key == VK_SUBTRACT) strcpy_s(key, "Num -");
    else if (binding.key == VK_MULTIPLY) strcpy_s(key, "Num *");
    else if (binding.key == VK_DIVIDE) strcpy_s(key, "Num /");
    else if (binding.key == VK_DECIMAL) strcpy_s(key, "Num .");
    else if (binding.key == VK_LBUTTON) strcpy_s(key, "Mouse 1");
    else if (binding.key == VK_RBUTTON) strcpy_s(key, "Mouse 2");
    else if (binding.key == VK_MBUTTON) strcpy_s(key, "Mouse 3");
    else if (binding.key == VK_XBUTTON1) strcpy_s(key, "Mouse 4");
    else if (binding.key == VK_XBUTTON2) strcpy_s(key, "Mouse 5");
    else if (binding.key == VK_SPACE) strcpy_s(key, "Space");
    else if (binding.key == VK_LEFT) strcpy_s(key, "Left Arrow");
    else if (binding.key == VK_RIGHT) strcpy_s(key, "Right Arrow");
    else if (binding.key == VK_UP) strcpy_s(key, "Up Arrow");
    else if (binding.key == VK_DOWN) strcpy_s(key, "Down Arrow");
    else if (binding.key == VK_PRIOR) strcpy_s(key, "Page Up");
    else if (binding.key == VK_NEXT) strcpy_s(key, "Page Down");
    else if (binding.key == VK_RETURN) strcpy_s(key, "Enter");
    else if (binding.key == VK_ESCAPE) strcpy_s(key, "Escape");
    else if (binding.key == VK_TAB) strcpy_s(key, "Tab");
    else if (binding.key == VK_HOME) strcpy_s(key, "Home");
    else if (binding.key == VK_END) strcpy_s(key, "End");
    else if (binding.key == VK_INSERT) strcpy_s(key, "Insert");
    else if (binding.key == VK_DELETE) strcpy_s(key, "Delete");
    else if (binding.key == VK_BACK) strcpy_s(key, "Backspace");
    else if (binding.key == VK_CAPITAL) strcpy_s(key, "Caps Lock");
    else if (binding.key == VK_NUMLOCK) strcpy_s(key, "Num Lock");
    else if (binding.key == VK_SCROLL) strcpy_s(key, "Scroll Lock");
    else if (binding.key == VK_SNAPSHOT) strcpy_s(key, "Print Screen");
    else if (binding.key == VK_PAUSE) strcpy_s(key, "Pause");
    else if (binding.key == VK_OEM_PLUS) strcpy_s(key, "Equals");
    else if (binding.key == VK_OEM_MINUS) strcpy_s(key, "Minus");
    else if (binding.key == VK_OEM_COMMA) strcpy_s(key, "Comma");
    else if (binding.key == VK_OEM_PERIOD) strcpy_s(key, "Period");
    else if (binding.key == VK_OEM_1) strcpy_s(key, "Semicolon");
    else if (binding.key == VK_OEM_2) strcpy_s(key, "Slash");
    else if (binding.key == VK_OEM_3) strcpy_s(key, "Tilde");
    else if (binding.key == VK_OEM_4) strcpy_s(key, "Left Bracket");
    else if (binding.key == VK_OEM_5) strcpy_s(key, "Backslash");
    else if (binding.key == VK_OEM_6) strcpy_s(key, "Right Bracket");
    else if (binding.key == VK_OEM_7) strcpy_s(key, "Apostrophe");
    else return false;
    char prefix[24] = {};
    if (binding.modifiers & ModifierCtrl) strcat_s(prefix, "Ctrl+");
    if (binding.modifiers & ModifierAlt) strcat_s(prefix, "Alt+");
    if (binding.modifiers & ModifierShift) strcat_s(prefix, "Shift+");
    return _snprintf_s(out, size, _TRUNCATE, "%s%s", prefix, key) >= 0;
}

}
