#include "input_hook.h"

#include <windows.h>
#include <atomic>

#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../ue/reflection.h"
#include "controls.h"
#include "../ui/overlay.h"

namespace sifucoop::tas {
namespace {

struct FKey { std::uint32_t comparison_index; std::uint32_t number; };
static_assert(sizeof(FKey) == 8, "UE4 FKey ABI changed");
using InputKeyFn = bool(__fastcall*)(void*, FKey, std::uint32_t, float, bool);
InputKeyFn g_original = nullptr;
void* g_target = nullptr;
struct SuppressedKey { int name = 0; std::uint32_t number = 0; };
SuppressedKey g_suppressed[16] = {};
std::atomic<void*> g_local_controller{nullptr};
std::atomic<bool> g_focused{false};
std::atomic<bool> g_gameplay_supported{false};

unsigned int CurrentModifiers() {
    unsigned int value = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) value |= ModifierCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) value |= ModifierAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) value |= ModifierShift;
    return value;
}

bool __fastcall InputKeyHook(void* self, FKey key, std::uint32_t event, float amount,
                            bool gamepad) {
    void* local = g_local_controller.load(std::memory_order_relaxed);
    if (!self || self != local) return g_original(self, key, event, amount, gamepad);
    const bool gui_open = ui::IsMenuOpen();
    bool suppress = gui_open;
    SuppressedKey* free_slot = nullptr;
    SuppressedKey* held_slot = nullptr;
    for (SuppressedKey& item : g_suppressed) {
        if (item.name == static_cast<int>(key.comparison_index) && item.number == key.number)
            held_slot = &item;
        if (!item.name && !free_slot) free_slot = &item;
    }

    if (!g_focused.load(std::memory_order_relaxed)) {
        if (held_slot && (event == 1 || event == 2)) {
            if (event == 1) *held_slot = {};
            return false;
        }
        return g_original(self, key, event, amount, gamepad);
    }

    // EInputEvent::IE_Pressed is zero; releases remain suppressed after a
    // modifier is released so Unreal never receives an unmatched key-up.
    if (event == 0 && gui_open) {
        if (free_slot) *free_slot = {static_cast<int>(key.comparison_index), key.number};
    } else if (event == 0 && !gui_open) {
        suppress = IsGuiToggleInput(static_cast<int>(key.comparison_index));
        if (g_gameplay_supported.load(std::memory_order_relaxed)) {
            const Settings settings = GetSettings();
            for (int i = 0; i < tas::layout::kSlotCount; ++i) {
                if (settings.bindings[i].key &&
                    IsInputBinding(static_cast<int>(key.comparison_index), i) &&
                    CurrentModifiers() == settings.bindings[i].modifiers) {
                    suppress = true;
                    break;
                }
            }
        }
        if (suppress && free_slot)
            *free_slot = {static_cast<int>(key.comparison_index), key.number};
    } else if (held_slot && event == 1) {
        suppress = true;
        *held_slot = {};
    } else if (held_slot && event == 2) {
        suppress = true;
    }

    if (gui_open || suppress) return false;
    return g_original(self, key, event, amount, gamepad);
}

}

bool InstallInputHook(std::uintptr_t base) {
    if (!offsets::APlayerController_InputKey) {
        SC_LOG("input: APlayerController::InputKey unavailable; bound-input suppression disabled");
        return false;
    }
    g_target = reinterpret_cast<void*>(base + offsets::APlayerController_InputKey);
    MH_STATUS status = MH_CreateHook(g_target, reinterpret_cast<void*>(&InputKeyHook),
                                     reinterpret_cast<void**>(&g_original));
    if (status == MH_OK) status = MH_EnableHook(g_target);
    if (status != MH_OK) {
        SC_LOG("input: local APlayerController::InputKey hook failed (%d)", static_cast<int>(status));
        MH_RemoveHook(g_target);
        g_target = nullptr;
        g_original = nullptr;
        return false;
    }
    SC_LOG("input: local APlayerController::InputKey suppression hook installed");
    return true;
}

void SetInputController(void* controller) { g_local_controller.store(controller, std::memory_order_relaxed); }
void SetInputSuppressionState(bool focused, bool supported) {
    g_focused.store(focused, std::memory_order_relaxed);
    g_gameplay_supported.store(supported, std::memory_order_relaxed);
}

void RemoveInputHook() {
    if (!g_target) return;
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
    g_original = nullptr;
    g_local_controller.store(nullptr, std::memory_order_relaxed);
    SetInputSuppressionState(false, false);
}

}
