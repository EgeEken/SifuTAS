# Development scope and architecture

## Objective

Create tool-assisted **Sifu combat choreography videos** with input precision beyond human play. Reproducibility is desirable for editing, but no competitive run validation is required. Slow motion and frame stepping first; prioritize usable checkpoints and savestates once the engine behavior is understood.

## Existing infrastructure

- `src/dllmain.cpp`: SifuCoop-derived `dsound.dll` bootstrapping and executable fingerprint safeguards.
- `src/proxy/dsound_proxy.cpp`: native Windows proxy loading.
- `src/core/hooks.cpp`: `UGameEngine::Tick` vtable hook, `AWorldSettings::GetEffectiveTimeDilation` multiplier hook, focused TAS hotkeys and `UWorld::Tick` pause/step hook.
- `src/ue/reflection.{h,cpp}`: Unreal object lookup, reflected calls and console command dispatch.
- `tools/pdbdump/pdbdump.py`: resolves function RVAs and field offsets from a matching Sifu PDB; regenerates `src/core/offsets.g.h` and local build fingerprints.
- `src/ui/d3d_overlay.cpp`: retained SifuCoop ImGui/Direct3D 11 integration hosting the dedicated TAS interface; the inherited multiplayer menu remains disabled.

## Immediate milestones

1. **Validate this pass in-game**: check the unsupported-state TAS guard through death/game-over, menus and loading; test the F10 replay recording probe; then check camera framing while paused, cloth reset after teleport, binding-key consumption, montage handling, and profiling output.
2. **Validate the dedicated SifuTAS GUI and settings**: mouse handling, tab behavior, configurable controls and settings persistence were previously reported working. Recheck for regressions from scaling and slider changes.
3. **Validate slowdown** on a training encounter. The recent extended diagnostic session did not reproduce acceleration; its late samples show world time stopped advancing and need a new baseline during active play.
4. **Validate pause and frame advance**: the user reports these work. Check for input buffering or multi-world tick differences if later changes affect them.
5. **Capture and inject inputs** at the relevant controller/input processing boundary, preserving press/release edges and analog axes.
6. **Recording/editing**: frame-indexed input timeline, replay from repeatable encounter starts, per-frame state checksum and divergence reporting.
7. **Active-fight savestates**: later reverse engineer behavior-tree/AI state, queued attacks and callbacks, montage/root-motion state, movement and physics, timers, and random streams. Do not treat actor transforms and vitals as a complete arbitrary-frame snapshot.

`src/tas/state_reference.h` keeps checkpoint identity/timeline metadata separate from the future recording system: level path, player/enemy identities, engine tick, optional verified simulation frame, and recording branch ID.

## Experimental state snapshot limits

The experimental state snapshot enumerates up to 256 current-world `AFightingCharacter` candidates and snapshots up to 32 distinct characters plus the player. There is no map-name restriction. The latest Free Training log reports level `/Game/Maps/TU_3/ArenaModeMenu/ArenaMode_Menu` and one active tracked opponent with `living=0`; source review confirms `Tracked::health` is populated by the multiplayer `PublishEnemies()` path, so it remains zero in offline play. The snapshot does not restore combat actions or encounter progression.

Capture records availability for transform, velocity, health/max health, guard, down/death flags, actor time dilation, and montage/playhead without requiring every field or actor to be restorable. It includes moving, downed, distant, and zero-health characters. If actor count exceeds the cap or enumeration fails, the snapshot is marked incomplete; a player-only snapshot is still possible.

Restore checks the same world and level, object pointer/path/class, and current-world membership per actor. Supported fields are written and read back. Missing actors are skipped without invalidating the slot. Transform and velocity are skipped when saved/current life or montage state makes pose restoration unsafe. Health and guard do not resurrect actors or reverse down/death state. Animation playback, movement mode, AI queues, timers, pending combat actions, and callbacks remain unsupported. Per-character logs report available, restored, failed, unsupported, and unavailable field masks. F7/F8 requests run on the game thread, and load leaves simulation paused with the slot available for repeated attempts.

The player's actual view is owned by `APlayerCameraManager`. Steam PDB member lookup resolves the controller's `PlayerCameraManager` and `ControlRotation`, both camera cache entries, and `FCameraCacheEntry::POV` location/rotation/FOV. Capture records current and previous cached POV plus controller rotation. Restore sets controller rotation through its reflected UFunction, writes both view caches with PDB-derived offsets, and verifies their values. It does not disable camera follow or interpolation; the normal manager recomputes the view after the world resumes. Camera rig/spring-arm internal lag state is not captured, so post-resume convergence still needs in-game checking.

After a successful character teleport, the mesh receives `ForceClothNextUpdateTeleportAndReset` by reflection. This queues reset for the next cloth update; it does not restore saved cloth particles or guarantee an immediate paused-frame visual refresh. If the UFunction is missing on the installed build, the restore log reports `cloth_reset=unavailable`.

Montage playhead capture is distinct from combat-action capture. On load, a montage position is only sought when the exact same montage UObject remains active; it is read back for verification. The implementation does not start a saved montage when it is no longer active, and does not restore montage notifies, attack orders, behavior trees, pending attacks, root motion, or related timers. Available Free Training logs from the reported attack test show `montage=0`; the observed attack continuation therefore was not a captured active montage and likely depends on Sifu combat-order/animation-graph state. Existing order hooks observe or replay selected orders for co-op presentation; their serialized order structures do not provide a safe whole-fight restore surface.

This is not a gameplay savestate or arbitrary-frame restore. A partial restore does not rewind AI, animation, attack actions, encounter progression, movement mode, timers, callbacks, or destroyed actors. Inspect the per-field result while paused before resuming.

## Native replay investigation

The Steam PDB confirms Sifu `UReplaySystem` recording start/stop and eligibility functions, status, current demo time and length, demo-play time dilation, keyframe/time conversion and snapping queries, and replay-management access. `UReplayManagementMenuWidget` exposes stream enumeration, playback, and edit entry points. These are PDB-level API confirmations; runtime behavior and output format are not yet confirmed. The Sifu API exposes Unreal's DemoNetDriver, actor replication-time queries, and an initial-state registration event, suggesting recordings contain a replication stream rather than local input. Which animation/gameplay state is replicated remains unverified. Whether recording timestamps follow TAS-dilated game time or wall time is not established. The stream may reproduce a visual encounter through replicated state and events, but exact arbitrary-time seeking and complete encounter reconstruction are unverified. Native playback dilation is available, although whether it can be set independently during Replay Editor playback needs testing. There is no confirmed supported handoff from replay playback back to the same live world.

An existing opt-in F10 probe (enable `[coop] replay_probe=1` in `SifuCoop.ini`) calls Sifu's recording eligibility and start/stop APIs, then logs whether recording stopped and the current replay length. It does not access the replay data struct because its layout is not established, and cannot resolve a replay ID. The user must inspect the newest recording through the normal Replay Editor. First test a short Free Training recording captured at 0.2× and played at 1.0×; compare event timing and scene continuity. Do not use generic DemoNetDriver APIs as live savestates.

## Deterministic input playback feasibility

The smallest meaningful test is a roughly five-second Free Training segment from a repeatable idle setup. Capture controller input at the earliest stable input boundary with explicit press/release edges and frame/timestamp, then inject the same sequence after restarting the same encounter under a fixed timestep. Hash a small set of observable state each tick: player/enemy transforms, velocities, health/structure, current montage/position, and selected AI/order state. Compare divergence per tick. A first version can measure determinism without claiming to control it. If divergence appears, add RNG stream state and physics/input sampling measurements before building an editable timeline. Current code has neither the input boundary interception nor a global fixed-timestep guarantee; enemy AI, physics, timers and random streams are unmeasured. Therefore deterministic reconstruction is an experiment, not an established savestate strategy.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or arbitrary-frame savestates.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. `OnFrame` profiling uses QPC to log five-second averages/maxima for the hook and major inherited calls. The GUI path separately logs its five-second rendering cost while open. Checkpoint save/load logs operation latency; a fixed snapshot is overwritten in place. Process working set/private usage is sampled once per minute. ImGui frame processing and rendering now run only while the TAS GUI is open. No runtime performance conclusion is possible until the instrumented DLL has been exercised for an extended session.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. The user has standing authorization for the established SifuTAS Steam test DLL path after confirming it is a known project DLL, Sifu is closed, the rollback backup is hash-verified, and the new build hash matches. Stop if the target is missing or unexpected. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add actionable log lines before speculative refactoring.
