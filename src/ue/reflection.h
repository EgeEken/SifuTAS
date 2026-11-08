#pragma once

#include <cstddef>
#include <cstdint>

namespace sifucoop::ue {

struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
};

struct FRotator {
    float Pitch = 0.f, Yaw = 0.f, Roll = 0.f;
};

struct FName {
    std::int32_t comparison_index = 0;
    std::int32_t number = 0;
};

bool MakeFName(const wchar_t* text, FName* out);

struct UObject;
struct UFunction;

bool InitReflection(std::uintptr_t module_base);

bool CallFunction(UObject* object, const wchar_t* function_name, void* params);

bool GetActorLocation(UObject* actor, FVector* out);
bool GetActorRotation(UObject* actor, FRotator* out);

UObject* GetPlayerCharacter(UObject* world_context, int index);
UObject* GetPlayerController(UObject* world_context, int index);

UObject* GetWorld();
bool IsWorldInSeamlessTravel(UObject* world);
bool ReadWorldTransitionState(UObject* world, bool* transitioning);

struct AnimState {
    UObject* montage = nullptr;
    float position = 0.f;
    bool position_valid = false;
};

struct CameraView {
    FVector location = {};
    FRotator rotation = {};
    float fov = 0.f;
};

struct PlayerCameraState {
    UObject* controller = nullptr;
    UObject* manager = nullptr;
    UObject* view_target = nullptr;
    UObject* pending_view_target = nullptr;
    UObject* transform_component = nullptr;
    FRotator control_rotation = {};
    CameraView current = {};
    CameraView previous = {};
    CameraView target_view = {};
    FVector view_target_location = {};
    FRotator view_target_rotation = {};
    FVector view_target_offset = {};
    FVector manager_component_location = {};
    FRotator manager_component_rotation = {};
    bool target_view_available = false;
    bool view_target_transform_available = false;
    bool component_transform_available = false;
};

bool ReadAnimState(UObject* actor, AnimState* out);
bool SetActiveMontagePosition(UObject* actor, const AnimState& saved);
bool ReadPlayerCameraState(UObject* world, PlayerCameraState* out);
bool RestorePlayerCameraState(const PlayerCameraState& state);
bool ResetClothForTeleport(UObject* actor);

bool ApplyAnimState(UObject* actor, const AnimState& state);
bool ClearActiveMontage(UObject* actor);

bool PlayAnimationAsset(UObject* actor, UObject* animation_asset, float start_at = 0.f);

void* GetAnimInstanceClass(UObject* actor);

bool RestoreAnimationBlueprint(UObject* actor, void* anim_class);

bool IsValidObject(UObject* object);
bool IsAliveObject(UObject* object);

// Whether [address, address + size) is committed, readable memory.
bool RangeReadable(const void* address, std::size_t size);

float GetAnimationAssetLength(UObject* animation_asset);

UObject* GetSkeletalMeshComponent(UObject* actor);
UObject* GetAnimInstance(UObject* actor);

bool GetObjectPathName(UObject* object, char* out, int out_size,
                       int* required_size = nullptr);

bool GetObjectClassPathName(UObject* object, char* out, int out_size,
                            int* required_size = nullptr);
bool ObjectClassIs(UObject* object, const char* leaf_name);

UObject* FindObjectByPath(const wchar_t* path_name);

bool GetCurrentLevelPath(char* out, int out_size);
bool GetWorldMatchState(UObject* world, FName* out);

bool OpenLevel(const char* level_path);

bool ExecuteConsoleCommand(const char* command, UObject* specific_player = nullptr);

}
