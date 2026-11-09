#include <windows.h>

#include <cstdio>

#include "core/hooks.h"
#include "core/log.h"
#include "game/actors.h"
#include "core/offsets.g.h"
#include "tas/controls.h"
#include "ui/overlay.h"
#include "ue/reflection.h"

namespace sifucoop::proxy {
bool Init();
}

namespace offsets = sifucoop::offsets;

namespace {

HANDLE g_game_process_mutex = nullptr;

// The root executable can be a short-lived launcher shim that starts
// a short-lived first game process that loads this dll, holds the guard for a
// few seconds, relaunches the real game and exits. Measured live 2026-09-14:
// the real process attached 3.4 s later, found the guard taken, and stayed
// inert. So a taken guard is retried for a while before it is believed.
bool AcquireGameProcessGuard() {
    constexpr DWORD kRetryForMs = 20000;
    constexpr DWORD kRetryEveryMs = 500;
    const DWORD started = GetTickCount();
    bool logged_wait = false;
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        HANDLE mutex = CreateMutexA(nullptr, FALSE, "Local\\SifuTAS.GameProcessGuard");
        const DWORD error = GetLastError();
        if (!mutex) {
            char message[192] = {};
            _snprintf(message, sizeof(message),
                      "SifuTAS could not create its process guard (Windows error %lu). "
                      "The mod will stay inactive to avoid unsafe duplicate hooks.",
                      error);
            SC_LOG("guard: process mutex creation failed (%lu) -- refusing to hook", error);
            MessageBoxA(nullptr, message, "SifuTAS not started",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return false;
        }
        if (error != ERROR_ALREADY_EXISTS) {
            g_game_process_mutex = mutex;
            SC_LOG("guard: acquired process-lifetime instance guard%s",
                   logged_wait ? " (the earlier process has exited)" : "");
            return true;
        }
        CloseHandle(mutex);
        if (!logged_wait) {
            logged_wait = true;
            SC_LOG("guard: another SifuTAS game process holds the guard -- waiting up to "
                   "%lus in case it is the launcher shim handing over",
                   kRetryForMs / 1000);
        }
        if (GetTickCount() - started >= kRetryForMs) break;
        Sleep(kRetryEveryMs);
    }

    SC_LOG("guard: another SifuTAS game process is still active -- refusing to hook");
    MessageBoxA(nullptr,
                "Another SifuTAS-enabled Sifu process is already running. Close it "
                "before starting another copy. This game will continue without the mod.",
                "SifuTAS already running",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    return false;
}

struct PeIdentity {
    DWORD time_date_stamp;
    DWORD size_of_image;
};

bool ReadPeIdentity(HMODULE module, PeIdentity* out) {
    auto* base = reinterpret_cast<const BYTE*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    out->time_date_stamp = nt->FileHeader.TimeDateStamp;
    out->size_of_image = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool VerifyGameBuild(HMODULE game, uintptr_t base) {
    PeIdentity id = {};
    if (!ReadPeIdentity(game, &id)) {
        SC_LOG("guard: could not parse exe headers -- refusing to hook");
        return false;
    }

    SC_LOG("guard: exe TimeDateStamp=0x%08lX SizeOfImage=0x%08lX", id.time_date_stamp,
           id.size_of_image);

    const char* build = offsets::SelectBuild(id.time_date_stamp, id.size_of_image);
    if (!build) {
        SC_LOG("guard: UNKNOWN BUILD -- no offsets for this executable.");
        SC_LOG("guard: known builds (%d):", offsets::kBuildCount);
        for (const auto& entry : offsets::kBuilds) {
            SC_LOG("guard:   %-12s stamp=0x%08X size=0x%08X", entry.name,
                   entry.time_date_stamp, entry.size_of_image);
        }
        SC_LOG("guard: to add this one, run on THIS machine:");
        SC_LOG("guard:   python pdbdump.py <Sifu-Win64-Shipping.pdb> "
               "--exe <Sifu-Win64-Shipping.exe> --emit-build <name> build.json");
        char message[512] = {};
        wsprintfA(message,
                  "SifuTAS did not start because this Sifu executable is not in its "
                  "supported-build table.\n\nBuild stamp: 0x%08lX\nImage size: 0x%08lX"
                  "\n\nThe game will continue normally. Rebuild SifuTAS for this executable.",
                  id.time_date_stamp, id.size_of_image);
        MessageBoxA(nullptr, message, "SifuTAS needs an update",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);        return false;
    }

    SC_LOG("guard: build '%s' matched. image base = 0x%llX", build,
           static_cast<unsigned long long>(base));

    for (const auto& entry : offsets::kBuilds) {
        if (_stricmp(entry.name, build) != 0) continue;
        int missing = 0;
        for (std::uint32_t value : entry.values) {
            if (value == 0) ++missing;
        }
        if (missing > 0) {
            SC_LOG("guard: %d of %d offsets are MISSING from the '%s' table -- regenerate it "
                   "on this machine (build.ps1 -GameDir <this game folder> -LocalBuildName %s) "
                   "or those features will silently do nothing here",
                   missing, static_cast<int>(sizeof(entry.values) / sizeof(entry.values[0])),
                   build, build);
        }
        break;
    }

    // Without these the mod cannot hook, reflect or track anything, and a zero
    // offset resolves to the PE header. Refuse rather than execute that.
    struct Required {
        const char* name;
        std::uint32_t value;
    };
    const Required required[] = {
        {"UGameEngine::Tick", offsets::UGameEngine_Tick},
        {"UWorld::Tick", offsets::UWorld_Tick},
        {"AWorldSettings::GetEffectiveTimeDilation", offsets::AWorldSettings_GetEffectiveTimeDilation},
        {"APlayerController::InputKey", offsets::APlayerController_InputKey},
        {"GEngine", offsets::GEngine},
        {"GWorld", offsets::GWorld},
        {"FName ctor", offsets::FName_FromWide},
        {"UObject::FindFunction", offsets::UObject_FindFunction},
        {"UObject::ProcessEvent", offsets::UObject_ProcessEvent},
        {"UGameplayStatics::GetPlayerCharacter", offsets::UGameplayStatics_GetPlayerCharacter},
        {"UObjectBaseUtility::GetPathName", offsets::UObjectBaseUtility_GetPathName},
        {"FMemory::Free", offsets::FMemory_Free},
        {"StaticFindObjectSafe", offsets::StaticFindObjectSafe},
        {"UCharacterHealthComponent::Get", offsets::UCharacterHealthComponent_Get},
        {"UCharacterHealthComponent::IsDown", offsets::UCharacterHealthComponent_IsDown},
        {"UHealthComponent::IsDead", offsets::UHealthComponent_IsDead},
        {"UWGGameplayStatics::IsGameOver", offsets::UWGGameplayStatics_BPF_IsGameOver},
        {"UHealthComponent::m_fHealth", offsets::M_UHealthComponent_fHealth},
        {"UHealthComponent::m_fMaxHealth", offsets::M_UHealthComponent_fMaxHealth},
        {"UWorld::IsInSeamlessTravel", offsets::UWorld_IsInSeamlessTravel},
        {"UWorld::OwningGameInstance", offsets::M_UWorld_OwningGameInstance},
        {"GameInstance::loadingScreen", offsets::M_UThePlainesGameInstance_loadingScreen},
        {"GameState::MatchState", offsets::M_AGameState_MatchState},
    };
    int required_missing = 0;
    for (const Required& item : required) {
        if (item.value != 0) continue;
        ++required_missing;
        SC_LOG("guard: REQUIRED offset '%s' is missing from the '%s' table", item.name, build);
    }
    if (required_missing > 0) {
        char message[512] = {};
        wsprintfA(message,
                  "SifuTAS did not start: %d required offset(s) are missing from its '%s' "
                  "table. Rebuild the mod against this game folder before playing.\n\n"
                  "The game will continue normally without the mod.",
                  required_missing, build);
        MessageBoxA(nullptr, message, "SifuTAS needs a rebuild",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return false;
    }
    return true;
}

#ifndef SIFUTAS_COMMIT
#define SIFUTAS_COMMIT "unknown"
#endif

DWORD WINAPI Bootstrap(LPVOID) {
    SC_LOG("build: SifuTAS commit %s, compiled %s %s", SIFUTAS_COMMIT, __DATE__, __TIME__);
    if (!AcquireGameProcessGuard()) return 0;

    HMODULE game = GetModuleHandleA(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(game);

    if (!VerifyGameBuild(game, base)) {
        SC_LOG("bootstrap: aborted, mod is inert (the game will run normally)");
        return 0;
    }

    auto** gengine = reinterpret_cast<void**>(base + offsets::GEngine);
    void* engine = nullptr;
    for (int i = 0; i < 600; ++i) {
        engine = *gengine;
        if (engine) {
            SC_LOG("bootstrap: GEngine populated after ~%d.%ds -> 0x%llX", i / 2, (i % 2) * 5,
                   reinterpret_cast<unsigned long long>(engine));
            break;
        }
        Sleep(500);
    }

    if (!engine) {
        SC_LOG("bootstrap: GEngine still null after 5 minutes -- offset is suspect");
        return 0;
    }

    SC_LOG("bootstrap: engine present; waiting for startup asset loading to settle");
    Sleep(8000);

    if (!sifucoop::ue::InitReflection(base)) {
        SC_LOG("bootstrap: reflection unavailable -- stopping before hooking");
        return 0;
    }

    sifucoop::tas::LoadSettings();
    sifucoop::game::InitActors(base);

    if (!sifucoop::ui::StartInGameOverlay()) {
        SC_LOG("bootstrap: GUI unavailable; time-control hooks were not installed");
        return 0;
    }

    if (!sifucoop::hooks::InstallTickHook(base, engine)) {
        SC_LOG("bootstrap: tick hook not installed -- mod is inert");
        return 0;
    }

    SC_LOG("bootstrap: armed -- SifuTAS controls loaded from SifuTAS.ini");
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        sifucoop::log::Open();

        // From here every log line carries the host's clock and this machine's
        // role, so the two logs of one session can be read against each other.
        // Installed before anything else runs; it reports nothing until a
        // session exists, and costs two reads per line after that.

        if (!sifucoop::proxy::Init()) {
            SC_LOG("proxy: init incomplete -- audio may misbehave");
        }

        HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        sifucoop::log::Close();
    }
    return TRUE;
}
