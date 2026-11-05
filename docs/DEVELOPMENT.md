# Development scope and architecture

## Objective

Create tool-assisted **Sifu combat choreography videos** with input precision beyond human play. Reproducibility is desirable for editing, but no competitive run validation is required. Slow motion and frame stepping first; prioritize usable checkpoints and savestates once the engine behavior is understood.

## Existing infrastructure

- `src/dllmain.cpp`: SifuCoop-derived `dsound.dll` bootstrapping and executable fingerprint safeguards.
- `src/proxy/dsound_proxy.cpp`: native Windows proxy loading.
- `src/core/hooks.cpp`: `UGameEngine::Tick` vtable hook, `AWorldSettings::GetEffectiveTimeDilation` multiplier hook, focused TAS hotkeys and `UWorld::Tick` pause/step hook.
- `src/ue/reflection.{h,cpp}`: Unreal object lookup, reflected calls and console command dispatch.
- `tools/pdbdump/pdbdump.py`: resolves function RVAs and field offsets from a matching Sifu PDB; regenerates `src/core/offsets.g.h` and local build fingerprints.
- `src/ui/d3d_overlay.cpp`: retained SifuCoop ImGui/Direct3D 11 overlay code for a future TAS interface; overlay initialization is disabled.

## Immediate milestones

1. **Diagnose apparent acceleration** on Windows using game-thread samples of stored dilation components, setter caller RVAs, actor custom dilation, tick delta, and world-time progression relative to real time. First collect a sustained 1× baseline; do not clamp or reset values until the changing source is identified.
2. **Validate slowdown** on a training encounter. Distinguish global time dilation from renderer/audio/UI clocks.
3. **Validate pause and frame advance**: the engine continues rendering and accepts hotkeys, no catastrophic hangs, no drift/jump when resuming. Check whether input events are buffered during pause and whether multiple worlds tick.
4. **Remove co-op runtime dependencies** once Phase 1 works. Keep the proxy, reflection, tick hooks, PDB tools, ImGui and logging; replace the co-op menu with TAS controls.
5. **Capture and inject inputs** at the relevant controller/input processing boundary, preserving press/release edges and analog axes.
6. **Recording/editing**: frame-indexed input timeline, replay from repeatable encounter starts, per-frame state checksum and divergence reporting.
7. **Checkpoint v1**: in the training room, save the player and one enemy only from a paused, idle state. Track stable actor identity within the current world and capture root transform, velocity, health, structure/guard, actor time dilation, down state, movement mode, and current montage/time. Restore the same actors by validated pointers or stable gameplay ID, using engine setters for transform/vitals where possible; then resume their AI only after their restored baseline has been checked.
8. **Active-fight savestates**: later reverse engineer behavior-tree/AI state, queued attacks and callbacks, montage/root-motion state, movement and physics, timers, and random streams. Do not treat actor transforms and vitals as a complete arbitrary-frame snapshot.

## Checkpoint v1 strategy

Use direct capture and restore of a quiet training-room encounter as the first prototype. Resetting combat systems before restore is riskier for v1: stopping behavior trees or clearing attack/defense components can dispatch callbacks, invalidate object references, or move actors into recovery states. Instead, require a capture/restore gate where player and enemy are alive, standing, not in an active attack/hit reaction montage, and have no pending order. Pause the world, capture both actors and the identified combat fields, restore transforms and movement before vitals and montage state, verify the result, then resume the enemy brain.

Use a stable enemy identity based on its current-world object path plus class and spawn generation; invalidate the snapshot when the world changes or the actor is destroyed/replaced. A reset/re-spawn path can be evaluated if that identity fails after training-room reset.

Likely v1 failures are stale actor references after reset, AI resuming with a queued attack, root motion overriding restored position, montage not restoring its linked animation state, health/structure callbacks firing out of order, and timers/callbacks from before the snapshot running after restore. On any identity or state validation failure, refuse the load and require a fresh capture. This prototype will retry a quiet setup; it will not restore an active fight.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or any savestate.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. Decouple the code only after the hook experiments are understood, to minimize simultaneous causes of breakage.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. Do not deploy or replace an installed DLL without approval. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add simple test instrumentation and actionable log lines before speculative refactoring.
