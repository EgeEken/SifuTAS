# Phase 1 test procedure (Windows)

**Status:** the user reports F1–F5, pause, frame stepping and mouse behavior work in-game. Cinematic transitions appear preserved. Extended play showed gradual apparent acceleration (around 1.5×); cause remains under investigation.

## Local build record

- 2026-11-04: `build.ps1` completed for the Steam install using its local `Sifu-Win64-Shipping.pdb` and `-LocalBuildName steam`.
- The generated Steam executable fingerprint is timestamp `0x68409AB0`, image size `0x06295000`; `UWorld_Tick` resolved to a nonzero RVA.
- The crypto test vectors passed and `build/dsound.dll` was produced (1,223,680 bytes).
- The game directory had no preexisting `dsound.dll` or `SifuCoop.ini`; only the base game PAK and shipped `CgSDK`/`Wwise` plugin directories were found.
- The first in-game test, reported by the user, found slowdown, pause and frame advance appeared functional. It also exposed mouse capture/camera sensitivity trouble and slowdown resetting around special attacks. Its log confirmed the inherited D3D overlay and cursor recentring hook were active.
- 2026-11-04: the first build was installed after approval. The revised build was compiled and installed after preserving the prior DLL as `build/dsound.pre-update.dll`.
- 2026-11-04: the effective-time-dilation hook and F1–F5 presets built successfully against the same Steam fingerprint; installation was held for backup verification and approval.
- 2026-11-05: after approval, the multiplicative-time-scale build replaced the prior DLL. Installed SHA-256: `ACD98E654B6ECCE2C64DE13734A48CE896900CE6678F66809595C5FB69E56523`. Prior installed DLL and earlier rollback copy are preserved in `build/`.

## Preconditions

- Game installed and closed; Windows; legitimately owned PC copy.
- Matching `Sifu-Win64-Shipping.exe` and `.pdb` available in game `Binaries\Win64` folder.
- Python 3 and WinLibs UCRT x64 `g++` accessible in PowerShell.
- Existing `dsound.dll` or conflicting mods **backed up and removed** before deployment. `SifuCoop.ini` has `[net] mode=off` or is absent (upstream defaults to off).
- Use a short solo training encounter, not a long gameplay save.

## Procedure

1. Run `.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam` (or `epic`) **without `-NoGen`**. Confirm the generated offset table contains nonzero `UWorld_Tick` for the current executable and the compiled DLL exists.
2. Inspect existing mods, make a verified backup, and obtain approval before replacing any existing `dsound.dll`. Do not use conflicting proxy mods.
3. Start Sifu. Confirm the game renders normally and no critical mod initialization errors appear.
4. Check that the cursor is hidden and camera rotation has normal sensitivity.
5. Press **F3**, **F4** and **F5** separately; confirm 0.5×, 0.2× and 0.1×. Press the active preset again to restore 1×, and switch directly between presets.
6. Use a special attack that previously changed playback speed. Check that Sifu's native slow-motion transition remains smooth and is multiplied by the selected TAS preset.
7. At each preset, press **F1** to pause, **F2** repeatedly to step, then **F1** to resume. Confirm the selected preset survives pausing and each step advances the same increment.
8. Test pause and resume after loading a different room; ensure loading transitions cannot wedge the simulation. Re-select a speed preset to return to 1×.

## Logs

`%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` (upstream logger path; may vary if the game installation redirects user data).

Diagnostic strings:
- `tas: world tick hooked`: hook installed, not proof of correct behavior.
- `tas: UWorld::Tick symbol missing`: offset unavailable; F1/F2 won't operate.
- `tas: world tick hook failed`: MinHook installation failed.
- `tas: effective time dilation hook failed`: MinHook installation failed.
- `tas: time scale game=... multiplier=... effective=...`: sampled native scale, TAS multiplier and combined scale; logs are change-driven and rate-limited.
- `tas: step N`: one hooked world-tick invocation occurred, not proof of a complete simulation step.

Report: storefront, executable build fingerprint, exact build command, compiler errors (if any), log excerpt, key behavior, crash/hang details. Do not upload game executable, PDB or other proprietary data.

## Release gate (future)

No public release until clean Windows build, install/uninstall/backup, observed working controls, collision handling, and documentation are verified. Deterministic replay/savestates are additional milestones, not Phase 1 claims.
