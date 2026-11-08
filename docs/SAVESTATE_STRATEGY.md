# Toward coherent SifuTAS savestates

Status: reset discovery diagnostics are implemented on `testing`, but no reset-based restoration or full gameplay savestate has been demonstrated. Sifu must be launched and controlled only by the user.

## Why current snapshots are incomplete

`src/tas/checkpoint.cpp` records selected player and `AFightingCharacter` fields, verifies writes, and can restore transforms/vitals. It does **not** restore attack/defense orders, AI/behavior trees, animation graph state, pending timers and callbacks, level-script progression, spawning/despawning, RNG, or the state of the camera rig and physics simulation. “Supported fields restored” does not imply a coherent rollback. Adding arbitrary offsets to this snapshot will not solve those dependencies.

## Relevant precedents

- **libTAS:** process-level savestates suspend threads and preserve memory/registers/related OS state. This is not straightforward for a Windows UE4.26 game with D3D11 driver/GPU state; do not attempt memory-region rollback in the loaded Sifu process without a separate feasibility study. <https://clementgallet.github.io/libTAS/guides/how/> and <https://clementgallet.github.io/libTAS/faq/>
- **UE4.27 Level Snapshots:** can compare/reconcile actor transforms, properties, components, added/removed actors. It is presented as an editor-facing tool and is one version newer than Sifu's UE4.26.2 build; it is **not** a directly usable packaged-game savestate API. Borrow concepts, not its deployment assumptions. <https://dev.epicgames.com/documentation/en-us/unreal-engine/level-snapshot?application_version=4.27>
- **Unreal save plugins:** NVSaveLoad claims to capture actor/component properties and selected internal component state, while documenting exceptions. These are project-integrated systems, not drop-in DLL injections. <https://www.fab.com/listings/75b64b1c-43de-449b-a3cc-abb63385f656>
- **UE4SS:** provides live property inspection, SDK dumps, and object exploration for reverse engineering. Prefer implementing a small read-only inspector in our existing native DLL rather than injecting an additional modding framework into Sifu. <https://docs.ue4ss.com/>
- **Sifu's existing disk-backup quicksave mod** only backs up ordinary save files and requires leaving/restarting the game. It cannot provide live combat rollback. <https://www.nexusmods.com/sifu/mods/10>

## Proposed architecture: reset, stabilize, restore, validate

### 1. Find the real reset entry point

Current `src/tas/reset_trace.cpp` observes `AGameModeBase::Reset/ResetLevel`, `AActor::Reset/K2_OnReset`, and `UWGGameFlow::BPF_RestartCurrentMap`. The **user** should manually press Free Training's Reset Situation key and capture the resulting calls.

If none fire, do **not** infer that resetting does not exist. Build an opt-in, bounded `UObject::ProcessEvent` trace for a very short window around the user's reset input. Record only function pointer, owner/class pointer, timing/order, and thread in the hot hook; resolve names **after** capture using the existing engine path-name helper. Filter/rank functions associated with `Training`, `Situation`, `Reset`, `Arena`, `Spawner`, `AI`, and `GameFlow`, but retain a bounded unfiltered trace for cases where the reset function has an unexpected name. Do not log every `ProcessEvent` synchronously or change its arguments. Resolve relevant functions through Sifu's existing reflection and PDB tooling; protect against recursion, overflow and stale pointers.

### 2. Measure actor lifecycle across reset

Before and after a user-triggered reset, take a **read-only** current-world inventory: class/name, UObject identity (object index/serial when available, not just raw pointers), transform, health/guard, active/pooled status, montage, component pointers and optionally game-state/encounter actor identity. Compare actors added, removed, recycled, moved, or reinitialized. Avoid dereferencing an actor pointer after a reset without revalidating it.

Determine whether Reset Situation:
- Mutates actors in place.
- Recycles pooled actors.
- Destroys/recreates actors while retaining the same world.
- Travels to another world.
- Clears AI orders and timers and reaches a stable post-init boundary.

A reset-begin event alone is **not** a safe point to reapply a checkpoint. Require a verified completion boundary plus stable actor registry/component initialization over subsequent game-thread updates.

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

## Work that can be batched before the next user test

- Analyze Sifu PDB for combat/AI/situation component types and supported property APIs.
- Implement the bounded read-only `ProcessEvent` capture and a pre/post-reset actor inventory; keep disabled unless armed manually.
- Add concise status and diagnostic export (CSV/JSON or log summaries), rather than more default GUI text.
- Keep the current passive reset hooks and input trace isolated, with bounded memory and no changes to gameplay state.
- Make the GUI window slightly translucent without making foreground text hard to read.

## Implemented discovery diagnostics (runtime test pending)

- The Savestates tab's Advanced / diagnostics section can arm a five-second `UObject::ProcessEvent` trace. The hook records at most 2048 calls in a fixed array: event order, QPC timestamp, thread ID, owner address, class address, and `UFunction` address. Overflow is counted. It performs no name conversion or file logging while the hook runs; function/class paths are resolved after capture and emitted in `SifuCoop.log`. The trace is unfiltered during its bounded capture so an unexpected Blueprint reset function is not hidden.
- The same section can capture a read-only baseline, let the user manually trigger Reset Situation, then request a comparison. The comparison waits for three consecutive game-thread samples with the same world, actor paths, UObject addresses and component addresses; it times out after ten seconds. It records current world path/address and match state; fighting actor paths/classes/addresses; transforms, velocity, custom dilation, health/guard, attack/defense/health component addresses, AI/movement/order component addresses, active montage path, and supported scalar fields. It also inventories `AAISituationActor` and `AAISpawner` instances.
- Scalar metadata was resolved from the locally installed Steam UE4.26.2 executable/PDB UHT property tables. The first sample fields are `UAttackComponent.m_fDangerTimeOut` and `m_eDangerState`, `UAIFightingComponent.m_iCurrentPhaseNodeIndex` / `m_iLastPhaseNodeIndex`, `UCharacterMovementComponent.MaxWalkSpeed`, `USCAnimInstance.m_fCinematicOverallWeight`, `AAISituationActor.m_iCurrentLastManOccuranceCount`, and spawner respawn/spawn delay and kill-counter configuration. Config values are not assumed to represent live encounter progress. Arrays, maps, bool bitfields, and UObject references are not read by the generic scalar probes.
- Actor comparison pairs by class path plus object path and separately reports pointer changes. It can show observable transform/vitals/selected-state differences and instance-address changes. It does not have a verified FUObjectArray serial identity, does not prove that same-address actors have the same lifecycle, and does not establish equality of hidden AI, timer, RNG, or physics state.
- Reset/reapply remains disabled. The trace must first reveal the actual Reset Situation call chain and a stable post-reset boundary; runtime observations must then confirm which situation/spawner/AI fields represent encounter progression. Current inventory is diagnostic and does not issue resets or mutate state.

Build and unit-test on the user's local Windows environment; keep changes on `testing`. Do not deploy while Sifu is running, and never launch or control Sifu from Codex.
