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
constexpr float kMaxQuietSpeed = 25.f;
constexpr float kMaxEnemyDistance = 1500.f;

struct ActorState {
    char identity[128] = {};
    char class_path[160] = {};
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
    ue::UObject* world = nullptr;
    ue::UObject* player = nullptr;
    ue::UObject* enemy = nullptr;
    std::uint32_t enemy_hash = 0;
    CheckpointReference reference = {};
    ActorState player_state = {};
    ActorState enemy_state = {};
};

SRWLOCK g_lock = SRWLOCK_INIT;
Snapshot g_snapshot = {};
Status g_status = {};
std::atomic<std::uint32_t> g_requests{0};

bool Finite(const ue::FVector& value) {
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

float SpeedSquared(const ue::FVector& value) {
    return value.X * value.X + value.Y * value.Y + value.Z * value.Z;
}

bool CopyString(char* out, int size, const char* value) {
    if (!out || size <= 0 || !value || strlen(value) >= static_cast<std::size_t>(size))
        return false;
    lstrcpynA(out, value, size);
    return true;
}

bool ReadableFloatField(const ue::UObject* object, std::uint32_t offset) {
    if (!object || !offset) return false;
    const auto* field = reinterpret_cast<const std::uint8_t*>(object) + offset;
    return ue::RangeReadable(field, sizeof(float));
}

void SetResult(const char* operation, const char* message) {
    AcquireSRWLockExclusive(&g_lock);
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "%s: %s", operation, message);
    ReleaseSRWLockExclusive(&g_lock);
    SC_LOG("tas: checkpoint %s: %s", operation, message);
}

bool IsTrainingRoom(char* level, int size) {
    if (!ue::GetCurrentLevelPath(level, size)) return false;
    return strstr(level, "TrainingRoom") != nullptr;
}

bool CaptureActor(ue::UObject* actor, ActorState* out, const char** error) {
    if (!ue::IsAliveObject(actor)) {
        *error = "actor is not a valid live world actor";
        return false;
    }
    if (!ue::GetObjectPathName(actor, out->identity, sizeof(out->identity)) ||
        !ue::GetObjectClassPathName(actor, out->class_path, sizeof(out->class_path))) {
        *error = "actor identity could not be read";
        return false;
    }

    game::Fighter fighter = game::ResolveFighter(actor);
    if (!fighter.health || !fighter.defense || !ue::IsAliveObject(fighter.health) ||
        !ue::IsAliveObject(fighter.defense)) {
        *error = "health or defense component is unavailable";
        return false;
    }
    if (!ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fHealth) ||
        !ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fMaxHealth) ||
        !ReadableFloatField(fighter.defense, offsets::M_UDefenseComponent_fCurrentGuard) ||
        !ReadableFloatField(actor, offsets::M_AActor_CustomTimeDilation)) {
        *error = "health, guard, or custom time-dilation field is unreadable";
        return false;
    }
    if (!ue::GetActorLocation(actor, &out->location) ||
        !ue::GetActorRotation(actor, &out->rotation)) {
        *error = "actor transform could not be read";
        return false;
    }
    const game::PresentationTargets targets = game::ResolvePresentationTargets(actor);
    if (!game::CanSetPresentationVelocity(targets) ||
        !ue::IsAliveObject(targets.movement) || !ue::IsAliveObject(targets.root)) {
        *error = "movement and root components are required for velocity restore";
        return false;
    }
    if (!game::GetActorVelocity(actor, &out->velocity) || !Finite(out->velocity)) {
        *error = "movement velocity could not be read";
        return false;
    }

    out->health = game::GetHealth(fighter);
    out->max_health = game::GetMaxHealth(fighter);
    out->guard = game::GetGuard(fighter);
    out->down = game::IsDown(fighter);
    out->dead = game::IsDead(fighter);
    if (!std::isfinite(out->health) || !std::isfinite(out->max_health) ||
        !std::isfinite(out->guard) || out->health <= 0.5f || out->max_health <= 0.f ||
        out->guard < 0.f) {
        *error = "health or guard values are invalid";
        return false;
    }
    if (out->down || out->dead) {
        *error = "down or dead actors cannot be checkpointed in v1";
        return false;
    }
    if (offsets::M_AActor_CustomTimeDilation == 0) {
        *error = "actor time-dilation offset is unavailable";
        return false;
    }
    out->time_dilation = game::GetActorTimeDilation(actor);
    if (!std::isfinite(out->time_dilation) || out->time_dilation <= 0.01f) {
        *error = "actor time dilation is invalid";
        return false;
    }
    if (!ue::ReadAnimState(actor, &out->animation)) {
        *error = "animation montage state could not be read";
        return false;
    }
    if (out->animation.montage) {
        *error = "active animation montages are not supported; save between attacks";
        return false;
    }
    if (SpeedSquared(out->velocity) > kMaxQuietSpeed * kMaxQuietSpeed) {
        *error = "actor is moving; v1 only saves a quiet encounter";
        return false;
    }
    return true;
}

bool FindNearestEnemy(ue::UObject* player, ue::UObject** actor_out,
                      std::uint32_t* hash_out, const char** error) {
    game::EnemyRow rows[64] = {};
    const int count = game::GetEnemyRows(rows, 64);
    float nearest = kMaxEnemyDistance;
    for (int i = 0; i < count; ++i) {
        const game::EnemyRow& row = rows[i];
        if (!row.active || row.down || row.health <= 0.5f || row.distance < 0.f ||
            row.distance >= nearest || row.hash == 0)
            continue;
        ue::UObject* enemy = game::FindEnemyByHash(row.hash);
        if (!enemy || enemy == player || !ue::IsAliveObject(enemy)) continue;
        nearest = row.distance;
        *actor_out = enemy;
        *hash_out = row.hash;
    }
    if (*actor_out) return true;
    *error = "no nearby active enemy is tracked in this world";
    return false;
}

bool Capture(Snapshot* out, const char** error) {
    if (!hooks::IsPaused()) {
        *error = "pause the simulation before saving (F1)";
        return false;
    }
    if (!game::ActorsReady()) {
        *error = "combat component access is not ready";
        return false;
    }
    ue::UObject* world = ue::GetWorld();
    if (!ue::IsAliveObject(world)) {
        *error = "current world is unavailable";
        return false;
    }
    char level[512] = {};
    if (!IsTrainingRoom(level, sizeof(level))) {
        *error = "checkpoint v1 only supports the training room";
        return false;
    }
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player || !ue::IsAliveObject(player)) {
        *error = "player character is unavailable";
        return false;
    }
    ue::UObject* enemy = nullptr;
    std::uint32_t enemy_hash = 0;
    if (!FindNearestEnemy(player, &enemy, &enemy_hash, error)) return false;

    Snapshot snapshot = {};
    snapshot.world = world;
    snapshot.player = player;
    snapshot.enemy = enemy;
    snapshot.enemy_hash = enemy_hash;
    if (!CopyString(snapshot.reference.level_path,
                    sizeof(snapshot.reference.level_path), level)) {
        *error = "training-room level identity is too long";
        return false;
    }
    if (!CaptureActor(player, &snapshot.player_state, error) ||
        !CaptureActor(enemy, &snapshot.enemy_state, error))
        return false;

    CopyString(snapshot.reference.player_identity,
               sizeof(snapshot.reference.player_identity), snapshot.player_state.identity);
    CopyString(snapshot.reference.enemy_identity,
               sizeof(snapshot.reference.enemy_identity), snapshot.enemy_state.identity);
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

bool ValidateActorIdentity(ue::UObject* actor, const ActorState& state,
                           game::Fighter* fighter_out, const char** error) {
    if (!actor || !ue::IsAliveObject(actor)) {
        *error = "actor failed UObject validation";
        return false;
    }
    char identity[128] = {};
    char class_path[160] = {};
    if (!ue::GetObjectPathName(actor, identity, sizeof(identity)) ||
        !ue::GetObjectClassPathName(actor, class_path, sizeof(class_path)) ||
        strcmp(identity, state.identity) != 0 || strcmp(class_path, state.class_path) != 0) {
        *error = "actor identity or class no longer matches the checkpoint";
        return false;
    }
    game::Fighter fighter = game::ResolveFighter(actor);
    if (!fighter.health || !fighter.defense || !ue::IsAliveObject(fighter.health) ||
        !ue::IsAliveObject(fighter.defense)) {
        *error = "health or defense component is unavailable";
        return false;
    }
    if (!ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fHealth) ||
        !ReadableFloatField(fighter.health, offsets::M_UHealthComponent_fMaxHealth) ||
        !ReadableFloatField(fighter.defense, offsets::M_UDefenseComponent_fCurrentGuard) ||
        !ReadableFloatField(actor, offsets::M_AActor_CustomTimeDilation)) {
        *error = "health, guard, or custom time-dilation field is unreadable";
        return false;
    }
    if (game::IsDown(fighter) || game::IsDead(fighter) || game::GetHealth(fighter) <= 0.5f) {
        *error = "down/dead actors cannot be resurrected by checkpoint v1";
        return false;
    }
    ue::FVector location = {};
    ue::FRotator rotation = {};
    ue::FVector velocity = {};
    ue::AnimState animation = {};
    if (!ue::GetActorLocation(actor, &location) || !ue::GetActorRotation(actor, &rotation) ||
        !ue::ReadAnimState(actor, &animation)) {
        *error = "required transform, movement, or animation component is unavailable";
        return false;
    }
    const game::PresentationTargets targets = game::ResolvePresentationTargets(actor);
    if (!game::CanSetPresentationVelocity(targets) ||
        !ue::IsAliveObject(targets.movement) || !ue::IsAliveObject(targets.root) ||
        !game::GetActorVelocity(actor, &velocity) || !Finite(velocity)) {
        *error = "movement/root components required for velocity restore are unavailable";
        return false;
    }
    *fighter_out = fighter;
    return true;
}

bool ValidateSnapshot(const Snapshot& snapshot, ue::UObject** player_out,
                      ue::UObject** enemy_out, game::Fighter* player_fighter,
                      game::Fighter* enemy_fighter, const char** error) {
    if (!hooks::IsPaused()) {
        *error = "pause the simulation before loading (F1)";
        return false;
    }
    if (!snapshot.valid) {
        *error = "no checkpoint is saved";
        return false;
    }
    ue::UObject* world = ue::GetWorld();
    if (!world || world != snapshot.world || !ue::IsAliveObject(world)) {
        *error = "world instance changed; checkpoint invalidated";
        return false;
    }
    char level[512] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level)) ||
        strcmp(level, snapshot.reference.level_path) != 0) {
        *error = "level identity changed; checkpoint invalidated";
        return false;
    }
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player || player != snapshot.player || !ue::IsAliveObject(player)) {
        *error = "player actor changed; checkpoint invalidated";
        return false;
    }

    game::EnemyRow rows[64] = {};
    const int count = game::GetEnemyRows(rows, 64);
    bool tracked = false;
    for (int i = 0; i < count; ++i) {
        if (rows[i].hash != snapshot.enemy_hash || !rows[i].active) continue;
        ue::UObject* current = game::FindEnemyByHash(snapshot.enemy_hash);
        tracked = current && current == snapshot.enemy && rows[i].hash != 0;
        break;
    }
    if (!tracked) {
        *error = "saved enemy is no longer an active tracked actor; checkpoint invalidated";
        return false;
    }
    if (!ValidateActorIdentity(player, snapshot.player_state, player_fighter, error) ||
        !ValidateActorIdentity(snapshot.enemy, snapshot.enemy_state, enemy_fighter, error))
        return false;
    *player_out = player;
    *enemy_out = snapshot.enemy;
    return true;
}

bool VerifyTransform(ue::UObject* actor, const ActorState& state) {
    ue::FVector location = {};
    ue::FRotator rotation = {};
    return ue::GetActorLocation(actor, &location) && ue::GetActorRotation(actor, &rotation) &&
           SameLocation(location, state.location) && SameRotation(rotation, state.rotation);
}

bool VerifyVelocity(ue::UObject* actor, const ActorState& state) {
    ue::FVector velocity = {};
    return game::GetActorVelocity(actor, &velocity) && SameVector(velocity, state.velocity);
}

bool VerifyVitals(const game::Fighter& fighter, const ActorState& state) {
    return std::fabs(game::GetMaxHealth(fighter) - state.max_health) <= 0.1f &&
           std::fabs(game::GetHealth(fighter) - state.health) <= 0.1f &&
           std::fabs(game::GetGuard(fighter) - state.guard) <= 0.1f &&
           !game::IsDown(fighter) && !game::IsDead(fighter);
}

bool VerifyDilation(ue::UObject* actor, const ActorState& state) {
    return std::fabs(game::GetActorTimeDilation(actor) - state.time_dilation) <= 0.001f;
}

void SaveCheckpoint() {
    Snapshot snapshot = {};
    const char* error = "unknown validation failure";
    if (!Capture(&snapshot, &error)) {
        SetResult("save refused", error);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = snapshot;
    g_status.exists = true;
    g_status.reference = snapshot.reference;
    g_status.player_health = snapshot.player_state.health;
    g_status.player_max_health = snapshot.player_state.max_health;
    g_status.player_guard = snapshot.player_state.guard;
    g_status.enemy_health = snapshot.enemy_state.health;
    g_status.enemy_max_health = snapshot.enemy_state.max_health;
    g_status.enemy_guard = snapshot.enemy_state.guard;
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "Saved quiet checkpoint; AI queues and movement mode are not captured.");
    ReleaseSRWLockExclusive(&g_lock);
    SC_LOG("tas: checkpoint saved level='%s' tick=%llu player='%s' health=%.1f guard=%.1f "
           "dilation=%.3f position=%.1f,%.1f,%.1f velocity=%.1f,%.1f,%.1f "
           "enemy='%s' hash=%08X health=%.1f guard=%.1f dilation=%.3f "
           "position=%.1f,%.1f,%.1f velocity=%.1f,%.1f,%.1f",
           snapshot.reference.level_path,
           static_cast<unsigned long long>(snapshot.reference.engine_tick),
           snapshot.player_state.identity, snapshot.player_state.health,
           snapshot.player_state.guard, snapshot.player_state.time_dilation,
           snapshot.player_state.location.X, snapshot.player_state.location.Y,
           snapshot.player_state.location.Z, snapshot.player_state.velocity.X,
           snapshot.player_state.velocity.Y, snapshot.player_state.velocity.Z,
           snapshot.enemy_state.identity, snapshot.enemy_hash, snapshot.enemy_state.health,
           snapshot.enemy_state.guard, snapshot.enemy_state.time_dilation,
           snapshot.enemy_state.location.X, snapshot.enemy_state.location.Y,
           snapshot.enemy_state.location.Z, snapshot.enemy_state.velocity.X,
           snapshot.enemy_state.velocity.Y, snapshot.enemy_state.velocity.Z);
}

void InvalidateSnapshot(const char* error) {
    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = {};
    g_status.exists = false;
    g_status.reference = {};
    g_status.player_health = 0.f;
    g_status.player_max_health = 0.f;
    g_status.player_guard = 0.f;
    g_status.enemy_health = 0.f;
    g_status.enemy_max_health = 0.f;
    g_status.enemy_guard = 0.f;
    _snprintf_s(g_status.last_result, sizeof(g_status.last_result), _TRUNCATE,
                "Load refused: %s", error);
    ReleaseSRWLockExclusive(&g_lock);
    SC_LOG("tas: checkpoint invalidated: %s", error);
}

void LoadCheckpoint() {
    Snapshot snapshot = {};
    AcquireSRWLockShared(&g_lock);
    snapshot = g_snapshot;
    ReleaseSRWLockShared(&g_lock);

    ue::UObject* player = nullptr;
    ue::UObject* enemy = nullptr;
    game::Fighter player_fighter;
    game::Fighter enemy_fighter;
    const char* error = "unknown validation failure";
    SC_LOG("tas: checkpoint load requested tick=%llu",
           static_cast<unsigned long long>(hooks::FrameCount()));
    if (!ValidateSnapshot(snapshot, &player, &enemy, &player_fighter, &enemy_fighter, &error)) {
        if (strstr(error, "invalidated")) InvalidateSnapshot(error);
        else SetResult("load refused", error);
        return;
    }

    if (!ue::ClearActiveMontage(player)) {
        SetResult("load partial", "player Montage_Stop failed or montage remained active");
        return;
    }
    if (!ue::ClearActiveMontage(enemy)) {
        SetResult("load partial", "enemy Montage_Stop failed or montage remained active");
        return;
    }
    if (!game::TeleportActor(player, snapshot.player_state.location,
                             snapshot.player_state.rotation) ||
        !VerifyTransform(player, snapshot.player_state)) {
        SetResult("load partial", "player transform write or verification failed");
        return;
    }
    if (!game::TeleportActor(enemy, snapshot.enemy_state.location,
                             snapshot.enemy_state.rotation) ||
        !VerifyTransform(enemy, snapshot.enemy_state)) {
        SetResult("load partial", "enemy transform write or verification failed");
        return;
    }
    if (!game::SetPresentationVelocity(player, snapshot.player_state.velocity) ||
        !VerifyVelocity(player, snapshot.player_state)) {
        SetResult("load partial", "player velocity write or verification failed");
        return;
    }
    if (!game::SetPresentationVelocity(enemy, snapshot.enemy_state.velocity) ||
        !VerifyVelocity(enemy, snapshot.enemy_state)) {
        SetResult("load partial", "enemy velocity write or verification failed");
        return;
    }

    game::SetMaxHealth(player_fighter, snapshot.player_state.max_health);
    game::SetHealth(player_fighter, snapshot.player_state.health);
    game::SetGuard(player_fighter, snapshot.player_state.guard);
    if (!VerifyVitals(player_fighter, snapshot.player_state)) {
        SetResult("load partial", "player health/max-health/guard write verification failed");
        return;
    }
    game::SetMaxHealth(enemy_fighter, snapshot.enemy_state.max_health);
    game::SetHealth(enemy_fighter, snapshot.enemy_state.health);
    game::SetGuard(enemy_fighter, snapshot.enemy_state.guard);
    if (!VerifyVitals(enemy_fighter, snapshot.enemy_state)) {
        SetResult("load partial", "enemy health/max-health/guard write verification failed");
        return;
    }
    if (!game::SetActorTimeDilation(player, snapshot.player_state.time_dilation) ||
        !VerifyDilation(player, snapshot.player_state)) {
        SetResult("load partial", "player custom time-dilation write verification failed");
        return;
    }
    if (!game::SetActorTimeDilation(enemy, snapshot.enemy_state.time_dilation) ||
        !VerifyDilation(enemy, snapshot.enemy_state)) {
        SetResult("load partial", "enemy custom time-dilation write verification failed");
        return;
    }

    SetResult("load verified", "supported fields restored; still paused; AI queues are not restored");
    SC_LOG("tas: checkpoint post-restore verified player transform/velocity/vitals/dilation; "
           "enemy transform/velocity/vitals/dilation; both alive and standing");
}

}

void QueueSave() { g_requests.fetch_or(kSaveRequest, std::memory_order_relaxed); }

void QueueLoad() { g_requests.fetch_or(kLoadRequest, std::memory_order_relaxed); }

void ProcessRequests() {
    const std::uint32_t requests = g_requests.exchange(0, std::memory_order_relaxed);
    if (requests & kSaveRequest) SaveCheckpoint();
    if (requests & kLoadRequest) LoadCheckpoint();
}

Status GetStatus() {
    AcquireSRWLockShared(&g_lock);
    Status status = g_status;
    status.exists = g_snapshot.valid;
    ReleaseSRWLockShared(&g_lock);
    return status;
}

}
