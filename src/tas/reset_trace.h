#pragma once

#include <cstdint>

namespace sifucoop::tas::reset_trace {

void InstallHooks(std::uintptr_t module_base);
void RemoveHooks();

}
