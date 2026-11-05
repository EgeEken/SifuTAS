#pragma once

#include <cstdint>

#include "../tas/controls.h"

namespace sifucoop::hooks {

bool InstallTickHook(std::uintptr_t module_base, void* gengine);

void RemoveTickHook();

float FrameDeltaSeconds();

// Monotonic count of engine ticks since the hook was installed.
unsigned long long FrameCount();

void ActivateAction(tas::Action action);
void SetPlaybackSpeed(float speed);
float PlaybackSpeed();
bool IsPaused();

}
