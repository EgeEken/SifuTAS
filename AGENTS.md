# Agent instructions: SifuTAS

- Work on `testing` only. Never modify `main`, open a release, merge a branch, or create a tag without explicit user permission.
- Commit and push each completed, reviewed development pass to `testing`.
- Never modify or merge into `main` without explicit user authorization.
- Report the commit SHA after each completed pass.
- Do not deploy or replace an installed DLL without explicit user approval.
- Scope: native single-player Sifu TAS tooling for choreographed combat videos. Prioritize usable slowdown and frame advance, then input recording/injection, editable playback, checkpoints, deterministic replay, and savestates.
- The phase-one controls, dedicated TAS GUI, mouse handling, shortcut rebinding, configurable speed and frame-step settings were reported working in-game. An extended diagnostic test did not reproduce the reported acceleration; the latest log shows native time-dilation transitions without monotonic drift, but later world time stopped advancing, so it does not prove normal gameplay progression throughout. Checkpoint v1 is compiled but awaits in-game testing.
- The TAS GUI must not draw the inherited SifuCoop interface or an external overlay while closed. Keep cursor drawing limited to the open TAS GUI. Do not add cursor-positioning or cursor-confinement hooks; closed-GUI gameplay must retain normal Sifu input and camera behavior.
- Keep keyboard bindings action-based and shared between GUI buttons and hotkeys. Persist TAS controls in `SifuTAS.ini` beside the mod. Checkpoint v1 is a paused, in-memory training-room snapshot of a quiet living player and one tracked enemy; it does not restore AI queues or movement mode. Input recording remains unavailable, and engine tick counts are not verified simulation-frame counts.
- Reuse imported SifuCoop code (native DLL/proxy, Unreal reflection, symbol generator, overlay, build guards). Prefer focused changes and keep upstream origin/license notices intact.
- Never commit or upload proprietary Sifu executables, PDBs, game assets, saves or user-specific paths. Never disable executable fingerprint checks to make an incompatible build run.
- The mod runs only on the user's local Windows system; GitHub/Codex cloud environments cannot directly test their Sifu installation. Do not claim remote code can automatically install into their computer.
- Do not overwrite existing `dsound.dll`, user settings, or other mods; inspect and obtain approval or make a restorable backup first. Keep `[net] mode=off` for solo testing.
- When troubleshooting, collect compiler output, `SifuCoop.log`, observed behavior and exact build fingerprint. Separate observed facts from hypotheses.
- A skipped `UWorld::Tick` is not proof of a total pause; a single `UWorld::Tick` is not proof of a single complete simulation frame. Assess Unreal's other tick groups/clocks/input queue carefully.
- Prefer minimal code changes, Windows PowerShell commands and reproducible build instructions. Avoid unnecessary abstractions and premature UI work.
