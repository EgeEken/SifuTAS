#include "input_trace.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::input_trace {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

constexpr std::uint32_t kMaxEvents = 8192;
constexpr ULONGLONG kCaptureDurationMs = 5000;

enum class EventType : std::uint8_t {
    CaptureBegin,
    CaptureEnd,
    EngineBegin,
    EngineEnd,
    WorldBegin,
    WorldEnd,
    ControllerKey,
    PlayerInputKey,
    PlayerInputAxis,
    InputStackBegin,
    InputStackEnd,
};

struct Event {
    std::uint64_t order = 0;
    std::uint64_t time_us = 0;
    std::uint64_t engine_tick = 0;
    std::uint64_t world_tick = 0;
    std::uintptr_t object = 0;
    DWORD thread_id = 0;
    EventType type = EventType::CaptureBegin;
    std::uint32_t key_index = 0;
    std::uint32_t key_number = 0;
    std::uint32_t input_event = 0;
    std::uint32_t flags = 0;
    std::int32_t detail = 0;
    float value = 0.f;
    float delta = 0.f;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Event g_events[kMaxEvents] = {};
std::uint32_t g_event_count = 0;
std::uint32_t g_dropped_count = 0;
std::uint64_t g_order = 0;
std::atomic<std::uint64_t> g_engine_tick{0};
std::atomic<std::uint64_t> g_world_tick{0};
std::uint64_t g_start_qpc = 0;
std::uint64_t g_qpc_frequency = 0;
ULONGLONG g_start_ms = 0;
std::atomic<void*> g_local_controller{nullptr};
std::atomic<void*> g_local_player_input{nullptr};
std::atomic<State> g_state{State::Ready};
std::atomic<bool> g_capture_requested{false};
std::atomic<bool> g_dumping{false};
std::atomic<bool> g_player_input_available{false};
void* g_player_input_key_target = nullptr;
void* g_player_input_axis_target = nullptr;
void* g_process_input_stack_target = nullptr;

struct FKey {
    std::uint32_t comparison_index;
    std::uint32_t number;
};
static_assert(sizeof(FKey) == 8, "UE4 FKey ABI changed");

using PlayerInputKeyFn = bool(__fastcall*)(void*, FKey, std::uint32_t, float, bool);
using PlayerInputAxisFn = bool(__fastcall*)(void*, FKey, float, float, int, bool);
using ProcessInputStackFn = void(__fastcall*)(void*, const void*, float, bool);

PlayerInputKeyFn g_player_input_key = nullptr;
PlayerInputAxisFn g_player_input_axis = nullptr;
ProcessInputStackFn g_process_input_stack = nullptr;

std::uint64_t QpcNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}

std::uint64_t ElapsedMicroseconds() {
    const std::uint64_t now = QpcNow();
    return g_qpc_frequency && now >= g_start_qpc
               ? (now - g_start_qpc) * 1000000ull / g_qpc_frequency
               : 0;
}

void AddEventLocked(EventType type, void* object, float value = 0.f,
                    float delta = 0.f, std::int32_t detail = 0,
                    std::uint32_t flags = 0, FKey key = {},
                    std::uint32_t input_event = 0) {
    if (g_event_count >= kMaxEvents) {
        ++g_dropped_count;
        return;
    }
    Event& event = g_events[g_event_count++];
    event.order = ++g_order;
    event.time_us = ElapsedMicroseconds();
    event.engine_tick = g_engine_tick.load(std::memory_order_relaxed);
    event.world_tick = g_world_tick.load(std::memory_order_relaxed);
    event.object = reinterpret_cast<std::uintptr_t>(object);
    event.thread_id = GetCurrentThreadId();
    event.type = type;
    event.key_index = key.comparison_index;
    event.key_number = key.number;
    event.input_event = input_event;
    event.flags = flags;
    event.detail = detail;
    event.value = value;
    event.delta = delta;
}

const char* EventName(EventType type) {
    switch (type) {
        case EventType::CaptureBegin: return "capture_begin";
        case EventType::CaptureEnd: return "capture_end";
        case EventType::EngineBegin: return "engine_begin";
        case EventType::EngineEnd: return "engine_end";
        case EventType::WorldBegin: return "world_begin";
        case EventType::WorldEnd: return "world_end";
        case EventType::ControllerKey: return "controller_key";
        case EventType::PlayerInputKey: return "playerinput_key";
        case EventType::PlayerInputAxis: return "playerinput_axis";
        case EventType::InputStackBegin: return "input_stack_begin";
        case EventType::InputStackEnd: return "input_stack_end";
    }
    return "unknown";
}

void FinishCapture() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_state.load(std::memory_order_relaxed) == State::Capturing) {
        AddEventLocked(EventType::CaptureEnd, nullptr);
        g_state.store(State::Complete, std::memory_order_release);
        g_dumping.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_dumping.load(std::memory_order_acquire)) return;

    HANDLE thread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SC_LOG("inputtrace: complete duration_ms=%llu events=%u dropped=%u player_input=%s",
               static_cast<unsigned long long>(GetTickCount64() - g_start_ms),
               g_event_count, g_dropped_count,
               g_player_input_available.load(std::memory_order_relaxed) ? "available"
                                                                         : "unavailable");
        for (std::uint32_t i = 0; i < g_event_count; ++i) {
            const Event& e = g_events[i];
            SC_LOG("inputtrace: event order=%llu us=%llu engine=%llu world=%llu tid=%lu "
                   "type=%s object=%p key=%u:%u input_event=%u value=%.4f delta=%.5f "
                   "detail=%d flags=%u",
                   static_cast<unsigned long long>(e.order),
                   static_cast<unsigned long long>(e.time_us),
                   static_cast<unsigned long long>(e.engine_tick),
                   static_cast<unsigned long long>(e.world_tick), e.thread_id,
                   EventName(e.type), reinterpret_cast<void*>(e.object), e.key_index,
                   e.key_number, e.input_event, e.value, e.delta, e.detail, e.flags);
        }
        g_dumping.store(false, std::memory_order_release);
        return 0;
    }, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        SC_LOG("inputtrace: could not start result writer thread (error=%lu)", GetLastError());
        g_dumping.store(false, std::memory_order_release);
    }
}

void StartCapture() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* controller = world ? ue::GetPlayerController(world, 0) : nullptr;
    if (!ue::IsAliveObject(world) || !ue::IsAliveObject(controller)) {
        g_state.store(State::Failed, std::memory_order_release);
        SC_LOG("inputtrace: capture refused; local world or player controller unavailable");
        return;
    }

    void* player_input = nullptr;
    const auto controller_address = reinterpret_cast<const std::uint8_t*>(controller);
    if (offsets::M_APlayerController_PlayerInput &&
        ue::RangeReadable(controller_address + offsets::M_APlayerController_PlayerInput,
                          sizeof(player_input))) {
        std::memcpy(&player_input,
                    controller_address + offsets::M_APlayerController_PlayerInput,
                    sizeof(player_input));
        if (!ue::IsAliveObject(static_cast<ue::UObject*>(player_input))) player_input = nullptr;
    }

    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    AcquireSRWLockExclusive(&g_lock);
    g_event_count = 0;
    g_dropped_count = 0;
    g_order = 0;
    g_engine_tick.store(0, std::memory_order_relaxed);
    g_world_tick.store(0, std::memory_order_relaxed);
    g_qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    g_start_qpc = QpcNow();
    g_start_ms = GetTickCount64();
    g_local_controller.store(controller, std::memory_order_release);
    g_local_player_input.store(player_input, std::memory_order_release);
    g_player_input_available.store(player_input != nullptr, std::memory_order_relaxed);
    g_state.store(State::Capturing, std::memory_order_release);
    AddEventLocked(EventType::CaptureBegin, controller,
                   player_input ? 1.f : 0.f, 0.f, 0,
                   player_input ? 1u : 0u);
    ReleaseSRWLockExclusive(&g_lock);

    char level[512] = {};
    ue::GetCurrentLevelPath(level, sizeof(level));
    SC_LOG("inputtrace: started duration_ms=%llu controller=%p player_input=%p level=%s",
           static_cast<unsigned long long>(kCaptureDurationMs), controller, player_input,
           level[0] ? level : "unavailable");
}

void Record(EventType type, void* object, float value = 0.f,
            float delta = 0.f, std::int32_t detail = 0,
            std::uint32_t flags = 0, FKey key = {},
            std::uint32_t input_event = 0) {
    if (g_state.load(std::memory_order_acquire) != State::Capturing) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_state.load(std::memory_order_relaxed) == State::Capturing)
        AddEventLocked(type, object, value, delta, detail, flags, key, input_event);
    ReleaseSRWLockExclusive(&g_lock);
}

void* LocalPlayerInput() {
    return g_local_player_input.load(std::memory_order_acquire);
}

bool __fastcall PlayerInputKeyHook(void* self, FKey key, std::uint32_t input_event,
                                   float amount, bool gamepad) {
    if (self == LocalPlayerInput())
        Record(EventType::PlayerInputKey, self, amount, 0.f, 0,
               gamepad ? 1u : 0u, key, input_event);
    return g_player_input_key ? g_player_input_key(self, key, input_event, amount, gamepad)
                              : false;
}

bool __fastcall PlayerInputAxisHook(void* self, FKey key, float value,
                                    float delta_seconds, int samples, bool gamepad) {
    if (self == LocalPlayerInput())
        Record(EventType::PlayerInputAxis, self, value, delta_seconds, samples,
               gamepad ? 1u : 0u, key);
    return g_player_input_axis
               ? g_player_input_axis(self, key, value, delta_seconds, samples, gamepad)
               : false;
}

std::int32_t InputComponentCount(const void* input_components) {
    if (!input_components ||
        !ue::RangeReadable(static_cast<const std::uint8_t*>(input_components), 16))
        return -1;
    std::int32_t count = -1;
    std::memcpy(&count, static_cast<const std::uint8_t*>(input_components) + sizeof(void*),
                sizeof(count));
    return count >= 0 && count <= 4096 ? count : -1;
}

void __fastcall ProcessInputStackHook(void* self, const void* input_components,
                                     float delta_seconds, bool paused) {
    const bool capture = g_state.load(std::memory_order_acquire) == State::Capturing &&
                         self == LocalPlayerInput();
    const std::int32_t count = capture ? InputComponentCount(input_components) : -1;
    if (capture)
        Record(EventType::InputStackBegin, self, delta_seconds, 0.f, count,
               paused ? 1u : 0u);
    if (g_process_input_stack)
        g_process_input_stack(self, input_components, delta_seconds, paused);
    if (capture)
        Record(EventType::InputStackEnd, self, delta_seconds, 0.f, count,
               paused ? 1u : 0u);
}

bool InstallOne(std::uint32_t rva, void* detour, void** original,
                void** target, const char* name, std::uintptr_t base) {
    if (!rva) {
        SC_LOG("inputtrace: %s offset unavailable", name);
        return false;
    }
    *target = reinterpret_cast<void*>(base + rva);
    MH_STATUS status = MH_CreateHook(*target, detour, original);
    if (status == MH_OK) status = MH_EnableHook(*target);
    if (status != MH_OK) {
        SC_LOG("inputtrace: %s hook failed (%d)", name, static_cast<int>(status));
        MH_RemoveHook(*target);
        *target = nullptr;
        *original = nullptr;
        return false;
    }
    SC_LOG("inputtrace: %s hook installed at %p", name, *target);
    return true;
}

}

void InstallHooks(std::uintptr_t module_base) {
    const bool key = InstallOne(offsets::UPlayerInput_InputKey,
                                reinterpret_cast<void*>(&PlayerInputKeyHook),
                                reinterpret_cast<void**>(&g_player_input_key),
                                &g_player_input_key_target, "UPlayerInput::InputKey",
                                module_base);
    const bool axis = InstallOne(offsets::UPlayerInput_InputAxis,
                                 reinterpret_cast<void*>(&PlayerInputAxisHook),
                                 reinterpret_cast<void**>(&g_player_input_axis),
                                 &g_player_input_axis_target, "UPlayerInput::InputAxis",
                                 module_base);
    const bool stack = InstallOne(offsets::UPlayerInput_ProcessInputStack,
                                  reinterpret_cast<void*>(&ProcessInputStackHook),
                                  reinterpret_cast<void**>(&g_process_input_stack),
                                  &g_process_input_stack_target,
                                  "UPlayerInput::ProcessInputStack", module_base);
    SC_LOG("inputtrace: hooks ready key=%u axis=%u stack=%u",
           key ? 1u : 0u, axis ? 1u : 0u, stack ? 1u : 0u);
}

void RemoveHooks() {
    void** targets[] = {&g_process_input_stack_target, &g_player_input_axis_target,
                        &g_player_input_key_target};
    for (void** target : targets) {
        if (!*target) continue;
        MH_DisableHook(*target);
        MH_RemoveHook(*target);
        *target = nullptr;
    }
    g_process_input_stack = nullptr;
    g_player_input_axis = nullptr;
    g_player_input_key = nullptr;
}

void QueueCapture() {
    if (g_dumping.load(std::memory_order_acquire)) return;
    State state = g_state.load(std::memory_order_acquire);
    if (state == State::Queued || state == State::Capturing) return;
    g_state.store(State::Queued, std::memory_order_release);
    g_capture_requested.store(true, std::memory_order_release);
}

Status GetStatus() {
    Status status = {};
    status.state = g_state.load(std::memory_order_acquire);
    status.player_input_available = g_player_input_available.load(std::memory_order_relaxed);
    status.writer_active = g_dumping.load(std::memory_order_acquire);
    AcquireSRWLockShared(&g_lock);
    status.event_count = g_event_count;
    status.dropped_count = g_dropped_count;
    ReleaseSRWLockShared(&g_lock);
    return status;
}

void EngineTickBegin(float delta_seconds, bool idle_mode) {
    g_engine_tick.fetch_add(1, std::memory_order_relaxed);
    if (g_capture_requested.exchange(false, std::memory_order_acq_rel)) {
        if (g_dumping.load(std::memory_order_acquire))
            g_capture_requested.store(true, std::memory_order_release);
        else
            StartCapture();
    }
    Record(EventType::EngineBegin, nullptr, delta_seconds, 0.f, idle_mode ? 1 : 0);
}

void EngineTickEnd() {
    Record(EventType::EngineEnd, nullptr);
    if (g_state.load(std::memory_order_acquire) == State::Capturing &&
        GetTickCount64() - g_start_ms >= kCaptureDurationMs)
        FinishCapture();
}

void WorldTickBegin(void* world, int tick_type, float delta_seconds, bool will_execute) {
    if (g_state.load(std::memory_order_acquire) != State::Capturing) return;
    if (world != ue::GetWorld()) return;
    g_world_tick.fetch_add(1, std::memory_order_relaxed);
    Record(EventType::WorldBegin, world, delta_seconds, 0.f, tick_type,
           will_execute ? 1u : 0u);
}

void WorldTickEnd(void* world, int tick_type, float delta_seconds, bool executed) {
    if (g_state.load(std::memory_order_acquire) != State::Capturing) return;
    if (world != ue::GetWorld()) return;
    Record(EventType::WorldEnd, world, delta_seconds, 0.f, tick_type,
           executed ? 1u : 0u);
}

void RecordControllerKey(void* controller, std::uint32_t key_index,
                         std::uint32_t key_number, std::uint32_t input_event,
                         float amount, bool gamepad) {
    if (controller != g_local_controller.load(std::memory_order_acquire)) return;
    const FKey key = {key_index, key_number};
    Record(EventType::ControllerKey, controller, amount, 0.f, 0,
           gamepad ? 1u : 0u, key, input_event);
}

}
