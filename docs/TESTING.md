# Phase 1 test procedure (Windows)

**Status:** the user reports the GUI, mouse handling, rebinding, configurable slowdown, frame-step increments, and settings persistence work in-game. Checkpoint v1 was tested in Free Training; saving was refused by its `TrainingRoom` path check. The old error did not reveal whether level-path lookup failed or returned a different path. This pass removes the unverified map-name restriction, logs the actual level on save refusal, and reports tracker search counts. GUI scaling, pause-on-open, and the continuous slider are built but await in-game testing. Multiplicative slowdown appeared to preserve cinematic transitions. An extended diagnostic test did not reproduce the reported acceleration. The stored base dilation moved through Sifu time effects and returned to 1.0; the time-dilation setter logged changes from caller RVA `0x033638F7`, without a monotonic upward drift. At 1×, samples reached about 1.006 world seconds per real second during active progression. Later, `world_time` froze while `real_time` advanced, with `paused=0` and no enemy identified, so that segment is not a valid normal gameplay-speed measurement.

The current diagnostic build logs a `tas: diag` sample about once per second on the game thread. It includes the three stored world dilation fields, original and TAS-scaled effective values, player and nearest active enemy custom dilation, the latest current-world tick delta, world/real time deltas, and world-time-to-wall-time ratios. Native setter change records include caller RVAs and are rate-limited. Do not treat diagnostics as game-tested until this build has run in Sifu.

## Local build record

- 2026-11-04: `build.ps1` completed for the Steam install using its local `Sifu-Win64-Shipping.pdb` and `-LocalBuildName steam`.
- The generated Steam executable fingerprint is timestamp `0x68409AB0`, image size `0x06295000`; `UWorld_Tick` resolved to a nonzero RVA.
- The crypto test vectors passed and `build/dsound.dll` was produced (1,223,680 bytes).
- The game directory had no preexisting `dsound.dll` or `SifuCoop.ini`; only the base game PAK and shipped `CgSDK`/`Wwise` plugin directories were found.
- The first in-game test, reported by the user, found slowdown, pause and frame advance appeared functional. It also exposed mouse capture/camera sensitivity trouble and slowdown resetting around special attacks. Its log confirmed the inherited D3D overlay and cursor recentring hook were active.
- 2026-11-04: the first build was installed after approval. The revised build was compiled and installed after preserving the prior DLL as `build/dsound.pre-update.dll`.
- 2026-11-04: the effective-time-dilation hook and F1–F5 presets built successfully against the same Steam fingerprint; installation was held for backup verification and approval.
- 2026-11-05: after approval, the multiplicative-time-scale build replaced the prior DLL. Installed SHA-256: `ACD98E654B6ECCE2C64DE13734A48CE896900CE6678F66809595C5FB69E56523`. Prior installed DLL and earlier rollback copy are preserved in `build/`.
- 2026-11-05: the diagnostic pass resolved the Steam PDB fields and setter RVAs, passed the build script's crypto vectors, and compiled a 1,228,288-byte DLL. It has not been deployed or tested in-game.
- 2026-11-05: the dedicated TAS GUI/configuration pass rebuilt against the same Steam fingerprint, resolved 16/16 PDB member offsets, passed the crypto test vectors, and compiled a 1,226,752-byte DLL (SHA-256 `168E1CB5A3BEF36AE4A6A2FF69333DB496D8EBD4ED7E49C7A4258C10C9483F5C`). The user later reported GUI, mouse, rebinding, configurable speeds/steps, and persistence working in-game.
- 2026-11-05: checkpoint v1 rebuilt against the same Steam fingerprint, resolved 16/16 PDB member offsets, passed the crypto test vectors, and compiled a 1,240,576-byte DLL (SHA-256 `66A6626A83063D1DF7A4C674CB3A86CBA9C7354066C0B3432298B6C179AFBD08`). It has not been installed or tested in-game.
- 2026-11-06: the user installed and tested checkpoint v1 in Free Training. Save was refused by the hardcoded `TrainingRoom` path check, but the old build did not log whether path lookup failed or returned a different path. The current pass removes that filter and adds current-level and enemy-discovery diagnostics; it is not yet installed.
- 2026-11-06: this follow-up rebuilt against the same Steam fingerprint, resolved 16/16 member offsets, passed the crypto test vectors, and compiled a 1,242,112-byte DLL (SHA-256 `11E80C6DF08682F022D940D9987E2E0DF9AFB88AF66A78199B5CC3A8497FD851`). It has not been installed or tested in-game.

## Preconditions

- Game installed and closed; Windows; legitimately owned PC copy.
- Matching `Sifu-Win64-Shipping.exe` and `.pdb` available in game `Binaries\Win64` folder.
- Python 3 and WinLibs UCRT x64 `g++` accessible in PowerShell.
- Existing `dsound.dll` and conflicting mods inspected; obtain approval before any replacement. `SifuCoop.ini` has `[net] mode=off` or is absent (upstream defaults to off).
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
9. For acceleration diagnostics, run unpaused at 1× for a sustained baseline in the training room, with no speed hotkeys pressed. Compare `world_per_real`, `world_per_wall`, stored dilation components, and `product` versus `original`; retain logs before and after any apparent speed change.
10. Open the GUI with **F6**. Confirm the Playback, Savestates, Recording, and Settings tabs render, mouse and keyboard work while paused, and the cursor is visible only while the GUI is open. Confirm F6 pauses on open only when gameplay was running; close with F6 and confirm it resumes only if the GUI-owned pause remains unchanged.
11. Use GUI buttons and shortcuts to pause/resume and advance frames. Set speed through the continuous Playback slider while paused and running, then confirm F3–F5 presets change the same speed value. Change the step count from 1 to 2–10 and verify that many world-tick hook calls run while paused. The displayed engine tick counter is not a verified simulation-frame count.
12. Rebind actions to unique function keys and Ctrl/Alt/Shift combinations; verify duplicate shortcuts are rejected and valid settings persist across restarts. Confirm F7/F8 remain configurable and invoke checkpoint save/load. Confirm no co-op menu or external overlay appears during ordinary gameplay.
13. For checkpoint v1, use Free Training with one living enemy. Pause, save, resume and change positions/vitals, pause and load. Check position, velocity, health/max health, guard, custom dilation, montage clearing, and that the game remains paused. Only save while both actors are standing, near-stationary, and between montages. Resume and watch for enemy AI actions that were not present in the snapshot; repeat several times. Record each field that restores and any partial-load message. Do not use dead actors or level transitions.
14. In Free Training, verify that F6 opens the GUI and pauses only if gameplay was running; F6 close resumes only if automatic opening caused the pause and no manual F1/GUI pause change occurred. Check opening while already paused, manually toggling pause while open, and preserving the selected playback speed.
15. Drag the Playback continuous speed slider at 1×, 0.01×, 2×, and 4× while running and paused. Confirm F3–F5 presets update the same value. Adjust GUI scale in Settings and confirm text/buttons/window resize without layout overlap; restart and confirm the scale persists.
16. In Free Training, pause between attacks and try checkpoint save/load. If rejected, record `tas: checkpoint save rejected level=...`, the detailed `save refused` reason, and `enemy search tracked/active/living/nearby` counts. The enemy tracker enumerates current-world `AFightingCharacter` actors and treats non-pooled actors as active; the diagnostics will show whether the training opponent was found. Retain the same restrictions on downed, dead, moving, and montage-active actors.

## Logs

`%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` (upstream logger path; may vary if the game installation redirects user data).

Diagnostic strings:
- `tas: world tick hooked`: hook installed, not proof of correct behavior.
- `tas: UWorld::Tick symbol missing`: offset unavailable; F1/F2 won't operate.
- `tas: world tick hook failed`: MinHook installation failed.
- `tas: effective time dilation hook failed`: MinHook installation failed.
- `tas: diag timestamp=...`: full one-second diagnostic sample; at a 1× baseline, `product`, `original`, and `effective` should match, and world-time ratios should be near 1.0 during unpaused play.
- `tas: setter=TimeDilation ... caller_rva=...`: native base-dilation setter changed the stored value.
- `tas: setter=DemoPlayTimeDilation ... caller_rva=...`: replay-system demo dilation setter received a changed value. Matinee dilation is sampled directly because no matching setter symbol was found in this PDB.
- `tas: step N`: one hooked world-tick invocation occurred, not proof of a complete simulation step.

Report: storefront, executable build fingerprint, exact build command, compiler errors (if any), log excerpt, key behavior, crash/hang details. Do not upload game executable, PDB or other proprietary data.

## Release gate (future)

No public release until clean Windows build, install/uninstall/backup, observed working controls, collision handling, and documentation are verified. Deterministic replay/savestates are additional milestones, not Phase 1 claims.
