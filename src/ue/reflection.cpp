#include "reflection.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../core/log.h"
#include "../core/offsets.g.h"

namespace sifucoop::ue {
namespace {

namespace offsets = sifucoop::offsets;

constexpr int kFNameAdd = 1;

using FNameCtorFn = FName*(__fastcall*)(FName*, const wchar_t*, int);
using FindFunctionFn = UFunction*(__fastcall*)(const UObject*, FName);
using ProcessEventFn = void(__fastcall*)(UObject*, UFunction*, void*);
using GetPlayerCharacterFn = UObject*(__fastcall*)(const UObject*, int);
using StaticClassFn = void*(__fastcall*)();
using MontageGetPositionFn = float(__fastcall*)(const UObject*, const UObject*);
using MontagePlayFn = float(__fastcall*)(UObject*, UObject*, float, int, float, bool);
using GetPathNameFn = void(__fastcall*)(const UObject* self, const UObject* stop_outer,
                                        void* out_string);
using FreeFn = void(__fastcall*)(void* memory);
using StaticFindObjectSafeFn = UObject*(__fastcall*)(void* uclass, UObject* outer,
                                                     const wchar_t* name, bool exact_class);

using OpenLevelFn = void(__fastcall*)(const UObject* world_context, FName level,
                                      bool absolute, void* options);
using IsInSeamlessTravelFn = bool(__fastcall*)(UObject* world);

FNameCtorFn g_fname_ctor = nullptr;
FindFunctionFn g_find_function = nullptr;
ProcessEventFn g_process_event = nullptr;
GetPlayerCharacterFn g_get_player_character = nullptr;
StaticClassFn g_skeletal_mesh_class = nullptr;
MontageGetPositionFn g_montage_get_position = nullptr;
MontagePlayFn g_montage_play = nullptr;
GetPathNameFn g_get_path_name = nullptr;
FreeFn g_free = nullptr;
StaticFindObjectSafeFn g_static_find_object_safe = nullptr;
OpenLevelFn g_open_level = nullptr;
IsInSeamlessTravelFn g_is_in_seamless_travel = nullptr;
UObject** g_gworld = nullptr;

bool g_ready = false;

// A zero offset means the PDB pass never found the symbol on this build.
// base + 0 is the PE header, which is not a function: resolve it to null so
// every caller's existing null check does what it says.
template <typename Fn>
Fn Resolve(std::uintptr_t base, std::uint32_t rva) {
    return rva ? reinterpret_cast<Fn>(base + rva) : nullptr;
}

std::uintptr_t g_module_base = 0;

FName MakeName(const wchar_t* text) {
    FName name = {};
    g_fname_ctor(&name, text, kFNameAdd);
    return name;
}

}

bool RangeReadable(const void* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;
    constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
    if (info.Protect & kNoRead) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}

namespace {

bool RangeWritable(const void* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        return false;
    const DWORD protection = info.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
        return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return reinterpret_cast<std::uintptr_t>(address) + size <= start + info.RegionSize;
}

template <typename T>
bool ReadOffset(const UObject* object, std::uint32_t offset, T* out) {
    if (!object || !offset || !out) return false;
    const auto* address = reinterpret_cast<const std::uint8_t*>(object) + offset;
    if (!RangeReadable(address, sizeof(T))) return false;
    std::memcpy(out, address, sizeof(T));
    return true;
}

template <typename T>
bool WriteOffset(UObject* object, std::uint32_t offset, const T& value) {
    if (!object || !offset) return false;
    auto* address = reinterpret_cast<std::uint8_t*>(object) + offset;
    if (!RangeWritable(address, sizeof(T))) return false;
    std::memcpy(address, &value, sizeof(T));
    return true;
}

bool ReadCameraView(const UObject* manager, std::uint32_t cache_offset, CameraView* out) {
    if (!manager || !out || !offsets::M_FCameraCacheEntry_POV) return false;
    const std::uint32_t view_offset = cache_offset + offsets::M_FCameraCacheEntry_POV;
    return ReadOffset(manager, view_offset + offsets::M_FMinimalViewInfo_Location,
                      &out->location) &&
           ReadOffset(manager, view_offset + offsets::M_FMinimalViewInfo_Rotation,
                      &out->rotation) &&
           ReadOffset(manager, view_offset + offsets::M_FMinimalViewInfo_FOV, &out->fov) &&
           std::isfinite(out->location.X) && std::isfinite(out->location.Y) &&
           std::isfinite(out->location.Z) && std::isfinite(out->rotation.Pitch) &&
           std::isfinite(out->rotation.Yaw) && std::isfinite(out->rotation.Roll) &&
           std::isfinite(out->fov);
}

bool WriteCameraView(UObject* manager, std::uint32_t cache_offset, const CameraView& view) {
    if (!manager || !offsets::M_FCameraCacheEntry_POV) return false;
    const std::uint32_t view_offset = cache_offset + offsets::M_FCameraCacheEntry_POV;
    return WriteOffset(manager, view_offset + offsets::M_FMinimalViewInfo_Location,
                       view.location) &&
           WriteOffset(manager, view_offset + offsets::M_FMinimalViewInfo_Rotation,
                       view.rotation) &&
           WriteOffset(manager, view_offset + offsets::M_FMinimalViewInfo_FOV, view.fov);
}

bool SameCameraView(const CameraView& a, const CameraView& b) {
    return std::fabs(a.location.X - b.location.X) <= 0.1f &&
           std::fabs(a.location.Y - b.location.Y) <= 0.1f &&
           std::fabs(a.location.Z - b.location.Z) <= 0.1f &&
           std::fabs(a.rotation.Pitch - b.rotation.Pitch) <= 0.1f &&
           std::fabs(std::remainder(a.rotation.Yaw - b.rotation.Yaw, 360.f)) <= 0.1f &&
           std::fabs(a.rotation.Roll - b.rotation.Roll) <= 0.1f &&
           std::fabs(a.fov - b.fov) <= 0.1f;
}

bool ReadViewTarget(UObject* manager, std::uint32_t offset, UObject** target,
                    CameraView* view) {
    if (!offset ||
        !ReadOffset(manager, offset + offsets::M_FTViewTarget_Target, target))
        return false;
    if (!view || !offsets::M_FTViewTarget_POV) return true;
    const std::uint32_t pov = offset + offsets::M_FTViewTarget_POV;
    return ReadOffset(manager, pov + offsets::M_FMinimalViewInfo_Location, &view->location) &&
           ReadOffset(manager, pov + offsets::M_FMinimalViewInfo_Rotation, &view->rotation) &&
           ReadOffset(manager, pov + offsets::M_FMinimalViewInfo_FOV, &view->fov) &&
           std::isfinite(view->location.X) && std::isfinite(view->location.Y) &&
           std::isfinite(view->location.Z) && std::isfinite(view->rotation.Pitch) &&
           std::isfinite(view->rotation.Yaw) && std::isfinite(view->rotation.Roll) &&
           std::isfinite(view->fov);
}

UObject* GetPlayerController(UObject* world) {
    UObject* statics = FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
    if (!world || !statics) return nullptr;
    struct Params {
        UObject* WorldContextObject;
        std::int32_t PlayerIndex;
        std::uint8_t pad[4];
        UObject* ReturnValue;
    } params = {};
    params.WorldContextObject = world;
    return CallFunction(statics, L"GetPlayerController", &params) ? params.ReturnValue
                                                                   : nullptr;
}

}

bool InitReflection(std::uintptr_t base) {
    g_module_base = base;
    g_fname_ctor = Resolve<FNameCtorFn>(base, offsets::FName_FromWide);
    g_find_function = Resolve<FindFunctionFn>(base, offsets::UObject_FindFunction);
    g_process_event = Resolve<ProcessEventFn>(base, offsets::UObject_ProcessEvent);
    g_get_player_character =
        Resolve<GetPlayerCharacterFn>(base, offsets::UGameplayStatics_GetPlayerCharacter);
    g_skeletal_mesh_class =
        Resolve<StaticClassFn>(base, offsets::USkeletalMeshComponent_StaticClass);
    g_montage_get_position =
        Resolve<MontageGetPositionFn>(base, offsets::UAnimInstance_Montage_GetPosition);
    g_montage_play = Resolve<MontagePlayFn>(base, offsets::UAnimInstance_Montage_Play);
    g_get_path_name = Resolve<GetPathNameFn>(base, offsets::UObjectBaseUtility_GetPathName);
    g_free = Resolve<FreeFn>(base, offsets::FMemory_Free);
    g_static_find_object_safe =
        Resolve<StaticFindObjectSafeFn>(base, offsets::StaticFindObjectSafe);
    g_open_level = Resolve<OpenLevelFn>(base, offsets::UGameplayStatics_OpenLevel);
    g_is_in_seamless_travel =
        Resolve<IsInSeamlessTravelFn>(base, offsets::UWorld_IsInSeamlessTravel);
    g_gworld = Resolve<UObject**>(base, offsets::GWorld);

    g_ready = g_fname_ctor && g_find_function && g_process_event && g_get_player_character &&
              g_gworld && g_get_path_name && g_free && g_static_find_object_safe;
    SC_LOG("reflection: %s", g_ready ? "ready" : "FAILED to resolve entry points");
    return g_ready;
}

bool MakeFName(const wchar_t* text, FName* out) {
    if (!g_ready || !text || !out) return false;
    *out = MakeName(text);
    return true;
}

bool CallFunction(UObject* object, const wchar_t* function_name, void* params) {
    if (!g_ready || !object) return false;

    const FName name = MakeName(function_name);
    UFunction* function = g_find_function(object, name);
    if (!function) return false;

    g_process_event(object, function, params);
    return true;
}

bool GetActorLocation(UObject* actor, FVector* out) {
    struct Params {
        FVector ReturnValue;
    } params = {};
    if (!CallFunction(actor, L"K2_GetActorLocation", &params)) return false;
    *out = params.ReturnValue;
    return true;
}

bool GetActorRotation(UObject* actor, FRotator* out) {
    struct Params {
        FRotator ReturnValue;
    } params = {};
    if (!CallFunction(actor, L"K2_GetActorRotation", &params)) return false;
    *out = params.ReturnValue;
    return true;
}

UObject* GetPlayerCharacter(UObject* world_context, int index) {
    if (!g_ready || !world_context) return nullptr;
    return g_get_player_character(world_context, index);
}

UObject* GetWorld() {

    if (!g_gworld) return nullptr;
    return *g_gworld;
}

bool IsWorldInSeamlessTravel(UObject* world) {
    return world && g_is_in_seamless_travel && g_is_in_seamless_travel(world);
}

bool ReadWorldTransitionState(UObject* world, bool* transitioning) {
    if (!world || !transitioning || !g_is_in_seamless_travel ||
        !offsets::M_UWorld_OwningGameInstance ||
        !offsets::M_UThePlainesGameInstance_loadingScreen || !IsValidObject(world))
        return false;
    bool in_transition = IsWorldInSeamlessTravel(world);
    UObject* game_instance = nullptr;
    if (!ReadOffset(world, offsets::M_UWorld_OwningGameInstance, &game_instance) ||
        !IsAliveObject(game_instance))
        return false;
    UObject* loading_screen = nullptr;
    if (!ReadOffset(game_instance, offsets::M_UThePlainesGameInstance_loadingScreen,
                    &loading_screen))
        return false;
    if (loading_screen) {
        if (!IsAliveObject(loading_screen)) return false;
        struct Params { bool ReturnValue; } params = {};
        if (!CallFunction(loading_screen, L"IsInViewport", &params)) return false;
        in_transition = in_transition || params.ReturnValue;
    }
    *transitioning = in_transition;
    return true;
}

bool GetObjectPathName(UObject* object, char* out, int out_size, int* required_size) {
    if (required_size) *required_size = 0;
    if (!g_get_path_name || !g_free || !object || !out || out_size <= 0) return false;

    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } result = {};

    g_get_path_name(object, nullptr, &result);

    if (!result.data) return false;
    if (result.num <= 0) {
        g_free(result.data);
        return false;
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, result.data, -1, nullptr, 0, nullptr,
                                             nullptr);
    if (required_size) *required_size = required;
    const bool converted = required > 0 && required <= out_size &&
                           WideCharToMultiByte(CP_UTF8, 0, result.data, -1, out, out_size,
                                               nullptr, nullptr) == required;
    if (!converted) out[0] = '\0';
    g_free(result.data);
    return converted;
}

UObject* FindObjectByPath(const wchar_t* path_name) {
    if (!g_static_find_object_safe || !path_name) return nullptr;

    return g_static_find_object_safe(nullptr, nullptr, path_name, false);
}

// The level path is cached per world because it does not change during that
// world's lifetime.
bool GetCurrentLevelPath(char* out, int out_size) {
    UObject* world = GetWorld();
    if (!world) return false;

    static UObject* cached_world = nullptr;
    static char cached_path[512] = {};
    if (cached_world != world || !cached_path[0]) {
        char full[512] = {};
        if (!GetObjectPathName(world, full, sizeof(full))) return false;
        char* dot = strrchr(full, '.');
        if (dot) *dot = '\0';
        lstrcpynA(cached_path, full, sizeof(cached_path));
        cached_world = world;
    }

    lstrcpynA(out, cached_path, out_size);
    return out[0] != '\0';
}

bool OpenLevel(const char* level_path) {
    if (!g_ready || !g_open_level || !level_path || !level_path[0]) return false;

    UObject* world = GetWorld();
    if (!world) return false;

    wchar_t wide[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, level_path, -1, wide, 512);

    const FName name = MakeName(wide);

    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } options = {};

    SC_LOG("level: opening '%s'", level_path);
    g_open_level(world, name, false, &options);
    return true;
}

bool ExecuteConsoleCommand(const char* command, UObject* specific_player) {
    if (!command || !command[0]) return false;
    UObject* world = GetWorld();
    UObject* kismet = FindObjectByPath(L"/Script/Engine.Default__KismetSystemLibrary");
    if (!world || !kismet) return false;

    wchar_t wide[512] = {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, command, -1, wide, 512);
    if (chars <= 0) return false;
    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } text = {wide, chars, chars};
    struct Params {
        UObject* WorldContextObject;
        FString Command;
        UObject* SpecificPlayer;
    } params = {world, text, specific_player};
    return CallFunction(kismet, L"ExecuteConsoleCommand", &params);
}

UObject* GetSkeletalMeshComponent(UObject* actor) {
    if (!g_ready || !actor || !g_skeletal_mesh_class) return nullptr;

    struct ComponentParams {
        void* ComponentClass;
        UObject* ReturnValue;
    } params = {};
    params.ComponentClass = g_skeletal_mesh_class();
    if (!CallFunction(actor, L"GetComponentByClass", &params)) return nullptr;
    return params.ReturnValue;
}

UObject* GetAnimInstance(UObject* actor) {
    UObject* mesh = GetSkeletalMeshComponent(actor);
    if (!mesh) return nullptr;
    struct AnimParams {
        UObject* ReturnValue;
    } anim_params = {};
    if (!CallFunction(mesh, L"GetAnimInstance", &anim_params)) return nullptr;
    return anim_params.ReturnValue;
}

bool ReadAnimState(UObject* actor, AnimState* out) {
    if (!out) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    struct MontageParams {
        UObject* ReturnValue;
    } montage_params = {};
    if (!CallFunction(anim_instance, L"GetCurrentActiveMontage", &montage_params)) return false;

    out->montage = montage_params.ReturnValue;
    out->position = 0.f;
    out->position_valid = out->montage == nullptr || g_montage_get_position != nullptr;

    if (out->montage && g_montage_get_position) {
        out->position = g_montage_get_position(anim_instance, out->montage);
        out->position_valid = std::isfinite(out->position);
    }
    return true;
}

bool SetActiveMontagePosition(UObject* actor, const AnimState& saved) {
    if (!saved.montage || !saved.position_valid || !IsAliveObject(saved.montage)) return false;
    AnimState current = {};
    if (!ReadAnimState(actor, &current) || current.montage != saved.montage) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;
    struct Params {
        UObject* Montage;
        float NewPosition;
    } params = {saved.montage, saved.position};
    if (!CallFunction(anim_instance, L"Montage_SetPosition", &params)) return false;
    AnimState after = {};
    return ReadAnimState(actor, &after) && after.montage == saved.montage &&
           after.position_valid && std::fabs(after.position - saved.position) <= 0.02f;
}

bool ReadPlayerCameraState(UObject* world, PlayerCameraState* out) {
    if (!world || !out) return false;
    UObject* controller = GetPlayerController(world);
    if (!IsAliveObject(controller)) return false;
    UObject* manager = nullptr;
    if (!ReadOffset(controller, offsets::M_APlayerController_PlayerCameraManager, &manager) ||
        !IsAliveObject(manager))
        return false;
    PlayerCameraState state = {};
    state.controller = controller;
    state.manager = manager;
    if (!ReadOffset(controller, offsets::M_AController_ControlRotation,
                    &state.control_rotation) ||
        !ReadCameraView(manager, offsets::M_APlayerCameraManager_CameraCachePrivate,
                        &state.current) ||
        !ReadCameraView(manager, offsets::M_APlayerCameraManager_LastFrameCameraCachePrivate,
                        &state.previous))
        return false;
    if (offsets::M_APlayerCameraManager_ViewTarget && offsets::M_FTViewTarget_POV) {
        state.target_view_available = ReadViewTarget(
            manager, offsets::M_APlayerCameraManager_ViewTarget, &state.view_target,
            &state.target_view);
        if (state.view_target && IsAliveObject(state.view_target)) {
            state.view_target_transform_available =
                GetActorLocation(state.view_target, &state.view_target_location) &&
                GetActorRotation(state.view_target, &state.view_target_rotation);
        }
        ReadViewTarget(manager, offsets::M_APlayerCameraManager_PendingViewTarget,
                       &state.pending_view_target, nullptr);
    }
    ReadOffset(manager, offsets::M_APlayerCameraManager_ViewTargetOffset,
               &state.view_target_offset);
    if (offsets::M_APlayerCameraManager_TransformComponent) {
        ReadOffset(manager, offsets::M_APlayerCameraManager_TransformComponent,
                   &state.transform_component);
        if (state.transform_component && IsAliveObject(state.transform_component)) {
            struct VectorParams { FVector ReturnValue; } location = {};
            struct RotationParams { FRotator ReturnValue; } rotation = {};
            state.component_transform_available =
                CallFunction(state.transform_component, L"K2_GetComponentLocation", &location) &&
                CallFunction(state.transform_component, L"K2_GetComponentRotation", &rotation);
            if (state.component_transform_available) {
                state.manager_component_location = location.ReturnValue;
                state.manager_component_rotation = rotation.ReturnValue;
            }
        }
    }
    *out = state;
    return true;
}

bool RestorePlayerCameraState(const PlayerCameraState& state) {
    if (!IsAliveObject(state.controller) || !IsAliveObject(state.manager)) return false;
    UObject* manager = nullptr;
    if (!ReadOffset(state.controller, offsets::M_APlayerController_PlayerCameraManager,
                    &manager) || manager != state.manager)
        return false;
    struct SetRotationParams {
        FRotator NewRotation;
    } params = {state.control_rotation};
    if (!CallFunction(state.controller, L"SetControlRotation", &params)) return false;
    FRotator control_rotation = {};
    if (!ReadOffset(state.controller, offsets::M_AController_ControlRotation,
                    &control_rotation) ||
        std::fabs(control_rotation.Pitch - state.control_rotation.Pitch) > 0.1f ||
        std::fabs(std::remainder(control_rotation.Yaw - state.control_rotation.Yaw,
                                 360.f)) > 0.1f ||
        std::fabs(control_rotation.Roll - state.control_rotation.Roll) > 0.1f)
        return false;
    if (!WriteCameraView(manager, offsets::M_APlayerCameraManager_CameraCachePrivate,
                         state.current) ||
        !WriteCameraView(manager, offsets::M_APlayerCameraManager_LastFrameCameraCachePrivate,
                         state.previous))
        return false;
    CameraView current = {};
    CameraView previous = {};
    return ReadCameraView(manager, offsets::M_APlayerCameraManager_CameraCachePrivate,
                          &current) &&
           ReadCameraView(manager, offsets::M_APlayerCameraManager_LastFrameCameraCachePrivate,
                          &previous) &&
           SameCameraView(current, state.current) && SameCameraView(previous, state.previous);
}

bool ResetClothForTeleport(UObject* actor) {
    UObject* mesh = GetSkeletalMeshComponent(actor);
    if (!IsAliveObject(mesh)) return false;
    struct Params {} params;
    return CallFunction(mesh, L"ForceClothNextUpdateTeleportAndReset", &params);
}

bool ApplyAnimState(UObject* actor, const AnimState& state) {
    if (!state.montage || !g_montage_play) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    g_montage_play(anim_instance, state.montage, 1.f, 0, state.position, true);
    return true;
}

bool ClearActiveMontage(UObject* actor) {
    AnimState before = {};
    if (!ReadAnimState(actor, &before)) return false;
    if (!before.montage) return true;

    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;
    struct Params {
        float InBlendOutTime;
        UObject* Montage;
    } params = {0.f, nullptr};
    if (!CallFunction(anim_instance, L"Montage_Stop", &params)) return false;

    AnimState after = {};
    return ReadAnimState(actor, &after) && after.montage == nullptr;
}

bool PlayAnimationAsset(UObject* actor, UObject* animation_asset, float start_at) {
    if (!animation_asset) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    struct Params {
        UObject* Asset;
        FName SlotNodeName;
        float BlendInTime;
        float BlendOutTime;
        float InPlayRate;
        std::int32_t LoopCount;
        float BlendOutTriggerTime;
        float InTimeToStartMontageAt;
        UObject* ReturnValue;
    } params = {};
    params.Asset = animation_asset;
    params.SlotNodeName = MakeName(L"Cinematic");
    params.BlendInTime = 0.03f;
    params.BlendOutTime = 0.08f;
    params.InPlayRate = 1.f;
    params.LoopCount = 1;
    params.BlendOutTriggerTime = -1.f;

    params.InTimeToStartMontageAt = start_at > 0.f ? start_at : 0.f;
    if (!CallFunction(anim_instance, L"PlaySlotAnimationAsDynamicMontage", &params)) {
        return false;
    }
    return params.ReturnValue != nullptr;
}

bool GetObjectClassPathName(UObject* object, char* out, int out_size, int* required_size) {
    if (!object || !out || out_size <= 0) return false;
    out[0] = 0;
    auto* klass = *reinterpret_cast<UObject**>(reinterpret_cast<std::uintptr_t>(object) + 0x10);
    if (!klass) return false;
    return GetObjectPathName(klass, out, out_size, required_size);
}

bool ObjectClassIs(UObject* object, const char* leaf_name) {
    char path[160] = {};
    if (!GetObjectClassPathName(object, path, sizeof(path))) return false;
    const char* dot = strrchr(path, '.');
    const char* leaf = dot ? dot + 1 : path;
    return lstrcmpA(leaf, leaf_name) == 0;
}

void* GetAnimInstanceClass(UObject* actor) {
    UObject* instance = GetAnimInstance(actor);
    if (!instance) return nullptr;

    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(instance) + 0x10);
}

bool RestoreAnimationBlueprint(UObject* actor, void* anim_class) {
    UObject* mesh = GetSkeletalMeshComponent(actor);
    if (!mesh) return false;

    struct ModeParams {
        std::uint8_t InAnimationMode;
        bool bForceInitAnimScript;
    } mode = {0, true};
    CallFunction(mesh, L"SetAnimationMode", &mode);

    if (GetAnimInstance(actor)) return true;

    if (!anim_class) return false;
    struct ClassParams {
        void* NewClass;
    } klass = {anim_class};
    if (!CallFunction(mesh, L"SetAnimInstanceClass", &klass)) return false;
    return GetAnimInstance(actor) != nullptr;
}

// Safe to READ; not proof of liveness. See CODE-NOTES.md.
bool IsValidObject(UObject* object) {
    if (!object || g_module_base == 0) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (address < 0x10000 || (address & 7) != 0) return false;
    if (!RangeReadable(object, 0x20)) return false;

    constexpr std::uintptr_t kModuleSpan = 0x10000000u;
    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
    if (vtable < g_module_base || vtable - g_module_base > kModuleSpan) return false;

    // UObjectBase::ClassPrivate
    const auto class_private = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const std::uint8_t*>(object) + 0x10);
    if (class_private < 0x10000 || (class_private & 7) != 0) return false;
    if (!RangeReadable(reinterpret_cast<const void*>(class_private), 8)) return false;

    const auto class_vtable = *reinterpret_cast<const std::uintptr_t*>(class_private);
    return class_vtable >= g_module_base && class_vtable - g_module_base <= kModuleSpan;
}

bool IsAliveObject(UObject* object) {
    if (!IsValidObject(object)) return false;
    static UObject* kismet = nullptr;
    if (!kismet)
        kismet = FindObjectByPath(L"/Script/Engine.Default__KismetSystemLibrary");
    if (!kismet || !IsValidObject(kismet)) return false;
    struct Params {
        UObject* Object;
        bool ReturnValue;
    } params = {object, false};
    return CallFunction(kismet, L"IsValid", &params) && params.ReturnValue;
}

float GetAnimationAssetLength(UObject* animation_asset) {
    if (!animation_asset) return 0.f;
    struct Params {
        float ReturnValue;
    } params = {};
    if (!CallFunction(animation_asset, L"GetPlayLength", &params)) return 0.f;
    return params.ReturnValue;
}

}
