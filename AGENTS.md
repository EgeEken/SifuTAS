# Agent instructions: SifuTAS

- Work on `testing` only. Never modify `main`, open a release, merge a branch, or create a tag without explicit user permission.
- Commit and push each completed, reviewed development pass to `testing`.
- Never modify or merge into `main` without explicit user authorization.
- Report the commit SHA after each completed pass.
- Standing user authorization permits replacing the SifuTAS test DLL at `C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64\dsound.dll` without another prompt, only after confirming the installed file is a known SifuTAS build, Sifu is closed, a hash-verified rollback backup exists, and the new build hash matches. If the target is missing, has an unknown hash, or may belong to another mod, stop and ask.
- Scope: native single-player Sifu TAS tooling for choreographed combat videos. Prioritize usable slowdown and frame advance, then input recording/injection, editable playback, checkpoints, deterministic replay, and savestates.
- Phase-one controls, GUI and mouse handling were reported working in-game. Repeated Free Training snapshot restores for position and supported vitals were confirmed, but camera motion after resume, combat actions, and encounter progression remain unresolved. Rain arena logged 103 discovered fighting characters but only seven captured; current code classifies exclusions and frees `GetPathName` FString values using the matching PDB's `FMemory::Free`. The current pass guards TAS timing controls against death/game-over, loading, menus, and unknown state; runtime verification is pending.
- The TAS GUI must not draw the inherited SifuCoop interface or an external overlay while closed. Keep cursor drawing limited to the open TAS GUI. Do not add cursor-positioning or cursor-confinement hooks; closed-GUI gameplay must retain normal Sifu input and camera behavior.
- Keep keyboard bindings action-based and shared between GUI buttons and hotkeys. Persist TAS controls in `SifuTAS.ini` beside the mod. The experimental state snapshot captures up to 32 distinct current-world fighting characters plus the player with field availability recorded, but does not restore combat actions or encounter progression. Input recording remains unavailable, and engine tick counts are not verified simulation-frame counts.
- Reuse imported SifuCoop code (native DLL/proxy, Unreal reflection, symbol generator, overlay, build guards). Prefer focused changes and keep upstream origin/license notices intact.
- Never commit or upload proprietary Sifu executables, PDBs, game assets, saves or user-specific paths. Never disable executable fingerprint checks to make an incompatible build run.
- The mod runs only on the user's local Windows system; GitHub/Codex cloud environments cannot directly test their Sifu installation. Do not claim remote code can automatically install into their computer.
- Do not overwrite user settings or unrelated mods. For the established SifuTAS Steam test DLL only, follow the standing deployment authorization above and preserve a rollback backup. Keep `[net] mode=off` for solo testing.
- When troubleshooting, collect compiler output, `SifuCoop.log`, observed behavior and exact build fingerprint. Separate observed facts from hypotheses.
- A skipped `UWorld::Tick` is not proof of a total pause; a single `UWorld::Tick` is not proof of a single complete simulation frame. Assess Unreal's other tick groups/clocks/input queue carefully.
- Prefer minimal code changes, Windows PowerShell commands and reproducible build instructions. Avoid unnecessary abstractions and premature UI work.

## Current control and replay policy

- The F7/F8 feature is an experimental state snapshot, not a gameplay savestate: it snapshots up to 32 current-world fighting characters plus the player and restores only supported actor fields. Combat actions and encounter progression are not restored; preserve the implementation for debugging but do not expand it speculatively.
- TAS controls must use the centralized `SupportedTASGameplayState` validator. On death/game-over, world transition, Sifu pause, known front-end/menu levels, or validation failure, F1/F2/presets and GUI playback actions must not affect gameplay; release TAS pause, clear pending steps, and apply an effective 1.0x multiplier while retaining the user's selected/configured speeds.
- The opt-in native replay probe calls Sifu `UReplaySystem` start/stop APIs only when eligible. Treat PDB API names as confirmed and replay contents, seek fidelity, independent playback speed, and return-to-live behavior as unverified until in-game tested. Never represent demo replay as a live-world savestate.
