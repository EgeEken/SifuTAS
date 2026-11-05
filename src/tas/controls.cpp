#include "controls.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../core/log.h"

namespace sifucoop::tas {
namespace {

constexpr int kActionCount = static_cast<int>(Action::Count);
SRWLOCK g_settings_lock = SRWLOCK_INIT;
Settings g_settings = {};
bool g_previous[kActionCount] = {};
std::atomic<bool> g_settings_saved{true};
bool g_save_error_logged = false;

int DefaultKey(Action action) {
    return VK_F1 + static_cast<int>(action);
}

bool ValidBinding(Binding binding) {
    return binding.key >= VK_F1 && binding.key <= VK_F12 &&
           (binding.modifiers & ~(ModifierCtrl | ModifierAlt | ModifierShift)) == 0;
}

bool UniqueBinding(const Settings& settings, Action action, Binding binding) {
    for (int i = 0; i < kActionCount; ++i) {
        if (i == static_cast<int>(action)) continue;
        if (settings.bindings[i].key == binding.key &&
            settings.bindings[i].modifiers == binding.modifiers)
            return false;
    }
    return true;
}

bool IsDown(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

unsigned int CurrentModifiers() {
    unsigned int modifiers = ModifierNone;
    if (IsDown(VK_CONTROL)) modifiers |= ModifierCtrl;
    if (IsDown(VK_MENU)) modifiers |= ModifierAlt;
    if (IsDown(VK_SHIFT)) modifiers |= ModifierShift;
    return modifiers;
}

bool ConfigPath(char* out, int size) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ConfigPath), &module))
        return false;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    return _snprintf_s(out, size, _TRUNCATE, "%sSifuTAS.ini", path) >= 0;
}

bool SaveLocked() {
    char path[MAX_PATH] = {};
    if (!ConfigPath(path, sizeof(path))) {
        g_settings_saved.store(false, std::memory_order_relaxed);
        if (!g_save_error_logged) {
            SC_LOG("tas: couldn't resolve the SifuTAS.ini path beside the mod");
            g_save_error_logged = true;
        }
        return false;
    }
    bool saved = true;
    char text[32] = {};
    for (int i = 0; i < kActionCount; ++i) {
        const Binding binding = g_settings.bindings[i];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%d,%u", binding.key,
                    binding.modifiers);
        char key[32] = {};
        _snprintf_s(key, sizeof(key), _TRUNCATE, "binding_%d", i);
        saved = WritePrivateProfileStringA("tas", key, text, path) != FALSE && saved;
    }
    for (int i = 0; i < 3; ++i) {
        char key[24] = {};
        _snprintf_s(key, sizeof(key), _TRUNCATE, "speed_%d", i + 1);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%.4g", g_settings.speeds[i]);
        saved = WritePrivateProfileStringA("tas", key, text, path) != FALSE && saved;
    }
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", g_settings.frames_per_step);
    saved = WritePrivateProfileStringA("tas", "frames_per_step", text, path) != FALSE && saved;
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%.3f", g_settings.gui_scale);
    saved = WritePrivateProfileStringA("tas", "gui_scale", text, path) != FALSE && saved;
    g_settings_saved.store(saved, std::memory_order_relaxed);
    if (!saved && !g_save_error_logged) {
        SC_LOG("tas: couldn't save SifuTAS.ini beside the mod (Windows error %lu)",
               GetLastError());
        g_save_error_logged = true;
    } else if (saved) {
        g_save_error_logged = false;
    }
    return saved;
}

void SetDefaults(Settings* settings) {
    *settings = Settings{};
    for (int i = 0; i < kActionCount; ++i)
        settings->bindings[i] = {DefaultKey(static_cast<Action>(i)), ModifierNone};
}

}

void LoadSettings() {
    AcquireSRWLockExclusive(&g_settings_lock);
    SetDefaults(&g_settings);
    char path[MAX_PATH] = {};
    if (ConfigPath(path, sizeof(path))) {
        char value[64] = {};
        Settings loaded = g_settings;
        for (int i = 0; i < kActionCount; ++i) {
            char key[32] = {};
            _snprintf_s(key, sizeof(key), _TRUNCATE, "binding_%d", i);
            GetPrivateProfileStringA("tas", key, "", value, sizeof(value), path);
            int vk = 0;
            unsigned int modifiers = 0;
            if (sscanf_s(value, "%d,%u", &vk, &modifiers) == 2) {
                Binding binding{vk, modifiers};
                if (ValidBinding(binding)) loaded.bindings[i] = binding;
            }
        }
        bool unique = true;
        for (int i = 0; i < kActionCount; ++i) {
            for (int j = i + 1; j < kActionCount; ++j) {
                if (loaded.bindings[i].key == loaded.bindings[j].key &&
                    loaded.bindings[i].modifiers == loaded.bindings[j].modifiers)
                    unique = false;
            }
        }
        if (unique) {
            for (int i = 0; i < kActionCount; ++i)
                g_settings.bindings[i] = loaded.bindings[i];
        } else {
            SC_LOG("tas: duplicate shortcut in SifuTAS.ini; using default bindings");
        }
        for (int i = 0; i < 3; ++i) {
            char key[24] = {};
            _snprintf_s(key, sizeof(key), _TRUNCATE, "speed_%d", i + 1);
            GetPrivateProfileStringA("tas", key, "", value, sizeof(value), path);
            char* end = nullptr;
            const float speed = strtof(value, &end);
            if (end != value && std::isfinite(speed) && speed >= 0.01f && speed <= 4.f)
                g_settings.speeds[i] = speed;
        }
        const int frames = GetPrivateProfileIntA("tas", "frames_per_step", 1, path);
        if (frames >= 1 && frames <= 10) g_settings.frames_per_step = frames;
        GetPrivateProfileStringA("tas", "gui_scale", "", value, sizeof(value), path);
        char* scale_end = nullptr;
        const float gui_scale = strtof(value, &scale_end);
        if (scale_end != value && std::isfinite(gui_scale) &&
            gui_scale >= 0.8f && gui_scale <= 2.f)
            g_settings.gui_scale = gui_scale;
    }
    SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
    SC_LOG("tas: settings loaded from SifuTAS.ini");
}

Settings GetSettings() {
    AcquireSRWLockShared(&g_settings_lock);
    const Settings copy = g_settings;
    ReleaseSRWLockShared(&g_settings_lock);
    return copy;
}

bool SetBinding(Action action, Binding binding) {
    const int index = static_cast<int>(action);
    if (index < 0 || index >= kActionCount || !ValidBinding(binding)) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    if (!UniqueBinding(g_settings, action, binding)) {
        ReleaseSRWLockExclusive(&g_settings_lock);
        return false;
    }
    g_settings.bindings[index] = binding;
    SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetSpeed(int index, float speed) {
    if (index < 0 || index >= 3 || !std::isfinite(speed) || speed < 0.01f || speed > 4.f)
        return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.speeds[index] = speed;
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetFramesPerStep(int frames) {
    if (frames < 1 || frames > 10) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.frames_per_step = frames;
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SetGuiScale(float scale) {
    if (!std::isfinite(scale) || scale < 0.8f || scale > 2.f) return false;
    AcquireSRWLockExclusive(&g_settings_lock);
    g_settings.gui_scale = scale;
    ReleaseSRWLockExclusive(&g_settings_lock);
    return true;
}

bool SaveSettings() {
    AcquireSRWLockExclusive(&g_settings_lock);
    const bool saved = SaveLocked();
    ReleaseSRWLockExclusive(&g_settings_lock);
    return saved;
}

bool SettingsSaved() { return g_settings_saved.load(std::memory_order_relaxed); }

bool WasActionPressed(Action action, bool focused) {
    const int index = static_cast<int>(action);
    if (index < 0 || index >= kActionCount) return false;
    const Settings settings = GetSettings();
    const Binding binding = settings.bindings[index];
    const bool down = IsDown(binding.key) && CurrentModifiers() == binding.modifiers;
    const bool pressed = down && !g_previous[index];
    g_previous[index] = down;
    return focused && pressed;
}

const char* ActionName(Action action) {
    switch (action) {
        case Action::PauseResume: return "Pause / resume";
        case Action::FrameAdvance: return "Frame advance";
        case Action::SpeedHalf: return "Speed preset 1";
        case Action::SpeedFifth: return "Speed preset 2";
        case Action::SpeedTenth: return "Speed preset 3";
        case Action::ToggleGui: return "Toggle GUI";
        case Action::SaveCheckpoint: return "Save checkpoint";
        case Action::LoadCheckpoint: return "Load checkpoint";
        default: return "Unknown action";
    }
}

bool BindingText(Binding binding, char* out, int out_size) {
    if (!out || out_size <= 0) return false;
    char text[32] = {};
    int used = 0;
    if (binding.modifiers & ModifierCtrl) used += _snprintf_s(text + used, sizeof(text) - used, _TRUNCATE, "Ctrl+");
    if (binding.modifiers & ModifierAlt) used += _snprintf_s(text + used, sizeof(text) - used, _TRUNCATE, "Alt+");
    if (binding.modifiers & ModifierShift) used += _snprintf_s(text + used, sizeof(text) - used, _TRUNCATE, "Shift+");
    char key[8] = {};
    _snprintf_s(key, sizeof(key), _TRUNCATE, "F%d", binding.key - VK_F1 + 1);
    return _snprintf_s(out, out_size, _TRUNCATE, "%s%s", text, key) >= 0;
}

}
