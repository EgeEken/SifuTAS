#pragma once

#include <windows.h>
#include <cstdint>

#include "../tas/controls.h"

namespace sifucoop::hooks {

bool InstallTickHook(std::uintptr_t module_base, void* gengine);
void RemoveTickHook();
float FrameDeltaSeconds();
unsigned long long FrameCount();
void ActivateSlot(int slot);
void AdvanceFrames(int count);
void ToggleGui();
void SetPaused(bool paused);
void SetPlaybackSpeed(float speed);
float PlaybackSpeed();
bool IsPaused();
bool IsGameplaySupported();
ULONGLONG SpeedIndicatorUntil();

}
