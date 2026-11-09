# SifuTAS savestate feasibility

> Archived research. Since 2026-11-09, SifuTAS is scoped as a time-control mod. This document is retained for historical reference; savestates are not an active feature or development objective.

Status: arbitrary-frame process rollback was not demonstrated by the independent Windows PSS experiment. The current architecture does not have a safe path to restore all process threads and external graphics/system state. Stop expanding the actor snapshot; it is a legacy diagnostic feature, not a gameplay savestate. See [the process rollback feasibility report](PROCESS_ROLLBACK_FEASIBILITY.md). Sifu must be launched and controlled only by the user.

## Why current snapshots are incomplete

`src/tas/checkpoint.cpp` records selected player and `AFightingCharacter` fields, verifies writes, and can restore transforms/vitals. It does **not** restore attack/defense orders, AI/behavior trees, animation graph state, pending timers and callbacks, level-script progression, spawning/despawning, RNG, or the state of the camera rig and physics simulation. “Supported fields restored” does not imply a coherent rollback. Adding arbitrary offsets to this snapshot will not solve those dependencies.

### Experimental enemy rebinding

Capture enumerates current-world `AAISpawner` objects using the existing enemy tracker and calls the reflected zero-argument `AAISpawner::BPF_GetSpawnedAI()` getter. An enemy stores the spawner's full path and class only when exactly one spawner returns that actor; unavailable, truncated, missing, or ambiguous associations are logged and mark the snapshot incomplete. Capacity is 256 spawners, so the 57-object Free Training inventory observed in the runtime log fits without truncation.

Load tries exact actor path/class and current-world fighter membership first. If that fails, it matches one current-world spawner by saved full path and class, reads its current spawned actor, then checks fighter membership, saved enemy class, active/non-pooled status, living/standing state, idle montage state, and freshly resolved attack/defense/health components. It refuses ambiguous or missing associations. Transform, velocity, health, guard, time dilation, animation checks, and cloth reset use the resolved target. No saved component pointers are reused. This is limited to the same world instance and level; map restarts invalidate the checkpoint. Successful rebinding restores only existing selected fields and does not prove AI, pending orders, timers, or encounter progression were restored.

The PDB gives `UOrderComponent::BPF_CancelAllOrders()` the native signature `void()` and its body forwards to `OrderManager::CancelAllOrdersAsynch`. That queues cancellation work; it is not a verified immediate idle reset, and its interactions with animation transitions, AI, callbacks, networking, and a paused world are unknown. `BPF_GetRunningAndPendingActionOrders` returns a reflected byte array and takes a boolean parameter; safely consuming/freeing that Unreal array and interpreting its IDs is not established. `UAIFightingComponent::BPF_ResetComboToDefault` takes an `FName` difficulty argument and changes combo selection, not proven active action state. Brain `StopLogic`/`RestartLogic` pause and restart behavior-tree logic and can strand or recreate task state. SifuTAS does not invoke these methods; there is no idle-state cancellation mode until effects and a reliable idle-order predicate are verified.

## Relevant precedents

- **libTAS:** process-level savestates suspend threads and preserve memory/registers/related OS state. This is not straightforward for a Windows UE4.26 game with D3D11 driver/GPU state; do not attempt memory-region rollback in the loaded Sifu process without a separate feasibility study. <https://clementgallet.github.io/libTAS/guides/how/> and <https://clementgallet.github.io/libTAS/faq/>
- **UE4.27 Level Snapshots:** can compare/reconcile actor transforms, properties, components, added/removed actors. It is presented as an editor-facing tool and is one version newer than Sifu's UE4.26.2 build; it is **not** a directly usable packaged-game savestate API. Borrow concepts, not its deployment assumptions. <https://dev.epicgames.com/documentation/en-us/unreal-engine/level-snapshot?application_version=4.27>
- **Unreal save plugins:** NVSaveLoad claims to capture actor/component properties and selected internal component state, while documenting exceptions. These are project-integrated systems, not drop-in DLL injections. <https://www.fab.com/listings/75b64b1c-43de-449b-a3cc-abb63385f656>
- **UE4SS:** provides live property inspection, SDK dumps, and object exploration for reverse engineering. Prefer implementing a small read-only inspector in our existing native DLL rather than injecting an additional modding framework into Sifu. <https://docs.ue4ss.com/>
- **Sifu's existing disk-backup quicksave mod** only backs up ordinary save files and requires leaving/restarting the game. It cannot provide live combat rollback. <https://www.nexusmods.com/sifu/mods/10>

## Proposed architecture: reset, stabilize, restore, validate

This section records the earlier reset-based investigation and is historical only. The process rollback feasibility result supersedes it as a development plan. Do not continue this route or add actor fields unless the project scope is explicitly reconsidered after a new architecture passes independent rollback tests.

### 1. Find the real reset entry point

Current `src/tas/reset_trace.cpp` observes `AGameModeBase::Reset/ResetLevel`, `AActor::Reset/K2_OnReset`, and `UWGGameFlow::BPF_RestartCurrentMap`. The user should capture a baseline, arm the trace, close the GUI, press the configurable marker immediately before Reset Situation, then reopen the GUI and compare. Native reset hooks are also monitored.

If none fire, do **not** infer that resetting does not exist. Advanced/Diagnostics now arms a two-second trace that waits indefinitely for a trigger: it keeps the most recent 512 ProcessEvent records, then records up to 1536 calls after a reset boundary. Excess calls are aggregated by function/class pair (up to 256 pairs with first/last QPC and count); overflow is reported. The current input hooks cannot map the Reset Situation key reliably, so use the configurable Ctrl+Alt+F12 marker immediately before manually activating Reset Situation; native reset hooks are also monitored. Function/class names are resolved and logged after capture. The hook does no path conversion or disk I/O and does not modify ProcessEvent arguments.

### 2. Measure actor lifecycle across reset

Before and after a user-triggered reset, take a **read-only** current-world inventory: class/name, UObject identity (object index/serial when available, not just raw pointers), transform, health/guard, active/pooled status, montage, component pointers and optionally game-state/encounter actor identity. Compare actors added, removed, recycled, moved, or reinitialized. Avoid dereferencing an actor pointer after a reset without revalidating it.

Determine whether Reset Situation:
- Mutates actors in place.
- Recycles pooled actors.
- Destroys/recreates actors while retaining the same world.
- Travels to another world.
- Clears AI orders and timers and reaches a stable post-init boundary.

A reset-begin event alone is **not** a safe point to reapply a checkpoint. Three consecutive matching registry samples are reported only as a stable observed inventory. Component availability and observed reset begin/return events are tracked separately; no signal currently proves reset initialization or pending callbacks have finished.

### 3. Inspect reflected combat state (read-only first)

The immediate targets are player/enemy `UAttackComponent`, `UDefenseComponent`, relevant order components, `UAIFightingComponent`/AI brain or behavior-tree state, character movement, `UAnimInstance`, the situation director/spawner, and level/game state.

Investigate UE4.26 reflected `FProperty` metadata for each object: property name, owner class, type, offset, flags and scalar value (where safely readable). The PDB can provide member layouts for native fields that are not reflected. Gather **before reset / after reset / after an attack / after checkpoint load** comparisons to discover the few state variables that actually drive behavior.

Never blindly memcpy an arbitrary UObject, duplicate `TArray` ownership, restore virtual tables, overwrite raw pointers, or write reflected object fields without analyzing property semantics. A property dump is not a serializer. Start with typed primitive read-only probes and a small explicit write whitelist, gated to known game-thread and actor lifecycle conditions.

### 4. Implement a separate experimental reset-and-reapply checkpoint

Preserve the existing in-place field-only checkpoint as a diagnostic control and fallback; don't silently replace it.

After identifying the native game reset path and a safe completion boundary, create a separate opt-in workflow:

`saved snapshot -> request observed game reset method -> wait for stable world/actor lifecycle -> match fresh actors to saved logical identities -> restore supported fields -> resume AI -> verify after subsequent ticks`

Match actors using spawn source/class plus stable actor identity or a deterministic per-encounter mapping, **not** stale UObject pointers or arrival order. Preserve current guards against game-over, world travel, and unsupported actor state. Do not attempt arbitrary mid-attack reconstruction in this stage. If the only available reset reloads the entire map, explicitly scope the prototype to the same encounter after reload and evaluate UI/loading implications.

### 5. Acceptance criteria

A convincing prototype should pass repeated manual tests:

1. Free Training: save at idle, fight, reset-and-reapply, resume with normal attacks/AI; repeat without state drift.
2. Rain arena: save before combat, defeat/advance part of a wave, restore earlier state **including encounter/wave progression**; confirm expected enemies spawn/act, not merely teleport.
3. Confirm identity reconciliation when a pooled actor changes or an enemy is replaced.
4. Inspect health, structure, animation/action state, camera, and encounter state **on the first few resumed frames**.
5. Refuse during death/game-over or level transition without a softlock; manual death-screen recovery belongs to Sifu.
6. Compare timing/memory logs before/after operations. Mark unimplemented fields and failure modes accurately.

Do not claim arbitrary-frame savestates, determinism, or gameplay replay from passing only transform/vitals tests.

## Next manual observation

- Capture and review a manual Free Training Reset Situation trace before selecting a reset or order-cancellation API.
- Use the new bounded trace and prioritized inventory to identify the actual reset call chain and compare the post-reset observation.
- Review detailed logs while keeping the default GUI status concise.
- Keep the reset and input hooks observational, bounded, and free of gameplay-state changes.

## Steam PDB reset candidates (discovery only)

The local Steam UE4.26.2 PDB contains no `ResetSituation`, `ResetTraining`, or `ResetEncounter` method. A name search for reset/situation/training found only `FWuguanAIManager::ResetJokerSituations` (RVA `0x0172E940`), which is specifically scoped to joker situations and is not evidence for Free Training's command. Existing observations cover standard `AGameModeBase::Reset`/`ResetLevel`, `AActor::Reset`/`K2_OnReset`, and `UWGGameFlow::BPF_RestartCurrentMap`; the last is a map-restart function with no evidence that it performs an in-place encounter reset.

PDB candidate APIs include `AAISituationActor::AreAllSpawnerGroupsCleared`, `GetSpawnersUsed`, `BPF_GetAis`, `BPF_SpawnSituation`, and `BPF_UnSpawnSituation`; `AAISpawner::BPF_AskForRespawn`, `BPF_GetSpawnedAI`, and spawn/despawn callbacks; `AAIWaveRefillDirector::Initialize`, `CancelCurrentWave`, `BPF_StartNextWave`, `BPF_StartWave`, `BPF_IsWaveInProgress`, `BPF_GetAIRemainingInCurrentWave`, and wave-completion callbacks; and `AArenaGameMode::BPF_FlagArenaForRestart`. These symbols indicate separate arena-wave progression and situation/spawner lifecycles but do not prove that any is the Free Training reset path or a safe reset operation.

For combat orders, `UOrderComponent` exposes `BPF_GetRunningAndPendingActionOrders`, `BPF_CancelAllOrders`, `CancelOrderByID/Type`, `FreezePendingOrders`, `InternalInitialize`, and `InternalUninitialize`. `UBrainComponent::StopLogic/RestartLogic` and `UAIFightingComponent::BPF_ResetComboToDefault` / `BPF_ResetAvoidAbilityToDefault` also exist. They are discovery candidates only: cancellation, brain restart, and combo reset do not establish restoration of animation, timers, situation progress, physics, or callbacks. Do not invoke them until runtime trace and signature/side-effect review establish their role.

## Implemented discovery diagnostics (runtime test pending)

- The Savestates tab's Advanced / diagnostics section arms a 512-record rolling pre-trigger buffer and waits indefinitely. A native reset boundary or configurable Ctrl+Alt+F12 marker begins up to two seconds of capture: 1536 detailed calls plus aggregates for up to 256 distinct function/class pairs. Records include event order, QPC timestamp, thread ID, owner/class pointers and `UFunction` pointer. Pre-ring overwrite, event aggregation, and untracked overflow are reported separately. No names are resolved or logged in the hot hook.
- The same section provides the manual sequence: capture baseline, arm trace, close GUI, press the configured marker, activate Reset Situation, reopen GUI, compare. Three unchanged game-thread samples establish only a stable observed world/actor path/class/address inventory. Component availability and reset-hook begin/return are separate indicators; pending callback completion remains unknown. Fighter enumeration checks up to 256 entries, prioritizes up to 64 active candidates, and separately samples inactive and pooled actors with explicit truncation. It also inventories `AAISituationActor` and `AAISpawner` instances.
- Scalar metadata was resolved from the locally installed Steam UE4.26.2 executable/PDB UHT property tables. The first sample fields are `UAttackComponent.m_fDangerTimeOut` and `m_eDangerState`, `UAIFightingComponent.m_iCurrentPhaseNodeIndex` / `m_iLastPhaseNodeIndex`, `UCharacterMovementComponent.MaxWalkSpeed`, `USCAnimInstance.m_fCinematicOverallWeight`, `AAISituationActor.m_iCurrentLastManOccuranceCount`, and spawner respawn/spawn delay and kill-counter configuration. Config values are not assumed to represent live encounter progress. Arrays, maps, bool bitfields, and UObject references are not read by the generic scalar probes.
- Actor comparison pairs by class path plus object path and separately reports pointer changes. It can show observable transform/vitals/selected-state differences and instance-address changes. It does not have a verified FUObjectArray serial identity, does not prove that same-address actors have the same lifecycle, and does not establish equality of hidden AI, timer, RNG, or physics state.
- Reset/reapply remains disabled. The trace must first reveal the actual Reset Situation call chain and a stable post-reset boundary; runtime observations must then confirm which situation/spawner/AI fields represent encounter progression. Current inventory is diagnostic and does not issue resets or mutate state.

Build and unit-test on the user's local Windows environment; keep changes on `testing`. Do not deploy while Sifu is running, and never launch or control Sifu from Codex.
