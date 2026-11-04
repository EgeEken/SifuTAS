# Phase 1 test procedure (Windows)

**Status:** uncompiled, untested. These are expected behaviors, not verified results.

## Preconditions

- Game installed and closed; Windows; legitimately owned PC copy.
- Matching `Sifu-Win64-Shipping.exe` and `.pdb` available in game `Binaries\Win64` folder.
- Python 3 and WinLibs UCRT x64 `g++` accessible in PowerShell.
- Existing `dsound.dll` or conflicting mods **backed up and removed** before deployment. `SifuCoop.ini` has `[net] mode=off` or is absent (upstream defaults to off).
- Use a short solo training encounter, not a long gameplay save.

## Procedure

1. Run `.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam` (or `epic`) **without `-NoGen`**. Confirm the generated offset table contains nonzero `UWorld_Tick` for the current executable and the compiled DLL exists.
2. Back up and remove any preexisting `dsound.dll`, install the newly compiled DLL. Do not use conflicting proxy mods.
3. Start Sifu. Confirm the game renders normally and no critical mod initialization errors appear.
4. Press **F6**. Observe player and enemy motion at approximately 0.2×. Press again to restore normal speed. Check logs for `tas:` errors.
5. Press **F7**. The world should freeze *without freezing rendering or hotkey polling*. If Sifu hangs or crashes, end the test and restore backups.
6. Press **F8** repeatedly. Record whether player/enemy poses each move by one simulation increment, whether attacks/defenses trigger, and whether key presses are queued or lost.
7. Press **F7** to resume. Watch for jumps in animation, physics, timer state or enemy AI. **F9** should reset to normal/unpaused.
8. Test pause and resume after loading a different room; ensure loading transitions cannot wedge the simulation.

## Logs

`%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` (upstream logger path; may vary if the game installation redirects user data).

Diagnostic strings:
- `tas: world tick hooked`: hook installed, not proof of correct behavior.
- `tas: UWorld::Tick symbol missing`: offset unavailable; F7/F8 won't operate.
- `tas: world tick hook failed`: MinHook installation failed.
- `tas: SetGlobalTimeDilation failed`: reflected slow-motion call failed.
- `tas: step N`: one hooked world-tick invocation occurred, not proof of a complete simulation step.

Report: storefront, executable build fingerprint, exact build command, compiler errors (if any), log excerpt, key behavior, crash/hang details. Do not upload game executable, PDB or other proprietary data.

## Release gate (future)

No public release until clean Windows build, install/uninstall/backup, observed working controls, collision handling, and documentation are verified. Deterministic replay/savestates are additional milestones, not Phase 1 claims.
