# Agent instructions: SifuTAS

- Work on `testing` only. Never modify `main`, open a release, merge a branch, or create a tag without explicit user permission.
- Commit and push each completed, reviewed development pass to `testing`.
- Never modify or merge into `main` without explicit user authorization.
- Report the commit SHA after each completed pass.
- Do not deploy or replace an installed DLL without explicit user approval.
- Scope: native single-player Sifu TAS tooling for choreographed combat videos. Prioritize usable slowdown and frame advance, then input recording/injection, editable playback, checkpoints, deterministic replay, and savestates.
- `src/core/hooks.cpp` currently contains uncompiled, untested phase-one hooks. Do not claim they work until they compile and have been tested in-game.
- Reuse imported SifuCoop code (native DLL/proxy, Unreal reflection, symbol generator, overlay, build guards). Prefer focused changes and keep upstream origin/license notices intact.
- Never commit or upload proprietary Sifu executables, PDBs, game assets, saves or user-specific paths. Never disable executable fingerprint checks to make an incompatible build run.
- The mod runs only on the user's local Windows system; GitHub/Codex cloud environments cannot directly test their Sifu installation. Do not claim remote code can automatically install into their computer.
- Do not overwrite existing `dsound.dll`, user settings, or other mods; inspect and obtain approval or make a restorable backup first. Keep `[net] mode=off` for solo testing.
- When troubleshooting, collect compiler output, `SifuCoop.log`, observed behavior and exact build fingerprint. Separate observed facts from hypotheses.
- A skipped `UWorld::Tick` is not proof of a total pause; a single `UWorld::Tick` is not proof of a single complete simulation frame. Assess Unreal's other tick groups/clocks/input queue carefully.
- Prefer minimal code changes, Windows PowerShell commands and reproducible build instructions. Avoid unnecessary abstractions and premature UI work.
