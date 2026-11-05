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

1. **Validate this pass in-game**: test Free Training checkpoint save/load, GUI text scaling, pause-on-open/manual pause handling, and the continuous playback speed slider. The code is built but awaits installation and testing.
2. **Validate the dedicated SifuTAS GUI and settings**: mouse handling, tab behavior, configurable controls and settings persistence were previously reported working. Recheck for regressions from scaling and slider changes.
3. **Validate slowdown** on a training encounter. The recent extended diagnostic session did not reproduce acceleration; its late samples show world time stopped advancing and need a new baseline during active play.
4. **Validate pause and frame advance**: the user reports these work. Check for input buffering or multi-world tick differences if later changes affect them.
5. **Capture and inject inputs** at the relevant controller/input processing boundary, preserving press/release edges and analog axes.
6. **Recording/editing**: frame-indexed input timeline, replay from repeatable encounter starts, per-frame state checksum and divergence reporting.
7. **Active-fight savestates**: later reverse engineer behavior-tree/AI state, queued attacks and callbacks, montage/root-motion state, movement and physics, timers, and random streams. Do not treat actor transforms and vitals as a complete arbitrary-frame snapshot.

`src/tas/state_reference.h` keeps checkpoint identity/timeline metadata separate from the future recording system: level path, player/enemy identities, engine tick, optional verified simulation frame, and recording branch ID.

## Checkpoint v1 limits

Checkpoint v1 implements a direct in-memory snapshot of a paused encounter. The first in-game Free Training save was rejected by a level-name restriction because Sifu's actual Free Training level identity was not verified. No reliable Free Training mode API is known in the current source, so the map-name gate has been removed; the slot still records level identity and refuses a load if the world or level changes. Save requires the same world and tracked actor instances, alive/standing actors, low velocity, and no active montage. The existing enemy tracker enumerates all current-world `AFightingCharacter` actors and marks non-pooled actors active. This pass logs actual level identity on save rejection and tracker counts when no enemy qualifies.

The snapshot restores transforms, velocity through the existing movement/root velocity helper, max/current health, guard, and custom time dilation, with readback verification. Empty saved montages are handled by stopping any currently active montage and verifying it cleared. F7/F8 requests are processed from the game-thread frame hook; the Savestates GUI only queues requests. Successful loads remain paused.

Identity validation uses the current level path and world pointer, current player pointer, enemy roster/hash and pointer, engine UObject validity, Unreal `IsValid`, object path, and class path. A changed world, level, player, or enemy invalidates the slot. The nearest active tracked enemy within the configured proximity gate is selected at save time.

Movement mode, behavior-tree state, queued enemy actions, attack/defense internals, timers, and callbacks are not captured. Active montages, down/death states, moving actors, destroyed actors, and level transitions are refused; there is no attempt to resurrect or respawn. A write/readback failure reports the exact failed field and may leave an explicitly reported partial restore. The caller must inspect while still paused before resuming. This is not an arbitrary-frame savestate.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or arbitrary-frame savestates.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. Decouple the code only after the hook experiments are understood, to minimize simultaneous causes of breakage. Checkpoint v1 is an experimental quiet-state restore, not an arbitrary-frame savestate; AI queues, movement mode, timers, and callbacks remain outside the snapshot.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. Do not deploy or replace an installed DLL without approval. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add simple test instrumentation and actionable log lines before speculative refactoring.
