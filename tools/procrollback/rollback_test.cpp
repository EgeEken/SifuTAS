#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using HPSS = HANDLE;
using HPSSWALK = HANDLE;

struct PSS_VA_CLONE_INFORMATION { HANDLE VaCloneHandle; };

struct PSS_THREAD_ENTRY {
    DWORD ExitStatus;
    void* TebBaseAddress;
    DWORD ProcessId;
    DWORD ThreadId;
    ULONG_PTR AffinityMask;
    int Priority;
    int BasePriority;
    void* LastSyscallFirstArgument;
    WORD LastSyscallNumber;
    FILETIME CreateTime;
    FILETIME ExitTime;
    FILETIME KernelTime;
    FILETIME UserTime;
    void* Win32StartAddress;
    FILETIME CaptureTime;
    DWORD Flags;
    WORD SuspendCount;
    WORD SizeOfContextRecord;
    PCONTEXT ContextRecord;
};

using PssCaptureSnapshotFn = DWORD(WINAPI*)(HANDLE, DWORD, DWORD, HPSS*);
using PssQuerySnapshotFn = DWORD(WINAPI*)(HPSS, int, void*, DWORD);
using PssWalkMarkerCreateFn = DWORD(WINAPI*)(const void*, HPSSWALK*);
using PssWalkSnapshotFn = DWORD(WINAPI*)(HPSS, int, HPSSWALK, void*, DWORD);
using PssWalkMarkerFreeFn = DWORD(WINAPI*)(HPSSWALK);
using PssFreeSnapshotFn = DWORD(WINAPI*)(HANDLE, HPSS);

template <typename T> T ResolveProc(HMODULE module, const char* name) {
    const FARPROC proc = GetProcAddress(module, name);
    T result = nullptr;
    static_assert(sizeof(result) == sizeof(proc));
    std::memcpy(&result, &proc, sizeof(result));
    return result;
}

constexpr DWORD kPssCaptureVaClone = 0x00000001;
constexpr DWORD kPssCaptureThreads = 0x00000080;
constexpr DWORD kPssCaptureThreadContext = 0x00000100;
constexpr DWORD kPssCaptureThreadContextExtended = 0x00000200;
constexpr DWORD kPssCaptureVaSpace = 0x00000800;
constexpr int kPssQueryVaClone = 1;
constexpr int kPssWalkThreads = 3;
constexpr LONG kThreadCount = 3;
constexpr SIZE_T kSnapshotLimit = 128ull * 1024ull * 1024ull;
constexpr ULONGLONG kRunA = 120;
constexpr ULONGLONG kRunB = 220;
constexpr ULONGLONG kRunC = 310;

enum Command : LONG { Run = 0, Quit = 1 };

struct Control {
    volatile std::uint64_t state_address;
    volatile LONG ready_count;
    volatile LONG ack[kThreadCount];
    volatile LONG command;
    volatile LONG generation;
    volatile LONG run_to_low;
    volatile LONG run_to_high;
    volatile LONG tids[kThreadCount];
    volatile LONG tls_low[kThreadCount];
    volatile LONG tls_high[kThreadCount];
    volatile LONG child_pid;
    volatile LONG worker_exit[kThreadCount];
    volatile LONG uncovered_count;
    volatile LONG uncovered_tids[8];
};

struct Node {
    Node* next;
    std::uint64_t id;
    std::uint64_t value;
};

struct Payload {
    volatile LONG list_lock;
    std::uint64_t ticks[kThreadCount];
    std::uint64_t rng[kThreadCount];
    std::uint64_t timer[kThreadCount];
    std::uint64_t callback_count;
    std::uint64_t next_node_id;
    std::uint64_t allocation_count;
    std::uint64_t free_count;
    Node* head;
};

struct PayloadCounters {
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t nodes = 0;
    std::uint64_t next_node_id = 0;
};

thread_local std::uint64_t g_tls_count = 0;
Control* g_control = nullptr;
HANDLE g_heap = nullptr;
Payload* g_payload = nullptr;

struct Region {
    std::uintptr_t base = 0;
    SIZE_T size = 0;
    std::uintptr_t allocation_base = 0;
    DWORD protect = 0;
    DWORD state = 0;
    DWORD type = 0;
    std::vector<std::uint8_t> bytes;
};

struct SavedThread {
    DWORD tid = 0;
    std::uintptr_t teb = 0;
    std::vector<std::uint8_t> context;
};

struct Checkpoint {
    HPSS snapshot = nullptr;
    HANDLE clone = nullptr;
    std::vector<Region> regions;
    std::vector<SavedThread> threads;
    std::vector<DWORD> uncovered_tids;
    SIZE_T captured_bytes = 0;
    std::uint64_t capture_us = 0;
};

std::uint64_t NowMs() {
    return GetTickCount64();
}

std::uint64_t NowUs() {
    LARGE_INTEGER frequency = {}, counter = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart * 1000000 / frequency.QuadPart);
}

std::uint64_t LoadU64(volatile LONG* low, volatile LONG* high, int index) {
    const std::uint64_t lo = static_cast<std::uint32_t>(InterlockedCompareExchange(&low[index], 0, 0));
    const std::uint64_t hi = static_cast<std::uint32_t>(InterlockedCompareExchange(&high[index], 0, 0));
    return lo | (hi << 32);
}

void StoreU64(volatile LONG* low, volatile LONG* high, int index, std::uint64_t value) {
    InterlockedExchange(&low[index], static_cast<LONG>(value));
    InterlockedExchange(&high[index], static_cast<LONG>(value >> 32));
}

std::uint64_t RunTo() {
    return LoadU64(&g_control->run_to_low, &g_control->run_to_high, 0);
}

void LockList() {
    while (InterlockedCompareExchange(&g_payload->list_lock, 1, 0) != 0) YieldProcessor();
}

void UnlockList() {
    InterlockedExchange(&g_payload->list_lock, 0);
}

std::uint64_t NextRandom(std::uint64_t value) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return value;
}

void SimulateStep(int index) {
    ++g_tls_count;
    ++g_payload->ticks[index];
    g_payload->rng[index] = NextRandom(g_payload->rng[index]);
    if (g_payload->timer[index]) --g_payload->timer[index];
    if ((g_payload->ticks[index] % 9) == 0) {
        ++g_payload->callback_count;
        g_payload->timer[index] = (g_payload->rng[index] % 71) + 11;
    }
    if ((g_payload->ticks[index] % 5) == 0) {
        LockList();
        Node* node = static_cast<Node*>(HeapAlloc(g_heap, HEAP_ZERO_MEMORY, sizeof(Node)));
        if (node) {
            node->id = ++g_payload->next_node_id;
            ++g_payload->allocation_count;
            node->value = g_payload->rng[index] ^ g_payload->ticks[index];
            node->next = g_payload->head;
            g_payload->head = node;
        }
        Node* cursor = g_payload->head;
        Node* previous = nullptr;
        int count = 0;
        while (cursor && count++ < 48) {
            previous = cursor;
            cursor = cursor->next;
        }
        if (previous && cursor) {
            previous->next = nullptr;
            while (cursor) {
                Node* next = cursor->next;
                if (HeapFree(g_heap, 0, cursor)) ++g_payload->free_count;
                cursor = next;
            }
        }
        UnlockList();
    }
}

DWORD WINAPI Worker(void* argument) {
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(argument));
    InterlockedExchange(&g_control->tids[index], static_cast<LONG>(GetCurrentThreadId()));
    InterlockedExchangeAdd(&g_control->ready_count, 1);
    for (;;) {
        if (InterlockedCompareExchange(&g_control->command, 0, 0) == Quit) break;
        const LONG generation = InterlockedCompareExchange(&g_control->generation, 0, 0);
        const std::uint64_t target = RunTo();
        if (g_payload->ticks[index] >= target) {
            StoreU64(&g_control->tls_low[index], &g_control->tls_high[index], index, g_tls_count);
            InterlockedExchange(&g_control->ack[index], generation);
            while (InterlockedCompareExchange(&g_control->command, 0, 0) != Quit &&
                   InterlockedCompareExchange(&g_control->generation, 0, 0) == generation)
                YieldProcessor();
            continue;
        }
        SimulateStep(index);
    }
    InterlockedExchange(&g_control->worker_exit[index], 1);
    return 0;
}

int ChildMain(const wchar_t* mapping_name) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
    if (!mapping) return 20;
    g_control = static_cast<Control*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                     sizeof(Control)));
    if (!g_control) { CloseHandle(mapping); return 21; }
    g_heap = HeapCreate(0, 8 * 1024 * 1024, 8 * 1024 * 1024);
    if (!g_heap) return 22;
    g_payload = static_cast<Payload*>(HeapAlloc(g_heap, HEAP_ZERO_MEMORY, sizeof(Payload)));
    if (!g_payload) return 23;
    for (int i = 0; i < kThreadCount; ++i) {
        g_payload->rng[i] = 0x9E3779B97F4A7C15ull ^ (0x100000001B3ull * (i + 1));
        g_payload->timer[i] = 47 + i;
    }
    g_control->state_address = reinterpret_cast<std::uintptr_t>(g_payload);
    MEMORY_BASIC_INFORMATION payload_mbi = {};
    VirtualQuery(g_payload, &payload_mbi, sizeof(payload_mbi));
    InterlockedExchange(&g_control->child_pid, static_cast<LONG>(GetCurrentProcessId()));
    for (int i = 1; i < kThreadCount; ++i) {
        HANDLE thread = CreateThread(nullptr, 0, Worker,
                                     reinterpret_cast<void*>(static_cast<std::intptr_t>(i)), 0, nullptr);
        if (!thread) return 24;
        CloseHandle(thread);
    }
    Worker(reinterpret_cast<void*>(0));
    while (InterlockedCompareExchange(&g_control->worker_exit[1], 0, 0) == 0 ||
           InterlockedCompareExchange(&g_control->worker_exit[2], 0, 0) == 0) Sleep(1);
    UnmapViewOfFile(g_control);
    CloseHandle(mapping);
    HeapDestroy(g_heap);
    return 0;
}

bool ResolvePss(PssCaptureSnapshotFn* capture, PssQuerySnapshotFn* query,
                PssWalkMarkerCreateFn* marker_create, PssWalkSnapshotFn* walk,
                PssWalkMarkerFreeFn* marker_free, PssFreeSnapshotFn* free_snapshot) {
    HMODULE module = GetModuleHandleW(L"kernel32.dll");
    if (!module) return false;
    *capture = ResolveProc<PssCaptureSnapshotFn>(module, "PssCaptureSnapshot");
    *query = ResolveProc<PssQuerySnapshotFn>(module, "PssQuerySnapshot");
    *marker_create = ResolveProc<PssWalkMarkerCreateFn>(module, "PssWalkMarkerCreate");
    *walk = ResolveProc<PssWalkSnapshotFn>(module, "PssWalkSnapshot");
    *marker_free = ResolveProc<PssWalkMarkerFreeFn>(module, "PssWalkMarkerFree");
    *free_snapshot = ResolveProc<PssFreeSnapshotFn>(module, "PssFreeSnapshot");
    return *capture && *query && *marker_create && *walk && *marker_free && *free_snapshot;
}

bool WaitReady(Control* control, DWORD timeout_ms) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeout_ms) {
        if (InterlockedCompareExchange(&control->ready_count, 0, 0) == kThreadCount) return true;
        Sleep(1);
    }
    return false;
}

bool WaitParked(Control* control, LONG generation, DWORD timeout_ms) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < timeout_ms) {
        bool parked = true;
        for (int i = 0; i < kThreadCount; ++i)
            parked = parked && InterlockedCompareExchange(&control->ack[i], 0, 0) == generation;
        if (parked) return true;
        Sleep(1);
    }
    return false;
}

LONG SetRunTo(Control* control, std::uint64_t value) {
    StoreU64(&control->run_to_low, &control->run_to_high, 0, value);
    return InterlockedIncrement(&control->generation);
}

bool AuditTargetThreads(DWORD pid, Control* control, std::vector<DWORD>* uncovered,
                        std::string* error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { *error = "thread snapshot failed"; return false; }
    bool found[kThreadCount] = {};
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    bool ok = Thread32First(snapshot, &entry) != FALSE;
    while (ok) {
        if (entry.th32OwnerProcessID == pid) {
            int registered = -1;
            for (int i = 0; i < kThreadCount; ++i)
                if (static_cast<DWORD>(InterlockedCompareExchange(&control->tids[i], 0, 0)) ==
                    entry.th32ThreadID) registered = i;
            if (registered >= 0) found[registered] = true;
            else uncovered->push_back(entry.th32ThreadID);
        }
        ok = Thread32Next(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    if (!found[0] || !found[1] || !found[2]) {
        *error = "a cooperative simulation thread disappeared";
        return false;
    }
    return true;
}

std::string ModuleForAddress(DWORD pid, std::uintptr_t address) {
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (modules == INVALID_HANDLE_VALUE) return "<unknown>";
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    std::string result = "<unknown>";
    if (Module32FirstW(modules, &module)) {
        do {
            const auto base = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            if (address >= base && address - base < module.modBaseSize) {
                char name[MAX_MODULE_NAME32 + 1] = {};
                WideCharToMultiByte(CP_UTF8, 0, module.szModule, -1, name, sizeof(name), nullptr, nullptr);
                result = name;
                break;
            }
        } while (Module32NextW(modules, &module));
    }
    CloseHandle(modules);
    return result;
}

bool Capture(Checkpoint* checkpoint, HANDLE process, DWORD pid,
             Control* control, std::string* error) {
    PssCaptureSnapshotFn pss_capture = nullptr;
    PssQuerySnapshotFn pss_query = nullptr;
    PssWalkMarkerCreateFn marker_create = nullptr;
    PssWalkSnapshotFn pss_walk = nullptr;
    PssWalkMarkerFreeFn marker_free = nullptr;
    PssFreeSnapshotFn pss_free = nullptr;
    if (!ResolvePss(&pss_capture, &pss_query, &marker_create, &pss_walk,
                    &marker_free, &pss_free)) {
        *error = "PSS APIs are unavailable in kernel32.dll";
        return false;
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) { *error = "GlobalMemoryStatusEx failed"; return false; }
    const ULONGLONG budget = std::min<ULONGLONG>(kSnapshotLimit,
        std::min(memory.ullAvailPhys / 4, memory.ullAvailPageFile / 8));
    if (budget == 0) { *error = "insufficient physical-memory or commit headroom"; return false; }

    InterlockedExchange(&control->uncovered_count, 0);
    if (!AuditTargetThreads(pid, control, &checkpoint->uncovered_tids, error)) return false;
    const std::uint64_t started = NowUs();
    const DWORD flags = kPssCaptureVaClone | kPssCaptureThreads |
                        kPssCaptureThreadContext | kPssCaptureThreadContextExtended |
                        kPssCaptureVaSpace;
    DWORD status = pss_capture(process, flags, CONTEXT_ALL, &checkpoint->snapshot);
    if (status != ERROR_SUCCESS) {
        *error = "PssCaptureSnapshot failed with " + std::to_string(status);
        return false;
    }
    PSS_VA_CLONE_INFORMATION clone = {};
    status = pss_query(checkpoint->snapshot, kPssQueryVaClone, &clone, sizeof(clone));
    if (status != ERROR_SUCCESS || !clone.VaCloneHandle) {
        pss_free(process, checkpoint->snapshot);
        checkpoint->snapshot = nullptr;
        *error = "PssQuerySnapshot(VA clone) failed with " + std::to_string(status);
        return false;
    }
    checkpoint->clone = clone.VaCloneHandle;

    HPSSWALK marker = nullptr;
    status = marker_create(nullptr, &marker);
    if (status != ERROR_SUCCESS) {
        *error = "PssWalkMarkerCreate failed with " + std::to_string(status);
        return false;
    }
    PSS_THREAD_ENTRY entry = {};
    while ((status = pss_walk(checkpoint->snapshot, kPssWalkThreads, marker,
                              &entry, sizeof(entry))) == ERROR_SUCCESS) {
        if (!entry.ContextRecord || entry.SizeOfContextRecord < sizeof(CONTEXT)) continue;
        SavedThread saved = {};
        saved.tid = entry.ThreadId;
        saved.teb = reinterpret_cast<std::uintptr_t>(entry.TebBaseAddress);
        saved.context.resize(entry.SizeOfContextRecord);
        std::memcpy(saved.context.data(), entry.ContextRecord, entry.SizeOfContextRecord);
        bool registered = false;
        for (int i = 0; i < kThreadCount; ++i)
            registered = registered || static_cast<DWORD>(
                InterlockedCompareExchange(&control->tids[i], 0, 0)) == entry.ThreadId;
        const auto start = reinterpret_cast<std::uintptr_t>(entry.Win32StartAddress);
        std::printf("snapshot-thread: tid=%lu start=%p module=%s registered=%s context_bytes=%zu\n",
                    saved.tid, entry.Win32StartAddress, ModuleForAddress(pid, start).c_str(),
                    registered ? "yes" : "no",
                    saved.context.size());
        checkpoint->threads.push_back(std::move(saved));
    }
    marker_free(marker);
    if (status != ERROR_NO_MORE_ITEMS || checkpoint->threads.size() < kThreadCount) {
        *error = "PSS thread walk failed or omitted a registered simulation thread";
        return false;
    }
    for (const SavedThread& thread : checkpoint->threads) {
        bool registered = false;
        for (int i = 0; i < kThreadCount; ++i)
            registered = registered || thread.tid == static_cast<DWORD>(
                InterlockedCompareExchange(&control->tids[i], 0, 0));
        if (!registered && std::find(checkpoint->uncovered_tids.begin(),
                                     checkpoint->uncovered_tids.end(), thread.tid) ==
                               checkpoint->uncovered_tids.end())
            checkpoint->uncovered_tids.push_back(thread.tid);
    }
    const LONG uncovered_count = InterlockedCompareExchange(&control->uncovered_count, 0, 0);
    for (LONG i = 0; i < uncovered_count && i < 8; ++i)
        checkpoint->uncovered_tids.push_back(static_cast<DWORD>(
            InterlockedCompareExchange(&control->uncovered_tids[i], 0, 0)));

    SYSTEM_INFO system = {};
    GetNativeSystemInfo(&system);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQueryEx(checkpoint->clone, reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
            break;
        if (mbi.RegionSize == 0) break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const bool private_commit = mbi.Type == MEM_PRIVATE && mbi.State == MEM_COMMIT;
        const bool protected_page = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
        if (private_commit && !protected_page) {
            if (checkpoint->captured_bytes + mbi.RegionSize > budget) {
                *error = "private committed memory exceeds the bounded snapshot budget";
                return false;
            }
            Region region = {};
            region.base = base;
            region.size = mbi.RegionSize;
            region.allocation_base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
            region.protect = mbi.Protect;
            region.state = mbi.State;
            region.type = mbi.Type;
            region.bytes.resize(mbi.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(checkpoint->clone, mbi.BaseAddress, region.bytes.data(),
                                   region.size, &read) || read != region.size) {
                *error = "failed to read a committed private clone region at " +
                         std::to_string(base);
                return false;
            }
            checkpoint->captured_bytes += region.size;
            checkpoint->regions.push_back(std::move(region));
        }
        address = base + mbi.RegionSize;
    }
    checkpoint->capture_us = NowUs() - started;
    return true;
}

bool RestoreGate(const Checkpoint& checkpoint, std::string* error) {
    if (!checkpoint.uncovered_tids.empty() || checkpoint.threads.size() != kThreadCount) {
        *error = "restore refused: unregistered process thread(s) lack cooperative checkpoint points";
        return false;
    }
    *error = "restore refused: this harness only validates the safe capture gate";
    return false;
}

std::uint64_t RemoteHash(HANDLE process, Control* control,
                         PayloadCounters* counters = nullptr) {
    const std::uintptr_t address = static_cast<std::uintptr_t>(control->state_address);
    Payload payload = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), &payload, sizeof(payload), &read) ||
        read != sizeof(payload)) {
        MEMORY_BASIC_INFORMATION mbi = {};
        const SIZE_T queried = VirtualQueryEx(process, reinterpret_cast<void*>(address), &mbi, sizeof(mbi));
        std::printf("remote-payload-read-failed: address=%p read=%llu error=%lu\n",
                    reinterpret_cast<void*>(address), static_cast<unsigned long long>(read),
                    GetLastError());
        std::printf("remote-payload-region: query=%llu base=%p size=%llu state=%lx type=%lx protect=%lx\n",
                    static_cast<unsigned long long>(queried), mbi.BaseAddress,
                    static_cast<unsigned long long>(mbi.RegionSize), mbi.State, mbi.Type, mbi.Protect);
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) { hash ^= static_cast<std::uint8_t>(value >> (i * 8)); hash *= 1099511628211ull; }
    };
    for (int i = 0; i < kThreadCount; ++i) {
        mix(payload.ticks[i]); mix(payload.rng[i]); mix(payload.timer[i]);
    }
    mix(payload.callback_count); mix(payload.next_node_id);
    mix(payload.allocation_count); mix(payload.free_count);
    PayloadCounters result = {};
    result.allocations = payload.allocation_count;
    result.frees = payload.free_count;
    result.next_node_id = payload.next_node_id;
    std::uintptr_t node_address = reinterpret_cast<std::uintptr_t>(payload.head);
    for (int count = 0; node_address && count < 64; ++count) {
        Node node = {};
        if (!ReadProcessMemory(process, reinterpret_cast<void*>(node_address), &node,
                               sizeof(node), &read) || read != sizeof(node)) return 0;
        mix(node.id); mix(node.value); mix(node_address);
        ++result.nodes;
        node_address = reinterpret_cast<std::uintptr_t>(node.next);
    }
    if (counters) *counters = result;
    return hash;
}

std::vector<std::uint64_t> ReadTls(Control* control) {
    std::vector<std::uint64_t> result(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i)
        result[i] = LoadU64(&control->tls_low[i], &control->tls_high[i], i);
    return result;
}

bool WaitProcess(Control* control) {
    if (!WaitReady(control, 15000)) return false;
    const LONG generation = SetRunTo(control, kRunA);
    return WaitParked(control, generation, 30000);
}

int ControllerMain() {
    wchar_t mapping_name[96] = {};
    swprintf_s(mapping_name, L"Local\\SifuTAS-ProcessRollback-%lu-%llu",
               GetCurrentProcessId(), GetTickCount64());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0, sizeof(Control), mapping_name);
    if (!mapping) { std::printf("FAIL: CreateFileMapping error=%lu\n", GetLastError()); return 2; }
    Control* control = static_cast<Control*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                            sizeof(Control)));
    if (!control) { CloseHandle(mapping); return 3; }
    std::memset(control, 0, sizeof(*control));

    wchar_t executable[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 4;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + mapping_name + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        std::printf("FAIL: CreateProcess error=%lu\n", GetLastError());
        UnmapViewOfFile(control); CloseHandle(mapping); return 5;
    }
    CloseHandle(info.hThread);
    bool success = false;
    std::string error;
    Checkpoint checkpoint = {};
    std::vector<std::uint64_t> tls_a;
    std::uint64_t pre_capture_hash = 0;
    std::uint64_t hash_a_clone = 0;
    std::uint64_t hash_a_live = 0;
    std::uint64_t hash_c_live = 0;
    PayloadCounters stats_a = {};
    PayloadCounters stats_b = {};
    PayloadCounters stats_c = {};
    HANDLE memory_process = nullptr;
    std::uintptr_t payload_address = 0;
    bool clone_has_payload_region = false;
    MEMORY_BASIC_INFORMATION payload_live_mbi = {};
    SIZE_T payload_query = 0;
    LONG branch_generation = 0;
    const std::uint64_t total_started = NowMs();
    if (!WaitProcess(control)) {
        error = "target did not reach initial cooperative checkpoint";
        goto cleanup;
    }
    tls_a = ReadTls(control);
    memory_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, info.dwProcessId);
    pre_capture_hash = memory_process ? RemoteHash(memory_process, control) : 0;
    std::printf("pre-capture-remote-hash=%016llX handle_pid=%lu shared_pid=%lu\n",
                static_cast<unsigned long long>(pre_capture_hash), GetProcessId(info.hProcess),
                static_cast<DWORD>(InterlockedCompareExchange(&control->child_pid, 0, 0)));
    if (memory_process) CloseHandle(memory_process);
    if (!Capture(&checkpoint, info.hProcess, info.dwProcessId, control, &error)) goto cleanup;
    hash_a_clone = RemoteHash(checkpoint.clone, control, &stats_a);
    hash_a_live = RemoteHash(info.hProcess, control);
    payload_address = static_cast<std::uintptr_t>(control->state_address);
    payload_query = VirtualQueryEx(info.hProcess,
        reinterpret_cast<void*>(payload_address), &payload_live_mbi, sizeof(payload_live_mbi));
    clone_has_payload_region = std::any_of(checkpoint.regions.begin(), checkpoint.regions.end(),
        [payload_address](const Region& region) {
            return payload_address >= region.base && payload_address - region.base < region.size;
        });
    std::printf("capture: pid=%lu handle_pid=%lu state=%p payload_query=%llu payload_state=%lx payload_type=%lx payload_region=%llu private_bytes=%llu regions=%zu threads=%zu uncovered_threads=%zu capture_us=%llu\n",
                static_cast<DWORD>(InterlockedCompareExchange(&control->child_pid, 0, 0)),
                GetProcessId(info.hProcess),
                reinterpret_cast<void*>(control->state_address),
                static_cast<unsigned long long>(payload_query), payload_live_mbi.State,
                payload_live_mbi.Type,
                static_cast<unsigned long long>(payload_live_mbi.RegionSize),
                static_cast<unsigned long long>(checkpoint.captured_bytes),
                checkpoint.regions.size(), checkpoint.threads.size(),
                checkpoint.uncovered_tids.size(),
                static_cast<unsigned long long>(checkpoint.capture_us));
    std::printf("capture-content: clone_has_payload=%s clone_hash=%016llX live_hash=%016llX match=%s A_tls=%llu,%llu,%llu A_allocs=%llu A_frees=%llu A_nodes=%llu\n",
                clone_has_payload_region ? "yes" : "no",
                static_cast<unsigned long long>(hash_a_clone),
                static_cast<unsigned long long>(hash_a_live),
                hash_a_clone == hash_a_live ? "yes" : "no",
                static_cast<unsigned long long>(tls_a[0]),
                static_cast<unsigned long long>(tls_a[1]),
                static_cast<unsigned long long>(tls_a[2]),
                static_cast<unsigned long long>(stats_a.allocations),
                static_cast<unsigned long long>(stats_a.frees),
                static_cast<unsigned long long>(stats_a.nodes));

    branch_generation = SetRunTo(control, kRunB);
    if (!WaitParked(control, branch_generation, 30000)) {
        error = "target did not complete branch B";
        goto cleanup;
    }
    {
        const auto tls_b = ReadTls(control);
        const std::uint64_t hash_b = RemoteHash(info.hProcess, control, &stats_b);
        const std::uint64_t hash_b_clone = RemoteHash(checkpoint.clone, control);
        std::printf("branch-B: live_hash=%016llX clone_hash=%016llX ticks=%llu,%llu,%llu tls=%llu,%llu,%llu allocs=%llu frees=%llu nodes=%llu\n",
                    static_cast<unsigned long long>(hash_b),
                    static_cast<unsigned long long>(hash_b_clone),
                    static_cast<unsigned long long>(LoadU64(&control->tls_low[0], &control->tls_high[0], 0)),
                    static_cast<unsigned long long>(LoadU64(&control->tls_low[1], &control->tls_high[1], 1)),
                    static_cast<unsigned long long>(LoadU64(&control->tls_low[2], &control->tls_high[2], 2)),
                    static_cast<unsigned long long>(tls_b[0]),
                    static_cast<unsigned long long>(tls_b[1]),
                    static_cast<unsigned long long>(tls_b[2]),
                    static_cast<unsigned long long>(stats_b.allocations),
                    static_cast<unsigned long long>(stats_b.frees),
                    static_cast<unsigned long long>(stats_b.nodes));
        std::string refusal;
        const bool gate_open = RestoreGate(checkpoint, &refusal);
        std::printf("restore-gate: open=%s reason=%s\n",
                    gate_open ? "yes" : "no", refusal.c_str());
        success = pre_capture_hash == hash_a_clone && hash_a_clone == hash_a_live &&
                  hash_b != hash_a_clone &&
                  hash_b_clone == hash_a_clone && stats_a.allocations == 72 &&
                  stats_a.frees == 24 && stats_a.nodes == 48 && !gate_open &&
                  refusal.find("unregistered process thread") != std::string::npos;
        if (!success) error = "snapshot preservation or fail-closed thread gate did not match expectations";
    }
    branch_generation = SetRunTo(control, kRunC);
    if (!WaitParked(control, branch_generation, 30000)) {
        error = "target did not complete branch C";
        success = false;
        goto cleanup;
    }
    hash_c_live = RemoteHash(info.hProcess, control, &stats_c);
    std::printf("branch-C: live_hash=%016llX checkpoint_A=%016llX ticks=%llu,%llu,%llu allocs=%llu frees=%llu nodes=%llu next_node_id=%llu\n",
                static_cast<unsigned long long>(hash_c_live),
                static_cast<unsigned long long>(hash_a_clone),
                static_cast<unsigned long long>(LoadU64(&control->tls_low[0], &control->tls_high[0], 0)),
                static_cast<unsigned long long>(LoadU64(&control->tls_low[1], &control->tls_high[1], 1)),
                static_cast<unsigned long long>(LoadU64(&control->tls_low[2], &control->tls_high[2], 2)),
                static_cast<unsigned long long>(stats_c.allocations),
                static_cast<unsigned long long>(stats_c.frees),
                static_cast<unsigned long long>(stats_c.nodes),
                static_cast<unsigned long long>(stats_c.next_node_id));
    success = success && hash_c_live != hash_a_clone && stats_c.allocations > stats_b.allocations &&
              stats_c.frees > stats_b.frees && stats_c.nodes == 48;

cleanup:
    if (checkpoint.snapshot) {
        PssFreeSnapshotFn pss_free = ResolveProc<PssFreeSnapshotFn>(
            GetModuleHandleW(L"kernel32.dll"), "PssFreeSnapshot");
        if (pss_free) pss_free(info.hProcess, checkpoint.snapshot);
    }
    if (info.hProcess && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
        InterlockedExchange(&control->command, Quit);
        SetRunTo(control, 0);
        if (WaitForSingleObject(info.hProcess, 5000) == WAIT_TIMEOUT) {
            std::printf("target did not exit cooperatively; test process left running\n");
            success = false;
        }
    }
    if (!error.empty()) std::printf("FAIL: %s\n", error.c_str());
    std::printf("result: %s; rollback=BLOCKED total_ms=%llu\n",
                success ? "capture-and-refusal PASS" : "FAIL",
                static_cast<unsigned long long>(NowMs() - total_started));
    CloseHandle(info.hProcess);
    UnmapViewOfFile(control);
    CloseHandle(mapping);
    return success ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wcscmp(argv[1], L"--child") == 0) return ChildMain(argv[2]);
    return ControllerMain();
}
