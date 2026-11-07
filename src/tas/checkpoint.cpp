#include "checkpoint.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/hooks.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../game/actors.h"
#include "../game/enemies.h"
#include "../ue/reflection.h"

namespace sifucoop::tas::checkpoint {
namespace {

namespace game = sifucoop::game;
namespace ue = sifucoop::ue;
namespace offsets = sifucoop::offsets;

constexpr std::uint32_t kSaveRequest = 1;
constexpr std::uint32_t kLoadRequest = 2;
constexpr int kMaxCharacters = 32;
constexpr int kEnumerationBuffer = 256;

enum Field : std::uint32_t {
    Location = 1u << 0,
    Rotation = 1u << 1,
    Velocity = 1u << 2,
    Health = 1u << 3,
    MaxHealth = 1u << 4,
    Guard = 1u << 5,
    Down = 1u << 6,
    Dead = 1u << 7,
    TimeDilation = 1u << 8,
    Animation = 1u << 9,
};

constexpr std::uint32_t kAllCaptureFields = (1u << 10) - 1u;
constexpr std::uint32_t kVitalsFields = Health | MaxHealth | Guard;
constexpr int kIdentitySize = 512;
constexpr int kClassPathSize = 256;

enum class CaptureFailure {
    None,
    InvalidActor,
    IdentityUnavailable,
    IdentityTruncated,
    ClassPathUnavailable,
    ClassPathTruncated,
};

struct ActorState {
    ue::UObject* actor = nullptr;
    char identity[kIdentitySize] = {};
    char class_path[kClassPathSize] = {};
    std::uint32_t fields = 0;
    bool player = false;
    ue::FVector location = {};
    ue::FRotator rotation = {};
    ue::FVector velocity = {};
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    float time_dilation = 1.f;
    bool down = false;
    bool dead = false;
    ue::AnimState animation = {};
};

struct Snapshot {
    bool valid = false;
    bool incomplete = false;
    ue::UObject* world = nullptr;
    int character_count = 0;
    int enumerated_count = 0;
    int world_actor_count = 0;
    int pooled_count = 0;
    int invalid_count = 0;
    int missing_health_count = 0;
    int inactive_count = 0;
    int identity_truncated_count = 0;
    int identity_unavailable_count = 0;
    int capacity_skipped_count = 0;
    int player_index = -1;
    bool camera_available = false;
    ue::PlayerCameraState camera = {};
    CheckpointReference reference = {};
    ActorState characters[kMaxCharacters] = {};
};

SRWLOCK g_lock = SRWLOCK_INIT;
Snapshot g_snapshot = {};
Status g_status = {};
std::atomic<std::uint32_t> g_requests{0};
bool g_camera_diagnostics_pending = false;
unsigned int g_camera_diagnostic_frame = 0;

bool Finite(const ue::FVector& value) {
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

bool ReadableFloatField(const ue::UObject* object, std::uint32_t offset) {
    if (!object || !offset) return false;
    const auto* field = reinterpret_cast<const std::uint8_t*>(object) + offset;
    return ue::RangeReadable(field, sizeof(float));
}

bool CopyString(char* out, int size, const char* value) {
    if (!out || size <= 0 || !value || strlen(value) >= static_cast<std::size_t>(size))
        return false;
    lstrcpynA(out, value, size);
    return true;
}

void LogCameraState(const char* stage, const ue::PlayerCameraState& state) {
    SC_LOG("tas: checkpoint camera stage=%s controller=%p control=%.2f,%.2f,%.2f "
           "current=%.1f,%.1f,%.1f/%.2f,%.2f,%.2f/%.2f "
           "previous=%.1f,%.1f,%.1f/%.2f,%.2f,%.2f/%.2f "
           "view_target=%p pending_target=%p target_view_valid=%u "
           "target_transform_valid=%u target_transform=%.1f,%.1f,%.1f/%.2f,%.2f,%.2f "
           "target=%.1f,%.1f,%.1f/%.2f,%.2f,%.2f/%.2f offset=%.1f,%.1f,%.1f "
           "transform_component=%p component_valid=%u component=%.1f,%.1f,%.1f/%.2f,%.2f,%.2f",
           stage, state.controller, state.control_rotation.Pitch, state.control_rotation.Yaw,
           state.control_rotation.Roll, state.current.location.X, state.current.location.Y,
           state.current.location.Z, state.current.rotation.Pitch, state.current.rotation.Yaw,
           state.current.rotation.Roll, state.current.fov, state.previous.location.X,
           state.previous.location.Y, state.previous.location.Z, state.previous.rotation.Pitch,
           state.previous.rotation.Yaw, state.previous.rotation.Roll, state.previous.fov,
           state.view_target, state.pending_view_target, state.target_view_available ? 1u : 0u,
           state.view_target_transform_available ? 1u : 0u,
           state.view_target_location.X, state.view_target_location.Y,
           state.view_target_location.Z, state.view_target_rotation.Pitch,
           state.view_target_rotation.Yaw, state.view_target_rotation.Roll,
           state.target_view.location.X, state.target_view.location.Y,
           state.target_view.location.Z, state.target_view.rotation.Pitch,
           state.target_view.rotation.Yaw, state.target_view.rotation.Roll,
           state.target_view.fov, state.view_target_offset.X, state.view_target_offset.Y,
           state.view_target_offset.Z, state.transform_component,
           state.component_transform_available ? 1u : 0u, state.manager_component_location.X,
           state.manager_component_location.Y, state.manager_component_location.Z,
           state.manager_component_rotation.Pitch, state.manager_component_rotation.Yaw,
           state.manager_component_rotation.Roll);
}

void SetResult(const char* operation, const char* message) {
    AcquireSRWLockExclusive(&g_lock);
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "%s: %s", operation, message);
    ReleaseSRWLockExclusive(&g_lock);
    SC_LOG("tas: checkpoint %s: %s", operation, message);
}

bool CaptureActor(ue::UObject* actor, bool player, ActorState* out,
                  CaptureFailure* failure) {
    *failure = CaptureFailure::None;
    if (!ue::IsAliveObject(actor) || !out) {
        *failure = CaptureFailure::InvalidActor;
        return false;
    }
    out->actor = actor;
    out->player = player;
    int required = 0;
    if (!ue::GetObjectPathName(actor, out->identity, sizeof(out->identity), &required)) {
        *failure = required > static_cast<int>(sizeof(out->identity))
                       ? CaptureFailure::IdentityTruncated
                       : CaptureFailure::IdentityUnavailable;
        return false;
    }
    if (!ue::GetObjectClassPathName(actor, out->class_path, sizeof(out->class_path),
                                    &required)) {
        *failure = required > static_cast<int>(sizeof(out->class_path))
                       ? CaptureFailure::ClassPathTruncated
                       : CaptureFailure::ClassPathUnavailable;
        return false;
    }

    if (ue::GetActorLocation(actor, &out->location) && Finite(out->location))
        out->fields |= Location;
    if (ue::GetActorRotation(actor, &out->rotation) &&
        std::isfinite(out->rotation.Pitch) && std::isfinite(out->rotation.Yaw) &&
        std::isfinite(out->rotation.Roll))
        out->fields |= Rotation;

    if (game::GetActorVelocity(actor, &out->velocity) && Finite(out->velocity))
        out->fields |= Velocity;

    const game::Fighter fighter = game::ResolveFighter(actor);
    if (fighter.health && ue::IsAliveObject(fighter.health)) {
        if (ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fHealth)) {
            out->health = game::GetHealth(fighter);
            if (std::isfinite(out->health)) out->fields |= Health;
        }
        if (ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fMaxHealth)) {
            out->max_health = game::GetMaxHealth(fighter);
            if (std::isfinite(out->max_health)) out->fields |= MaxHealth;
        }
        if (game::ReadDownDeathState(fighter, &out->down, &out->dead))
            out->fields |= Down | Dead;
    }
    if (fighter.defense && ue::IsAliveObject(fighter.defense) &&
        ReadableFloatField(fighter.defense, offsets::M_UDefenseComponent_fCurrentGuard)) {
        out->guard = game::GetGuard(fighter);
        if (std::isfinite(out->guard)) out->fields |= Guard;
    }

    if (ReadableFloatField(actor, offsets::M_AActor_CustomTimeDilation)) {
        out->time_dilation = game::GetActorTimeDilation(actor);
        if (std::isfinite(out->time_dilation)) out->fields |= TimeDilation;
    }

    if (ue::ReadAnimState(actor, &out->animation) && out->animation.position_valid &&
        (!out->animation.montage || ue::IsAliveObject(out->animation.montage)))
        out->fields |= Animation;
    return true;
}

bool AppendActor(Snapshot* snapshot, ue::UObject* actor, bool player) {
    if (!actor) return true;
    for (int i = 0; i < snapshot->character_count; ++i) {
        if (snapshot->characters[i].actor == actor) {
            if (player) {
                snapshot->characters[i].player = true;
                snapshot->player_index = i;
            }
            return true;
        }
    }
    if (snapshot->character_count >= kMaxCharacters) {
        snapshot->incomplete = true;
        ++snapshot->capacity_skipped_count;
        return false;
    }
    ActorState& state = snapshot->characters[snapshot->character_count];
    CaptureFailure failure = CaptureFailure::None;
    if (!CaptureActor(actor, player, &state, &failure)) {
        snapshot->incomplete = true;
        if (failure == CaptureFailure::InvalidActor) ++snapshot->invalid_count;
        else if (failure == CaptureFailure::IdentityTruncated ||
                 failure == CaptureFailure::ClassPathTruncated)
            ++snapshot->identity_truncated_count;
        else
            ++snapshot->identity_unavailable_count;
        return true;
    }
    if (state.fields != kAllCaptureFields) snapshot->incomplete = true;
    if (player) snapshot->player_index = snapshot->character_count;
    ++snapshot->character_count;
    return true;
}

bool Capture(Snapshot* out, char* level, int level_size, const char** error) {
    const bool have_level = ue::GetCurrentLevelPath(level, level_size);
    if (!have_level) lstrcpynA(level, "<unavailable>", level_size);
    if (!have_level) {
        *error = "current world level identity could not be read";
        return false;
    }

    ue::UObject* world = ue::GetWorld();
    if (!ue::IsAliveObject(world)) {
        *error = "current world is unavailable";
        return false;
    }
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!game::CheckpointGameplayState(world, player, error)) return false;
    if (!hooks::IsPaused()) {
        *error = "pause the simulation before saving (F1)";
        return false;
    }

    Snapshot snapshot = {};
    snapshot.world = world;
    if (!CopyString(snapshot.reference.level_path,
                    sizeof(snapshot.reference.level_path), level)) {
        *error = "current level identity is too long for the checkpoint slot";
        return false;
    }
    if (!AppendActor(&snapshot, player, true) || snapshot.player_index < 0) {
        *error = "player identity could not be captured";
        return false;
    }
    snapshot.enumerated_count = 1;

    ue::UObject* fighters[kEnumerationBuffer] = {};
    int fighter_count = 0;
    int fighter_total = 0;
    const bool enumerated = game::EnumerateWorldFighters(
        fighters, kEnumerationBuffer, &fighter_count, &fighter_total);
    if (!enumerated) {
        snapshot.incomplete = true;
        SC_LOG("tas: checkpoint character enumeration unavailable; saving player snapshot only");
    } else {
        snapshot.world_actor_count = fighter_total;
        for (int i = 0; i < fighter_count; ++i) {
            ue::UObject* actor = fighters[i];
            if (actor == player) continue;
            if (!ue::IsAliveObject(actor)) {
                ++snapshot.invalid_count;
                continue;
            }
            ue::FVector location = {};
            if (!ue::GetActorLocation(actor, &location) || !Finite(location)) {
                ++snapshot.invalid_count;
                continue;
            }
            if (game::IsPooled(location)) {
                ++snapshot.pooled_count;
                continue;
            }
            const game::Fighter fighter = game::ResolveFighter(actor);
            if (!fighter.health || !ue::IsAliveObject(fighter.health)) {
                ++snapshot.missing_health_count;
                continue;
            }
            const float health = game::GetHealth(fighter);
            if (!std::isfinite(health) || health <= 0.5f) {
                ++snapshot.inactive_count;
                continue;
            }
            ++snapshot.enumerated_count;
            AppendActor(&snapshot, actor, false);
        }
        if (fighter_total > fighter_count) snapshot.incomplete = true;
    }

    if (snapshot.enumerated_count < snapshot.character_count)
        snapshot.enumerated_count = snapshot.character_count;
    if (snapshot.capacity_skipped_count || snapshot.identity_truncated_count ||
        snapshot.identity_unavailable_count || snapshot.missing_health_count ||
        snapshot.invalid_count)
        snapshot.incomplete = true;

    snapshot.camera_available = ue::ReadPlayerCameraState(world, &snapshot.camera);
    if (!snapshot.camera_available) {
        snapshot.incomplete = true;
        SC_LOG("tas: checkpoint camera state unavailable at capture");
    }

    ActorState& player_state = snapshot.characters[snapshot.player_index];
    if (!CopyString(snapshot.reference.player_identity,
                    sizeof(snapshot.reference.player_identity), player_state.identity)) {
        *error = "player identity is too long for the checkpoint reference";
        return false;
    }
    for (int i = 0; i < snapshot.character_count; ++i) {
        const ActorState& state = snapshot.characters[i];
        if (state.player) continue;
        CopyString(snapshot.reference.enemy_identity,
                   sizeof(snapshot.reference.enemy_identity), state.identity);
        break;
    }
    snapshot.reference.engine_tick = hooks::FrameCount();
    snapshot.reference.simulation_frame = 0;
    snapshot.reference.simulation_frame_verified = false;
    snapshot.reference.recording_branch_id[0] = '\0';
    snapshot.valid = true;
    *out = snapshot;
    return true;
}

bool SameRotation(const ue::FRotator& a, const ue::FRotator& b) {
    return std::fabs(a.Pitch - b.Pitch) <= 2.f &&
           std::fabs(a.Roll - b.Roll) <= 2.f &&
           std::fabs(std::remainder(a.Yaw - b.Yaw, 360.f)) <= 2.f;
}

bool SameLocation(const ue::FVector& a, const ue::FVector& b) {
    const float x = a.X - b.X;
    const float y = a.Y - b.Y;
    const float z = a.Z - b.Z;
    return x * x + y * y + z * z <= 16.f;
}

bool SameVector(const ue::FVector& a, const ue::FVector& b) {
    const float x = a.X - b.X;
    const float y = a.Y - b.Y;
    const float z = a.Z - b.Z;
    return x * x + y * y + z * z <= 1.f;
}

bool ActorIdentityMatches(const ActorState& state) {
    if (!state.actor || !ue::IsAliveObject(state.actor)) return false;
    char identity[kIdentitySize] = {};
    char class_path[kClassPathSize] = {};
    return ue::GetObjectPathName(state.actor, identity, sizeof(identity)) &&
           ue::GetObjectClassPathName(state.actor, class_path, sizeof(class_path)) &&
           strcmp(identity, state.identity) == 0 &&
           strcmp(class_path, state.class_path) == 0;
}

bool VerifyTransform(ue::UObject* actor, const ActorState& state,
                     std::uint32_t* restored) {
    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(actor, &location) || !ue::GetActorRotation(actor, &rotation))
        return false;
    const bool location_ok = !(state.fields & Location) ||
                             SameLocation(location, state.location);
    const bool rotation_ok = !(state.fields & Rotation) ||
                             SameRotation(rotation, state.rotation);
    if (location_ok && (state.fields & Location)) *restored |= Location;
    if (rotation_ok && (state.fields & Rotation)) *restored |= Rotation;
    return location_ok && rotation_ok;
}

void RestoreActor(const Snapshot& snapshot, const ActorState& state, int index,
                  int* restored_count, int* partial_count, int* skipped_count,
                  int* cloth_reset_count, ue::UObject* const* fighters,
                  int fighter_count, bool fighters_available,
                  ActorDiagnostic* diagnostic) {
    if (diagnostic) {
        lstrcpynA(diagnostic->identity, state.identity, sizeof(diagnostic->identity));
        diagnostic->captured_fields = state.fields;
        diagnostic->player = state.player;
    }
    const char* reason = "";
    if (!ActorIdentityMatches(state)) {
        reason = "actor missing or UObject identity/class changed";
    } else if (state.player) {
        if (ue::GetPlayerCharacter(snapshot.world, 0) != state.actor)
            reason = "saved player is no longer the current player";
    } else if (!fighters_available) {
        reason = "current-world fighter enumeration unavailable";
    } else {
        bool found = false;
        for (int i = 0; i < fighter_count; ++i) {
            if (fighters[i] == state.actor) {
                found = true;
                break;
            }
        }
        if (!found) reason = "actor is no longer a current-world FightingCharacter";
    }
    if (reason[0]) {
        ++*skipped_count;
        if (diagnostic) diagnostic->skipped = true;
        SC_LOG("tas: checkpoint restore actor[%d] '%s' skipped: %s", index,
               state.identity, reason);
        return;
    }

    const game::Fighter fighter = game::ResolveFighter(state.actor);
    std::uint32_t restored = 0;
    std::uint32_t failed = 0;
    std::uint32_t unsupported = 0;
    bool current_down = false;
    bool current_dead = false;
    const bool current_life_state =
        game::ReadDownDeathState(fighter, &current_down, &current_dead);
    ue::AnimState current_animation = {};
    const bool current_animation_state = ue::ReadAnimState(state.actor, &current_animation) &&
                                         current_animation.position_valid &&
                                         (!current_animation.montage ||
                                          ue::IsAliveObject(current_animation.montage));
    const bool saved_life_state = (state.fields & (Down | Dead)) == (Down | Dead);
    const bool saved_animation_state = (state.fields & Animation) != 0;
    const bool life_state_safe = saved_life_state && current_life_state && !state.down &&
                                 !state.dead && !current_down && !current_dead;
    const bool animation_safe = saved_animation_state && current_animation_state &&
                                !state.animation.montage && !current_animation.montage;
    const bool safe_motion = life_state_safe && animation_safe;

    if ((state.fields & (Location | Rotation)) != 0) {
        ue::FVector target_location = state.location;
        ue::FRotator target_rotation = state.rotation;
        const bool have_location = (state.fields & Location) != 0 ||
                                   ue::GetActorLocation(state.actor, &target_location);
        const bool have_rotation = (state.fields & Rotation) != 0 ||
                                   ue::GetActorRotation(state.actor, &target_rotation);
        if (!safe_motion) {
            unsupported |= state.fields & (Location | Rotation);
        } else if (!have_location || !have_rotation ||
                   !game::TeleportActor(state.actor, target_location, target_rotation)) {
            failed |= state.fields & (Location | Rotation);
        } else {
            if (!VerifyTransform(state.actor, state, &restored))
                failed |= state.fields & (Location | Rotation) & ~restored;
            const bool cloth_reset = ue::ResetClothForTeleport(state.actor);
            if (cloth_reset) ++*cloth_reset_count;
            SC_LOG("tas: checkpoint restore actor[%d] cloth_reset=%s", index,
                   cloth_reset ? "queued" : "unavailable");
        }
    }
    if (state.fields & Velocity) {
        if (!safe_motion) {
            unsupported |= Velocity;
        } else if (game::SetPresentationVelocity(state.actor, state.velocity)) {
            ue::FVector velocity = {};
            if (game::GetActorVelocity(state.actor, &velocity) &&
                SameVector(velocity, state.velocity))
                restored |= Velocity;
            else
                failed |= Velocity;
        } else {
            failed |= Velocity;
        }
    }

    const bool life_fields_available = (state.fields & (Down | Dead)) == (Down | Dead);
    if (!life_fields_available || !current_life_state || state.down || state.dead ||
        current_down || current_dead ||
        ((state.fields & Health) && state.health <= 0.5f) ||
        ((state.fields & Health) && game::GetHealth(fighter) <= 0.5f)) {
        unsupported |= state.fields & kVitalsFields;
    } else {
        if (state.fields & MaxHealth) {
            if (!fighter.health || !ue::IsAliveObject(fighter.health) ||
                !ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fMaxHealth)) {
                failed |= MaxHealth;
            } else {
                game::SetMaxHealth(fighter, state.max_health);
                if (std::fabs(game::GetMaxHealth(fighter) - state.max_health) <= 0.1f)
                    restored |= MaxHealth;
                else
                    failed |= MaxHealth;
            }
        }
        if (state.fields & Health) {
            if (!fighter.health || !ue::IsAliveObject(fighter.health) ||
                !ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fHealth)) {
                failed |= Health;
            } else {
                game::SetHealth(fighter, state.health);
                if (std::fabs(game::GetHealth(fighter) - state.health) <= 0.1f)
                    restored |= Health;
                else
                    failed |= Health;
            }
        }
        if (state.fields & Guard) {
            if (!fighter.defense || !ue::IsAliveObject(fighter.defense) ||
                !ReadableFloatField(fighter.defense,
                                    offsets::M_UDefenseComponent_fCurrentGuard)) {
                failed |= Guard;
            } else {
                game::SetGuard(fighter, state.guard);
                if (std::fabs(game::GetGuard(fighter) - state.guard) <= 0.1f)
                    restored |= Guard;
                else
                    failed |= Guard;
            }
        }
    }

    if (state.fields & TimeDilation) {
        if (game::SetActorTimeDilation(state.actor, state.time_dilation) &&
            std::fabs(game::GetActorTimeDilation(state.actor) - state.time_dilation) <= 0.001f)
            restored |= TimeDilation;
        else
            failed |= TimeDilation;
    }

    if (state.fields & (Down | Dead)) {
        if (saved_life_state && current_life_state && state.down == current_down &&
            state.dead == current_dead)
            restored |= state.fields & (Down | Dead);
        else
            unsupported |= state.fields & (Down | Dead);
    }
    if (state.fields & Animation) {
        if (!current_animation_state)
            unsupported |= Animation;
        else if (!state.animation.montage && !current_animation.montage)
            restored |= Animation;
        else if (state.animation.montage &&
                 current_animation.montage == state.animation.montage &&
                 ue::SetActiveMontagePosition(state.actor, state.animation))
            restored |= Animation;
        else
            unsupported |= Animation;
    }

    const std::uint32_t missing = kAllCaptureFields & ~state.fields;
    if (failed || unsupported || missing) {
        ++*partial_count;
        SC_LOG("tas: checkpoint restore actor[%d] '%s' partial available=%03X restored=%03X "
               "failed=%03X unsupported=%03X unavailable=%03X",
               index, state.identity, state.fields, restored, failed, unsupported, missing);
    } else {
        ++*restored_count;
        SC_LOG("tas: checkpoint restore actor[%d] '%s' restored fields=%03X",
               index, state.identity, restored);
    }
    if (diagnostic) {
        diagnostic->restored_fields = restored;
        diagnostic->failed_fields = failed;
        diagnostic->unsupported_fields = unsupported;
        diagnostic->unavailable_fields = missing;
    }
}

void SaveCheckpoint() {
    Snapshot snapshot = {};
    char level[512] = {};
    const char* error = "unknown validation failure";
    if (!Capture(&snapshot, level, sizeof(level), &error)) {
        SC_LOG("tas: checkpoint save rejected level='%s'", level);
        SetResult("save refused", error);
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = snapshot;
    g_status.exists = true;
    g_status.incomplete = snapshot.incomplete;
    g_status.camera_available = snapshot.camera_available;
    g_status.camera_restored = false;
    g_status.reference = snapshot.reference;
    g_status.enumerated_count = snapshot.enumerated_count;
    g_status.captured_count = snapshot.character_count;
    g_status.restored_count = 0;
    g_status.skipped_count = 0;
    g_status.partial_count = 0;
    g_status.cloth_reset_count = 0;
    g_status.world_actor_count = snapshot.world_actor_count;
    g_status.pooled_count = snapshot.pooled_count;
    g_status.invalid_count = snapshot.invalid_count;
    g_status.missing_health_count = snapshot.missing_health_count;
    g_status.inactive_count = snapshot.inactive_count;
    g_status.identity_truncated_count = snapshot.identity_truncated_count;
    g_status.identity_unavailable_count = snapshot.identity_unavailable_count;
    g_status.capacity_skipped_count = snapshot.capacity_skipped_count;
    g_status.actor_count = snapshot.character_count;
    for (int i = 0; i < snapshot.character_count; ++i) {
        ActorDiagnostic& actor = g_status.actors[i];
        actor = {};
        lstrcpynA(actor.identity, snapshot.characters[i].identity, sizeof(actor.identity));
        actor.captured_fields = snapshot.characters[i].fields;
        actor.player = snapshot.characters[i].player;
    }
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                snapshot.incomplete ? "Saved partial checkpoint - %d / %d active actors"
                                    : "Checkpoint saved - %d actors",
                snapshot.character_count, snapshot.enumerated_count);
    ReleaseSRWLockExclusive(&g_lock);

    SC_LOG("tas: checkpoint saved level='%s' tick=%llu characters=%d discovered=%d "
           "world_actors=%d pooled=%d invalid=%d missing_health=%d inactive=%d "
           "identity_truncated=%d identity_unavailable=%d capacity_skipped=%d "
           "capacity=%d incomplete=%u camera=%u snapshot_bytes=%llu", snapshot.reference.level_path,
           static_cast<unsigned long long>(snapshot.reference.engine_tick),
           snapshot.character_count, snapshot.enumerated_count, snapshot.world_actor_count,
           snapshot.pooled_count, snapshot.invalid_count, snapshot.missing_health_count,
           snapshot.inactive_count, snapshot.identity_truncated_count,
           snapshot.identity_unavailable_count, snapshot.capacity_skipped_count, kMaxCharacters,
           snapshot.incomplete ? 1u : 0u, snapshot.camera_available ? 1u : 0u,
           static_cast<unsigned long long>(sizeof(Snapshot)));
    if (snapshot.camera_available) LogCameraState("capture", snapshot.camera);
    for (int i = 0; i < snapshot.character_count; ++i) {
        const ActorState& state = snapshot.characters[i];
        SC_LOG("tas: checkpoint capture actor[%d] role=%s path='%s' class='%s' "
               "fields=%03X hp=%.1f/%.1f guard=%.1f down=%u dead=%u dilation=%.3f "
               "montage=%u montage_pos=%.3f pos_valid=%u velocity=%.1f,%.1f,%.1f",
               i, state.player ? "player" : "character", state.identity, state.class_path,
               state.fields, state.health, state.max_health, state.guard, state.down ? 1u : 0u,
               state.dead ? 1u : 0u, state.time_dilation,
               state.animation.montage ? 1u : 0u, state.animation.position,
               state.animation.position_valid ? 1u : 0u, state.velocity.X,
               state.velocity.Y, state.velocity.Z);
    }
}

void InvalidateSnapshot(const char* error) {
    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = {};
    g_status.exists = false;
    g_status.incomplete = false;
    g_status.camera_available = false;
    g_status.camera_restored = false;
    g_status.reference = {};
    g_status.enumerated_count = 0;
    g_status.captured_count = 0;
    g_status.restored_count = 0;
    g_status.skipped_count = 0;
    g_status.partial_count = 0;
    g_status.cloth_reset_count = 0;
    g_status.world_actor_count = 0;
    g_status.pooled_count = 0;
    g_status.invalid_count = 0;
    g_status.missing_health_count = 0;
    g_status.inactive_count = 0;
    g_status.identity_truncated_count = 0;
    g_status.identity_unavailable_count = 0;
    g_status.capacity_skipped_count = 0;
    g_status.actor_count = 0;
    for (auto& actor : g_status.actors) actor = {};
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "Load invalidated: %s", error);
    ReleaseSRWLockExclusive(&g_lock);
    SC_LOG("tas: checkpoint invalidated: %s", error);
}

void LoadCheckpoint() {
    Snapshot snapshot = {};
    AcquireSRWLockShared(&g_lock);
    snapshot = g_snapshot;
    ReleaseSRWLockShared(&g_lock);

    if (!hooks::IsPaused()) {
        SetResult("load refused", "pause the simulation before loading (F1)");
        return;
    }
    if (!snapshot.valid) {
        SetResult("load refused", "no checkpoint is saved");
        return;
    }
    ue::UObject* world = ue::GetWorld();
    if (world != snapshot.world || !ue::IsAliveObject(world)) {
        InvalidateSnapshot("world instance changed");
        return;
    }
    char level[512] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level))) {
        SetResult("load refused", "current world level identity could not be read");
        return;
    }
    if (strcmp(level, snapshot.reference.level_path) != 0) {
        InvalidateSnapshot("level identity changed");
        return;
    }
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    const char* gameplay_error = nullptr;
    if (!game::CheckpointGameplayState(world, player, &gameplay_error)) {
        SetResult("load refused", gameplay_error ? gameplay_error
                                                   : "player/game state unavailable");
        return;
    }
    if (!hooks::IsPaused()) {
        SetResult("load refused", "pause the simulation before loading (F1)");
        return;
    }

    int restored_count = 0;
    int partial_count = 0;
    int skipped_count = 0;
    int cloth_reset_count = 0;
    ActorDiagnostic actor_diagnostics[kMaxCharacters] = {};
    bool camera_restored = false;
    ue::UObject* current_fighters[256] = {};
    int current_fighter_count = 0;
    int current_fighter_total = 0;
    bool needs_fighter_enumeration = false;
    for (int i = 0; i < snapshot.character_count; ++i)
        if (!snapshot.characters[i].player) needs_fighter_enumeration = true;
    const bool fighters_available = !needs_fighter_enumeration ||
        game::EnumerateWorldFighters(current_fighters, kEnumerationBuffer,
                                     &current_fighter_count, &current_fighter_total);
    SC_LOG("tas: checkpoint load fighter enumeration available=%u count=%d total=%d",
           fighters_available ? 1u : 0u, current_fighter_count, current_fighter_total);
    SC_LOG("tas: checkpoint load requested tick=%llu characters=%d discovered=%d incomplete=%u",
           static_cast<unsigned long long>(hooks::FrameCount()), snapshot.character_count,
           snapshot.enumerated_count, snapshot.incomplete ? 1u : 0u);
    ue::PlayerCameraState before_camera = {};
    const bool have_before_camera = snapshot.camera_available &&
                                    ue::ReadPlayerCameraState(world, &before_camera);
    if (have_before_camera) LogCameraState("before_load", before_camera);
    for (int i = 0; i < snapshot.character_count; ++i) {
        RestoreActor(snapshot, snapshot.characters[i], i, &restored_count,
                     &partial_count, &skipped_count, &cloth_reset_count,
                     current_fighters, current_fighter_count, fighters_available,
                     &actor_diagnostics[i]);
    }

    if (snapshot.camera_available) {
        camera_restored = have_before_camera &&
                          before_camera.controller == snapshot.camera.controller &&
                          before_camera.manager == snapshot.camera.manager &&
                          ue::RestorePlayerCameraState(snapshot.camera);
        ue::PlayerCameraState after_camera = {};
        if (ue::ReadPlayerCameraState(world, &after_camera))
            LogCameraState("after_load", after_camera);
        SC_LOG("tas: checkpoint camera restore=%s",
               camera_restored ? "verified" : "failed");
    }

    AcquireSRWLockExclusive(&g_lock);
    g_status.restored_count = restored_count;
    g_status.partial_count = partial_count;
    g_status.skipped_count = skipped_count;
    g_status.cloth_reset_count = cloth_reset_count;
    g_status.actor_count = snapshot.character_count;
    for (int i = 0; i < snapshot.character_count; ++i) {
        g_status.actors[i] = actor_diagnostics[i];
        g_status.actors[i].captured_fields = snapshot.characters[i].fields;
    }
    g_status.camera_available = snapshot.camera_available;
    g_status.camera_restored = camera_restored;
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "Partial restore - %d actors; combat state remains live",
                restored_count + partial_count);
    ReleaseSRWLockExclusive(&g_lock);
    if (camera_restored) {
        g_camera_diagnostics_pending = true;
        g_camera_diagnostic_frame = 0;
    }
    SC_LOG("tas: checkpoint load finished restored=%d partial=%d skipped=%d "
           "camera=%s cloth_reset=%d snapshot_incomplete=%u; AI/combat actions remain live",
           restored_count, partial_count, skipped_count,
           camera_restored ? "restored" : "unavailable", cloth_reset_count,
           snapshot.incomplete ? 1u : 0u);
}

}

void QueueSave() { g_requests.fetch_or(kSaveRequest, std::memory_order_relaxed); }

void QueueLoad() { g_requests.fetch_or(kLoadRequest, std::memory_order_relaxed); }

void ProcessRequests() {
    const std::uint32_t requests = g_requests.exchange(0, std::memory_order_relaxed);
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER start = {};
    LARGE_INTEGER end = {};
    QueryPerformanceFrequency(&frequency);
    if (requests & kSaveRequest) {
        QueryPerformanceCounter(&start);
        SaveCheckpoint();
        QueryPerformanceCounter(&end);
        if (frequency.QuadPart)
            SC_LOG("tas: checkpoint perf operation=save elapsed_ms=%.3f",
                   1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                       static_cast<double>(frequency.QuadPart));
    }
    if (requests & kLoadRequest) {
        QueryPerformanceCounter(&start);
        LoadCheckpoint();
        QueryPerformanceCounter(&end);
        if (frequency.QuadPart)
            SC_LOG("tas: checkpoint perf operation=load elapsed_ms=%.3f",
                   1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                       static_cast<double>(frequency.QuadPart));
    }
}

void TickCameraDiagnostics() {
    if (!g_camera_diagnostics_pending || hooks::IsPaused()) return;
    ue::UObject* world = ue::GetWorld();
    ue::PlayerCameraState state = {};
    if (!world || !ue::ReadPlayerCameraState(world, &state)) {
        SC_LOG("tas: checkpoint camera stage=postresume_%u unavailable",
               g_camera_diagnostic_frame + 1);
        g_camera_diagnostics_pending = false;
        return;
    }
    ++g_camera_diagnostic_frame;
    char stage[32] = {};
    _snprintf_s(stage, sizeof(stage), _TRUNCATE, "postresume_%u",
                g_camera_diagnostic_frame);
    LogCameraState(stage, state);
    if (g_camera_diagnostic_frame >= 6) g_camera_diagnostics_pending = false;
}

Status GetStatus() {
    AcquireSRWLockShared(&g_lock);
    Status status = g_status;
    status.exists = g_snapshot.valid;
    ReleaseSRWLockShared(&g_lock);
    return status;
}

}
