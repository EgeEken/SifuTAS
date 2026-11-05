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

1. **Compile** on Windows against the locally installed game's matching PDB. Resolve symbol ambiguities or calling convention issues without guessing offsets.
2. **Validate slowdown** on a training encounter. Distinguish global time dilation from renderer/audio/UI clocks.
3. **Validate pause and frame advance**: the engine continues rendering and accepts hotkeys, no catastrophic hangs, no drift/jump when resuming. Check whether input events are buffered during pause and whether multiple worlds tick.
4. **Remove co-op runtime dependencies** once Phase 1 works. Keep the proxy, reflection, tick hooks, PDB tools, ImGui and logging; replace the co-op menu with TAS controls.
5. **Capture and inject inputs** at the relevant controller/input processing boundary, preserving press/release edges and analog axes.
6. **Recording/editing**: frame-indexed input timeline, replay from repeatable encounter starts, per-frame state checksum and divergence reporting.
7. **Checkpoint/rewind**: prefer restarting and fast-forwarding deterministically initially; investigate actual state snapshots after confirming what engine state is serializable.
8. **Savestates**: investigate Unreal gameplay, AI, animation, physics and RNG state. Avoid assuming an arbitrary process memory snapshot is safe.

## Known risks

`UWorld::Tick` suppression is an **experiment**, not authoritative engine pausing; other engine timing, animation and input paths may continue. Passing a fixed 1/60 delta to `UWorld::Tick` does not fix the complete engine timestep. `F2` can currently advance a game-world tick without delivering the intended gameplay input. Engine tick hooks may be build-specific or unstable. The prototype does not support deterministic playback or any savestate.

The current source still initializes original SifuCoop co-op/network infrastructure. Use `[net] mode=off` and do not attempt multiplayer testing. Decouple the code only after the hook experiments are understood, to minimize simultaneous causes of breakage.

## Engineering principles

Keep `main` release-only; all experimentation occurs on `testing`. Commit and push completed development passes to `testing`, and report each commit SHA. Never modify or merge into `main` without explicit authorization. Do not deploy or replace an installed DLL without approval. Retain upstream MIT and third-party licenses. No proprietary Sifu files in git. Add simple test instrumentation and actionable log lines before speculative refactoring.
