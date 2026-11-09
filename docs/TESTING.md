# Testing SifuTAS

Codex must never launch, close, kill, or control Sifu. The user performs all runtime tests. Before deployment, check whether Sifu is running; if so, leave it untouched and skip installation.

## Offline checks

From PowerShell in the repository:

```powershell
.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam
git diff --check
```

The build uses the installed matching PDB to regenerate offsets and runs the existing crypto vectors. A successful build is not an in-game test.

## Manual acceptance

1. Launch Sifu normally and enter a playable encounter.
2. Press F1. Confirm the fullscreen interface appears and gameplay pauses.
3. Click Resume, pause again, advance one tick, and advance the configured 2–10 ticks. Confirm the interface remains responsive while paused.
4. Adjust the center speed slider from 0.1× to 2.0× and reset to 1.0×. Close F1 and confirm gameplay resumes only when the GUI owned the pause.
5. Press `2`, `3`, and `4`; confirm they set 0.8×, 0.5×, and 0.2×, including repeated presses. Confirm the briefly displayed indicator uses the selected value.
6. Confirm `1` pauses/resumes and `5`/`6` advance one/configured world ticks without activating Sifu's corresponding input actions. In Free Training, check `2` does not also invoke Reset Situation.
7. Reopen F1, rebind an action to a keyboard key and a mouse button in separate checks, reject a duplicate, and verify F1 cannot be assigned. Verify an unbound key still reaches Sifu.
8. Disable the speed indicator. Close and reopen the GUI and then restart the game to verify the selection and all settings persist in `SifuTAS.ini`.
9. Close the GUI and verify mouse movement, camera rotation, and all unbound gameplay input remain normal.

Report the exact mod build identifier, Sifu executable version, and any related `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` lines. Do not commit logs or game files.

## Deployment safeguards

For the established Steam test DLL only, standing authorization permits replacement after all of these checks pass:

- Sifu is confirmed closed without controlling its process.
- The installed DLL hash matches a known SifuTAS build. Stop if it is missing or belongs to another mod.
- A rollback copy is created and its hash matches the installed DLL.
- The local build hash is verified and the same artifact is installed.
- `SifuTAS.ini` is preserved.

If a condition cannot be verified, do not deploy. Never overwrite another mod's files.

## Current verification status

The time-dilation, world-tick, and GUI approaches are based on previously tested SifuTAS infrastructure. The redesigned fullscreen layout, versioned 1–9 bindings, local input suppression, speed indicator, and updated pause ownership need a fresh manual in-game pass. No feature should be described as verified from compilation alone.
