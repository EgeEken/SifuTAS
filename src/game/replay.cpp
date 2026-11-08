#include "replay.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../core/log.h"
#include "../ue/reflection.h"
#include "actors.h"
#include "../core/hooks.h"

namespace sifucoop::game {
namespace {

constexpr ULONGLONG kStartupTimeoutMs = 10000;
constexpr ULONGLONG kFinalizeTimeoutMs = 15000;
constexpr ULONGLONG kPollIntervalMs = 200;
constexpr std::uint8_t kManualStopReason = 1;

SRWLOCK g_status_lock = SRWLOCK_INIT;
ReplayRecordingStatus g_status = {};
std::atomic<bool> g_toggle_requested{false};
ULONGLONG g_request_time_ms = 0;
ULONGLONG g_recording_start_ms = 0;
ULONGLONG g_last_poll_ms = 0;
ue::UObject* g_replay_cdo = nullptr;
bool g_start_requested = false;

ue::UObject* ResolveReplaySystem() {
    if (ue::IsAliveObject(g_replay_cdo)) return g_replay_cdo;
    g_replay_cdo = ue::FindObjectByPath(L"/Script/Sifu.Default__ReplaySystem");
    return ue::IsAliveObject(g_replay_cdo) ? g_replay_cdo : nullptr;
}

struct WorldContextBool {
    ue::UObject* WorldContextObject = nullptr;
    bool ReturnValue = false;
};

struct WorldContextFloat {
    ue::UObject* WorldContextObject = nullptr;
    float ReturnValue = 0.f;
};

struct WorldContextOnly {
    ue::UObject* WorldContextObject = nullptr;
};

struct WorldContextObjectOut {
    ue::UObject* WorldContextObject = nullptr;
    ue::UObject* ReturnValue = nullptr;
};

struct StopRecordingParams {
    ue::UObject* WorldContextObject = nullptr;
    std::uint8_t Reason = 0;
};

bool CallBool(ue::UObject* system, const wchar_t* name, ue::UObject* world,
              bool* out) {
    if (!system || !out) return false;
    WorldContextBool params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(system, name, &params)) return false;
    *out = params.ReturnValue;
    return true;
}

bool ReadRecordingState(ue::UObject* system, ue::UObject* world, bool* recording) {
    return CallBool(system, L"BPF_IsRecording", world, recording);
}

bool ReadDemoDriver(ue::UObject* system, ue::UObject* world,
                    ue::UObject** driver) {
    if (!system || !driver) return false;
    WorldContextObjectOut params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(system, L"BPF_GetDemoNetDriver", &params)) return false;
    *driver = ue::IsAliveObject(params.ReturnValue) ? params.ReturnValue : nullptr;
    return true;
}

bool ReadReplaySystemInstance(ue::UObject* system, ue::UObject* world,
                              ue::UObject** instance) {
    if (!system || !instance) return false;
    WorldContextObjectOut params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(system, L"BPF_GetReplaySystem", &params)) return false;
    *instance = ue::IsAliveObject(params.ReturnValue) ? params.ReturnValue : nullptr;
    return true;
}

bool ReadReplayLength(ue::UObject* system, ue::UObject* world, float* seconds) {
    if (!system || !seconds) return false;
    WorldContextFloat params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(system, L"BPF_GetCurrentReplayLengthS", &params)) return false;
    *seconds = params.ReturnValue;
    return true;
}

void SetStatus(ReplayRecordingState state, const char* message, float elapsed,
               bool system_available, bool driver_available) {
    AcquireSRWLockExclusive(&g_status_lock);
    const bool changed = g_status.state != state ||
                         std::strcmp(g_status.message, message) != 0;
    g_status.state = state;
    lstrcpynA(g_status.message, message, sizeof(g_status.message));
    g_status.elapsed_seconds = elapsed;
    g_status.replay_system_available = system_available;
    g_status.demo_driver_available = driver_available;
    ReleaseSRWLockExclusive(&g_status_lock);
    if (changed) SC_LOG("replay: %s", message);
}

ReplayRecordingStatus GetStatusCopy() {
    AcquireSRWLockShared(&g_status_lock);
    ReplayRecordingStatus copy = g_status;
    ReleaseSRWLockShared(&g_status_lock);
    return copy;
}

void Fail(ReplayRecordingState state, const char* message, bool system_available,
          bool driver_available = false) {
    SetStatus(state, message, 0.f, system_available, driver_available);
    g_start_requested = false;
    g_recording_start_ms = 0;
}

void StartRecording() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const char* gameplay_reason = nullptr;
    if (!SupportedTASGameplayState(world, player, &gameplay_reason)) {
        char message[192] = {};
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "Unsupported in current mode: %s",
                    gameplay_reason ? gameplay_reason : "active gameplay unavailable");
        Fail(ReplayRecordingState::UnsupportedMode, message, false);
        return;
    }

    ue::UObject* system = ResolveReplaySystem();
    if (!system) {
        Fail(ReplayRecordingState::SystemUnavailable,
             "Replay system unavailable at /Script/Sifu.Default__ReplaySystem.", false);
        return;
    }
    ue::UObject* instance = nullptr;
    if (!ReadReplaySystemInstance(system, world, &instance) || !instance) {
        Fail(ReplayRecordingState::SystemUnavailable,
             "Sifu's native UReplaySystem instance is unavailable in this world.", false);
        return;
    }

    bool disabled = false;
    if (!CallBool(system, L"BPF_IsRecordingDisabled", world, &disabled)) {
        Fail(ReplayRecordingState::Failed,
             "Could not query Sifu's native recording-disabled state.", true);
        return;
    }
    if (disabled) {
        Fail(ReplayRecordingState::DisabledByGame,
             "Recording is disabled by Sifu in this game state.", true);
        return;
    }

    bool can_start = false;
    if (!CallBool(system, L"BPF_CanStartRecording", world, &can_start)) {
        Fail(ReplayRecordingState::Failed,
             "Sifu's recording eligibility API is unavailable.", true);
        return;
    }
    if (!can_start) {
        Fail(ReplayRecordingState::UnsupportedMode,
             "Sifu reports native replay recording is unsupported in this mode.", true);
        return;
    }

    bool recording = false;
    bool playing = false;
    if (!ReadRecordingState(system, world, &recording) ||
        !CallBool(system, L"BPF_IsPlaying", world, &playing)) {
        Fail(ReplayRecordingState::Failed,
             "Could not query Sifu's native replay status.", true);
        return;
    }
    if (recording) {
        ue::UObject* driver = nullptr;
        const bool have_driver = ReadDemoDriver(system, world, &driver) && driver;
        g_recording_start_ms = GetTickCount64();
        SetStatus(ReplayRecordingState::Active,
                  have_driver
                      ? "Native Sifu replay recording is already active."
                      : "Sifu reports recording active, but its demo driver is unavailable.",
                  0.f, true,
                  have_driver);
        return;
    }
    if (playing) {
        Fail(ReplayRecordingState::UnsupportedMode,
             "Stop native replay playback before starting a recording.", true);
        return;
    }

    if (!hooks::ReleaseGuiOwnedPauseForRecording()) {
        Fail(ReplayRecordingState::Failed,
             "Resume gameplay before recording; TAS pause is not owned by the GUI.",
             true);
        return;
    }

    WorldContextOnly start = {};
    start.WorldContextObject = world;
    if (!ue::CallFunction(system, L"BPF_ReplayStartRecording", &start)) {
        Fail(ReplayRecordingState::Failed,
             "Sifu rejected the native recording start call.", true);
        return;
    }

    g_start_requested = true;
    g_request_time_ms = GetTickCount64();
    g_last_poll_ms = 0;
    g_recording_start_ms = 0;
    SetStatus(ReplayRecordingState::Requested,
              "Recording requested; waiting for Sifu's replay system and demo driver.",
              0.f, true, false);
}

void StopRecording() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* system = ResolveReplaySystem();
    if (!world || !system) {
        Fail(ReplayRecordingState::Failed,
             "Could not finalize recording: replay system or world is unavailable.",
             system != nullptr);
        return;
    }

    const bool gameplay_can_tick = hooks::ReleaseGuiOwnedPauseForRecording();

    StopRecordingParams stop = {};
    stop.WorldContextObject = world;
    stop.Reason = kManualStopReason;
    if (!ue::CallFunction(system, L"BPF_ReplayStopRecording", &stop)) {
        Fail(ReplayRecordingState::Failed,
             "Sifu rejected the native recording stop call.", true);
        return;
    }

    bool recording = true;
    const bool status_read = ReadRecordingState(system, world, &recording);
    g_start_requested = false;
    g_request_time_ms = GetTickCount64();
    g_last_poll_ms = 0;
    SetStatus(ReplayRecordingState::Finalizing,
              !gameplay_can_tick
                  ? "Stop requested; TAS pause is user-owned. Resume to let Sifu finish finalizing."
                  : (status_read && !recording
                         ? "Stop requested; waiting for Sifu to finalize the replay stream."
                         : "Stop requested; waiting for recording shutdown and replay finalization."),
              GetStatusCopy().elapsed_seconds, true, false);
}

void PollRecording() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_poll_ms < kPollIntervalMs) return;
    g_last_poll_ms = now;
    const ReplayRecordingStatus previous = GetStatusCopy();
    if (previous.state != ReplayRecordingState::Requested &&
        previous.state != ReplayRecordingState::Starting &&
        previous.state != ReplayRecordingState::Active &&
        previous.state != ReplayRecordingState::Finalizing)
        return;

    ue::UObject* system = ResolveReplaySystem();
    ue::UObject* world = ue::GetWorld();
    if (!system || !world) {
        if (g_start_requested)
            Fail(ReplayRecordingState::Failed,
                 "Recording startup failed: replay system or world disappeared.",
                 system != nullptr);
        return;
    }

    bool recording = false;
    if (!ReadRecordingState(system, world, &recording)) {
        Fail(ReplayRecordingState::Failed,
             "Could not read Sifu's native recording status.", true);
        return;
    }
    ue::UObject* replay_instance = nullptr;
    const bool replay_system_active =
        ReadReplaySystemInstance(system, world, &replay_instance) && replay_instance;
    ue::UObject* driver = nullptr;
    const bool driver_query_ok = ReadDemoDriver(system, world, &driver);
    const bool driver_available = driver_query_ok && driver;

    if (previous.state == ReplayRecordingState::Finalizing) {
        float length = -1.f;
        const bool length_ok = ReadReplayLength(system, world, &length);
        if (!recording && length_ok && length > 0.f) {
            char message[192] = {};
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "Recording finalized (%.2f s). Check Sifu's Replay Editor.", length);
            SetStatus(ReplayRecordingState::Finalized, message, length, true,
                      driver_available);
            g_recording_start_ms = 0;
            return;
        }
        if (now - g_request_time_ms > kFinalizeTimeoutMs) {
            Fail(ReplayRecordingState::Failed,
                 "Recording stop timed out before Sifu finalized a replay stream.", true,
                 driver_available);
            return;
        }
        SetStatus(ReplayRecordingState::Finalizing,
                  "Sifu is finalizing the native replay stream.",
                  previous.elapsed_seconds, true, driver_available);
        return;
    }

    if (recording && driver_available && replay_system_active) {
        if (!g_recording_start_ms) g_recording_start_ms = now;
        g_start_requested = false;
        SetStatus(ReplayRecordingState::Active,
                  "Native Sifu replay recording is active. Close the GUI to play.",
                  static_cast<float>(now - g_recording_start_ms) / 1000.f, true, true);
        return;
    }

    if (g_start_requested) {
        if (now - g_request_time_ms > kStartupTimeoutMs) {
            Fail(ReplayRecordingState::Failed,
                 !replay_system_active
                     ? "Recording startup failed: Sifu's UReplaySystem instance never became available."
                     : (driver_query_ok
                            ? "Recording startup failed: Sifu never activated recording with a demo driver."
                            : "Recording startup failed: Sifu's demo-driver query is unavailable."),
                 true, driver_available);
            return;
        }
        SetStatus(ReplayRecordingState::Starting,
                  !replay_system_active
                      ? "Start requested; waiting for Sifu's native UReplaySystem instance."
                      : (driver_query_ok
                             ? "Start requested; waiting for Sifu recording and demo-driver startup."
                             : "Start requested; waiting for Sifu's demo-driver query."),
                  0.f, replay_system_active, driver_available);
        return;
    }

    if (previous.state == ReplayRecordingState::Active && !recording) {
        float length = -1.f;
        const bool length_ok = ReadReplayLength(system, world, &length);
        if (length_ok && length > 0.f) {
            char message[192] = {};
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "Recording finalized by Sifu (%.2f s). Check Replay Editor.", length);
            SetStatus(ReplayRecordingState::Finalized, message, length, true,
                      driver_available);
        } else {
            Fail(ReplayRecordingState::Failed,
                 "Sifu stopped recording unexpectedly without a confirmed replay length.",
                 true, driver_available);
        }
        g_recording_start_ms = 0;
        return;
    }

    if (previous.state == ReplayRecordingState::Active) {
        SetStatus(ReplayRecordingState::Active,
                  driver_available
                      ? "Native Sifu replay recording is active. Close the GUI to play."
                      : "Sifu reports recording active, but its demo driver is unavailable.",
                  static_cast<float>(now - g_recording_start_ms) / 1000.f,
                  replay_system_active,
                  driver_available);
    }
}

void ProcessToggleRequest() {
    if (!g_toggle_requested.exchange(false, std::memory_order_relaxed)) return;

    const ReplayRecordingStatus status = GetStatusCopy();
    if (status.state == ReplayRecordingState::Active ||
        status.state == ReplayRecordingState::Starting ||
        status.state == ReplayRecordingState::Requested) {
        StopRecording();
    } else if (status.state == ReplayRecordingState::Finalizing) {
        SetStatus(ReplayRecordingState::Finalizing,
                  "Sifu is already finalizing the replay stream.",
                  status.elapsed_seconds, status.replay_system_available,
                  status.demo_driver_available);
    } else {
        StartRecording();
    }
}

}  // namespace

void QueueReplayRecordingToggle() {
    g_toggle_requested.store(true, std::memory_order_relaxed);
}

void TickReplayRecording() {
    ProcessToggleRequest();
    PollRecording();
}

ReplayRecordingStatus GetReplayRecordingStatus() { return GetStatusCopy(); }

}  // namespace sifucoop::game
