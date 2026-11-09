#pragma once

#include <cstdint>

namespace sifucoop::tas {

bool InstallInputHook(std::uintptr_t module_base);
void SetInputController(void* controller);
void SetInputSuppressionState(bool focused, bool gameplay_supported);
void RemoveInputHook();

}
