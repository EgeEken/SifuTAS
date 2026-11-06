# Prompt for Codex on my Windows PC

Use **Codex CLI/desktop running on the actual Windows computer with Sifu installed**, not only a cloud GitHub task; a remote task cannot inspect or install files on my machine.

Copy the following into Codex:

---

I'm working on https://github.com/EgeEken/SifuTAS on the `testing` branch. Please set up the **phase-one SifuTAS prototype locally on this Windows machine**, verify the build as far as the available game files permit, and guide me through an in-game test.

Start by reading `README.md`, `AGENTS.md`, `docs/TESTING.md`, `docs/DEVELOPMENT.md`, and `docs/UPSTREAM.md`. This is native Unreal Engine 4/Sifu code imported from SifuCoop (pinned upstream snapshot), with experimental slowdown and frame-step code in `src/core/hooks.cpp`. It is **not yet known to compile or work**.

Tasks:
1. Clone SifuTAS into a convenient workspace (or use the already-open repo), check out `testing` and confirm `main` is untouched. Check local working tree before changing anything.
2. Detect installed Sifu on Steam/Epic if accessible. Identify the location of `Sifu-Win64-Shipping.exe` and whether the matching `Sifu-Win64-Shipping.pdb` exists. **Do not download, copy into Git, upload or expose proprietary game files or PDBs.** If the PDB is missing, report the blocker and find only legitimate game-distribution/debug-symbol options; do not invent offsets or bypass the executable fingerprint checks.
3. Check Python 3 and WinLibs UCRT x64 g++ availability, and install/configure the missing *developer tools* only with my approval if an installer/admin action is required.
4. Inspect the exact `build.ps1` arguments and compile using `-GameDir` and the correct `-LocalBuildName` (steam/epic). Regenerate the PDB-derived `UWorld_Tick` offset; do not use `-NoGen` initially. If compilation fails, fix only the immediate issue, rerun, and explain any significant code changes.
5. Before deployment, inspect the existing `dsound.dll`, `SifuCoop.ini` and other mod files. The user has standing authorization to replace the known SifuTAS test DLL at `C:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64\dsound.dll` after confirming Sifu is closed, the installed DLL matches a known SifuTAS build, a rollback backup is hash-verified, and the new build hash is verified. Do not ask for approval again when all these conditions hold. Stop if the target is missing, has an unknown hash, or may belong to another mod. Never overwrite user settings or unrelated files. Avoid conflicting `dsound.dll` proxies. Set network mode to `off` for solo testing.
6. Once build and backup are verified, ask before installing the test DLL next to the game executable. Do not automate a game launch or claim in-game results without actually observing them on this PC; I can launch Sifu and test F1–F5, including speed changes during special attacks.
7. Read `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log`, summarize the result, and propose the next smallest fix based on compiler/log evidence. Test if rendering continues during pause and whether F2 actually advances exactly one gameplay step.
8. Update the local docs/test notes with actual results and unresolved blockers if useful. Commit and push completed development passes to `testing`; report each commit SHA. Never modify or merge into `main` without explicit authorization. Follow the standing DLL replacement authorization and checks above; never deploy an unknown or unrelated DLL.

My goal is editing spectacular, inhuman Sifu combat videos. Frame stepping and slowdown are first; later we want programmable inputs, editable recordings, reproducible playbacks, checkpoints and savestates. Reuse the existing SifuCoop machinery rather than rebuilding it from scratch. Keep changes focused and verify every claim.

---
