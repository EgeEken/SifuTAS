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

1. **Validate this pass in-game**: test generalized checkpoint capture and partial restore in Free Training, including a player-only save, then test the GUI and supported fields. The previous log diagnosis is confirmed; this build is installed and awaits in-game verification.
2. **Validate the dedicated SifuTAS GUI and settings**: mouse handling, tab behavior, configurable controls and settings persistence were previously reported working. Recheck for regressions from scaling and slider changes.
3. **Validate slowdown** on a training encounter. The recent extended diagnostic session did not reproduce acceleration; its late samples show world time stopped advancing and need a new baseline during active play.
4. **Validate pause and frame advance**: the user reports these work. Check for input buffering or multi-world tick differences if later changes affect them.
5. **Capture and inject inputs** at the relevant controller/input processing boundary, preserving press/release edges and analog axes.
6. **Recording/editing**: frame-indexed input timeline, replay from repeatable encounter starts, per-frame state checksum and divergence reporting.
7. **Active-fight savestates**: later reverse engineer behavior-tree/AI state, queued attacks and callbacks, montage/root-motion state, movement and physics, timers, and random streams. Do not treat actor transforms and vitals as a complete arbitrary-frame snapshot.

`src/tas/state_reference.h` keeps checkpoint identity/timeline metadata separate from the future recording system: level path, player/enemy identities, engine tick, optional verified simulation frame, and recording branch ID.

## Checkpoint v1 limits

Checkpoint v1 enumerates up to 256 current-world `AFightingCharacter` candidates and snapshots up to 32 distinct characters plus the player. There is no map-name restriction. The latest Free Training log reports level `/Game/Maps/TU_3/ArenaModeMenu/ArenaMode_Menu` and one active tracked opponent with `living=0`; source review confirms `Tracked::health` is populated by the multiplayer `PublishEnemies()` path, so it remains zero in offline play. The checkpoint no longer treats that network cache as authoritative and reads health directly via `ResolveFighter()` / `GetHealth()`.

Capture records availability for transform, velocity, health/max health, guard, down/death flags, actor time dilation, and montage/playhead without requiring every field or actor to be restorable. It includes moving, downed, distant, and zero-health characters. If actor count exceeds the cap or enumeration fails, the snapshot is marked incomplete; a player-only snapshot is still possible.

Restore checks the same world and level, object pointer/path/class, and current-world membership per actor. Supported fields are written and read back. Missing actors are skipped without invalidating the slot. Transform and velocity are skipped when saved/current life or montage state makes pose restoration unsafe. Health and guard do not resurrect actors or reverse down/death state. Animation playback, movement mode, AI queues, timers, pending combat actions, and callbacks remain unsupported. Per-character logs report available, restored, failed, unsupported, and unavailable field masks. F7/F8 requests run on the game thread, and load leaves simulation paused with the slot available for repeated attempts.

This is not an arbitrary-frame savestate. A partial restore does not rewind AI, animation, attack actions, movement mode, timers, callbacks, or destroyed actors. Inspect the per-field result while paused before resuming.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or arbitrary-frame savestates.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. Checkpoint v1 can capture active states but does not restore them coherently; a moving, downed, or montage-active save may load only a subset of fields.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. The user has standing authorization for the established SifuTAS Steam test DLL path after confirming it is a known project DLL, Sifu is closed, the rollback backup is hash-verified, and the new build hash matches. Stop if the target is missing or unexpected. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add actionable log lines before speculative refactoring.
