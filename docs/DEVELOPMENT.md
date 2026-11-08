# Development scope and architecture

## Objective

Create tool-assisted **Sifu combat choreography videos** with input precision beyond human play. Reproducibility is desirable for editing, but no competitive run validation is required. Savestates are the primary development objective, starting with restoration after Sifu's own Free Training reset. Input recording and native replay are secondary unless they directly contribute to restoration.

## Existing infrastructure

- `src/dllmain.cpp`: SifuCoop-derived `dsound.dll` bootstrapping and executable fingerprint safeguards.
- `src/proxy/dsound_proxy.cpp`: native Windows proxy loading.
- `src/core/hooks.cpp`: `UGameEngine::Tick` vtable hook, `AWorldSettings::GetEffectiveTimeDilation` multiplier hook, focused TAS hotkeys and `UWorld::Tick` pause/step hook.
- `src/ue/reflection.{h,cpp}`: Unreal object lookup, reflected calls and console command dispatch.
- `tools/pdbdump/pdbdump.py`: resolves function RVAs and field offsets from a matching Sifu PDB; regenerates `src/core/offsets.g.h` and local build fingerprints.
- `src/ui/d3d_overlay.cpp`: retained SifuCoop ImGui/Direct3D 11 integration hosting the dedicated TAS interface; the inherited multiplayer menu remains disabled.

## Immediate milestones

1. **Identify Free Training Reset Situation**: observe whether it calls Unreal actor/game-mode reset APIs or Sifu's whole-map restart function. Never invoke these functions from the mod until their semantics are established.
2. **Build reset-based restoration** only after the reset completion signal, actor replacement/identity behavior, and post-reset initialization boundary are known. Apply saved supported fields only to validated initialized actors, then return to a playable paused encounter.
3. **Measure coherent restoration** for one player and one enemy in Free Training, including health/structure, movement/physics, camera, animation, AI, and post-resume combat behavior.
4. Input traces and native replay are secondary. Continue only where they help identify reset behavior, restore state, or verify a checkpoint.
5. Keep established slowdown, pause, and frame stepping stable; the user performs all runtime gameplay tests manually.

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

The Steam PDB confirms Sifu `UReplaySystem` recording APIs. Runtime testing activated recording and Sifu showed a 17-second Wuguan item in Replay Management; the mod's finalization poll timed out, and replay contents/fidelity remain unverified. Treat this path as secondary to checkpoint restoration. There is no confirmed supported handoff from replay playback back to the same live world.

The F6 Recording tab and configurable F10 action call Sifu's native replay APIs from the game thread. The corrected `/Script/Sifu.Default__ReplaySystem` path activated a runtime recording and produced a visible 17-second Replay Management entry. The mod's stop/finalization poll timed out; clean finalization, replay contents, and fidelity remain unverified. This is secondary to savestate work and must not be treated as a state restore mechanism.

TAS controls now require the game's reflected `AGameState::MatchState` field to equal Unreal's `InProgress` value. The matching Steam PDB resolved the field at offset `0x300` and exposes the standard `WaitingPostMatch` state for ended matches. The state is queried through `UGameplayStatics::GetGameState`, without relying on a map-name substring. In-game testing must verify Sifu and Free Training actually use `InProgress` while controllable and leave it on arena victory/results screens.

## Deterministic input playback feasibility

The Steam PDB resolves `APlayerController::InputKey` (RVA `0x0359C360`), `UPlayerInput::InputKey` (`0x037636B0`), `UPlayerInput::InputAxis` (`0x03763030`), `UPlayerInput::ProcessInputStack` (`0x0376A530`), and `APlayerController::PlayerInput` (offset `0x3D8`). An opt-in, fixed-capacity five-second trace is now wired to the Recording tab. It observes controller key events, the local player's `UPlayerInput` key/axis callbacks and input-stack boundaries, plus engine/world tick boundaries; each event carries a monotonic order, QPC-relative time, thread ID, and tick indices. The existing controller hook is reused, preserving its second-player suppression behavior. This instrumentation has built locally but has not yet been runtime tested. It does not inject inputs, change timing, or establish callback order until an actual trace is captured.

The PDB also contains Unreal fixed-rate support (`UEngine::bUseFixedFrameRate`, `UEngine::FixedFrameRate`, and `UFixedFrameRateCustomTimeStep`) and `UGameEngine::MaxDeltaTime`, but none is enabled or configured by this mod. `UWorld::Tick` interception alone is not a global fixed timestep: engine input, game-instance callbacks, tick prerequisites, physics, timers, animation, and rendering may observe other delta/time sources. Current frame advance forces `1/60` only for one world tick; it does not demonstrate that all relevant systems advanced exactly once at that timestep.

PDB symbols include `FRandomStream`, `UAISystem::RandomStream`, and random-stream/Kismet APIs. This confirms the engine and AI system expose random state, but the symbols alone do not prove which streams Sifu's combat and enemy decisions consume or whether their seeds remain stable after reset. Combat-specific call sites and seeds remain unmeasured.

The Steam PDB resolves `AGameModeBase::Reset` (RVA `0x03361A60`), `AGameModeBase::ResetLevel` (`0x03361CF0`), `AActor::Reset` (`0x030D4690`), and `AActor::K2_OnReset` (`0x037A4B80`). Sifu also exposes `UWGGameFlow::BPF_RestartCurrentMap` (`0x0185F650`). The latter is explicitly a current-map restart function; no evidence ties it to the in-place Reset Situation command, so the mod does not call it. The PDB contains `TrainingRoom` enums but no named Free Training situation-reset function. The actual button-to-operation path and whether actors are reset in place or replaced are unknown. Passive hooks log candidate calls with object path, level, and caller RVA; GameMode reset and map-restart hooks log begin/end around the native call using identity captured before it. Actor callback detail is capped at 32 events per five-second window; additional callbacks are counted and reported at most once per second. Top-level GameMode/map-restart events are not capped. These hooks do not invoke or alter actors. A runtime trace is required before selecting a restoration workflow.

A reset-and-restore prototype is blocked by the missing verified reset-complete signal and actor lifecycle semantics. The user can activate Reset Situation manually and inspect `tas: reset trace` events. If the reset triggers `AGameModeBase::ResetLevel`/`Reset` and actor callbacks, those call sequences provide candidate boundaries and reveal whether the same actor instances are reset. If only `UWGGameFlow::BPF_RestartCurrentMap` fires, it is map travel, not an in-place encounter reset. If none of these hooks fire, the next smallest diagnostic is a narrowly armed `UObject::ProcessEvent` trace around the manual reset. Do not substitute speculative invocation. No checkpoint is re-applied after a manual reset yet.
Next runtime test (performed by the user): activate Free Training Reset Situation manually and inspect `tas: reset trace` events for call order, affected actors, and level identity. Compare player/opponent transforms, vitals, movement, and animation at idle after repeated resets. AI/RNG equivalence remains unmeasured. No reset-based restoration or complete gameplay savestate has been demonstrated.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or arbitrary-frame savestates.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. `OnFrame` profiling uses QPC to log five-second averages/maxima for the hook and major inherited calls. The GUI path separately logs its five-second rendering cost while open. Checkpoint save/load logs operation latency; a fixed snapshot is overwritten in place. Process working set/private usage is sampled once per minute. ImGui frame processing and rendering now run only while the TAS GUI is open. No runtime performance conclusion is possible until the instrumented DLL has been exercised for an extended session.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. The user has standing authorization for the established SifuTAS Steam test DLL path after confirming it is a known project DLL, Sifu is closed, the rollback backup is hash-verified, and the new build hash matches. Stop if the target is missing or unexpected. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add actionable log lines before speculative refactoring.
