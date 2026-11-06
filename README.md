# SifuTAS

**Experimental Windows mod for Sifu — development build, not a release.**

Goal: tool-assisted, choreographed combat footage with precise slowdown, frame stepping, and eventually input editing, replay, checkpoints and savestates. This is for creative gameplay videos, **not** verified competitive TAS speedrun files.

The `testing` branch is the development branch. **Do not install from `main` or publish a release yet.** Slowdown, pause, frame stepping, gameplay mouse behavior, the dedicated GUI, configurable controls, shortcut rebinding, settings persistence, GUI scaling, and the continuous speed slider were reported working in-game. The previous checkpoint test in Free Training failed because the offline enemy row had zero cached health. This pass uses current-world fighter enumeration and direct combat-component reads. It awaits in-game verification.

## Current prototype

| Key | Intended action | Status |
| --- | --- | --- |
| F1 | Pause/resume world simulation | Previously tested working |
| F2 | Advance the configured 1–10 world ticks while paused | Previously tested at one tick; multi-tick setting awaits test |
| F3 | Select/toggle speed preset 1 (default 0.5×) | Previously tested working at default values |
| F4 | Select/toggle speed preset 2 (default 0.2×) | Previously tested working at default values |
| F5 | Select/toggle speed preset 3 (default 0.1×) | Previously tested working at default values |
| F6 | Toggle SifuTAS GUI and pause on open | GUI tested; automatic pause awaits test |
| F7 | Save the in-memory checkpoint | Implemented; awaits in-game test |
| F8 | Load the in-memory checkpoint | Implemented; awaits in-game test |

Mouse capture and camera sensitivity were also reported fixed. Speed presets multiply Sifu's effective world time dilation. A frame step temporarily bypasses the TAS multiplier so it advances the same 1/60 s world tick at every selected preset.

The GUI has Playback, Savestates, Recording, and Settings tabs. F6 pauses on open only if gameplay was running; closing resumes only if the user has not changed pause state since opening. Playback uses a continuous 0.01×–4.0× slider; F3–F5 remain configurable presets sharing the same current speed. GUI text, spacing, buttons and initial window dimensions scale together; the scale is saved in `SifuTAS.ini`. The Savestates tab exposes one in-memory checkpoint slot; the Recording tab remains unavailable. The inherited SifuCoop menu and external overlay are not drawn. ImGui cursor drawing is enabled only while the GUI is open; the old cursor positioning and confinement hooks remain disabled.

Bindings, speed multipliers, and frame-step increment are read from and saved to `SifuTAS.ini` alongside `dsound.dll`. If absent, the mod creates it with defaults on startup. Shortcuts support F1–F12, optionally combined with Ctrl, Alt, or Shift; duplicate bindings are rejected. See [SifuTAS.ini.example](SifuTAS.ini.example) for the default values.

The Playback tab's engine tick counter is not a verified simulation-frame count. Checkpoint v1 has no map-name restriction. It enumerates up to 256 current-world `AFightingCharacter` candidates and captures up to 32 distinct actors plus the player; larger or unavailable enumerations are marked incomplete. Field availability is stored per actor for transform, velocity, health/max health, guard, down/death flags, time dilation, and montage/playhead. Moving and downed actors do not prevent saving. Loading checks the same world and level, actor pointer, object path/class, and current-world membership. It restores supported fields per character with readback, skipping missing actors and reporting partial state. Animation/combat actions, down/death transitions, movement mode, AI queues, timers, and callbacks are not restored; loading remains paused. The checkpoint reference retains level, player/enemy identity, engine tick, future verified simulation frame, and recording branch ID. Input recording and deterministic replay are not implemented.

**Acceleration status:** the diagnostic log from the user's extended test did not reproduce the reported gradual acceleration. It recorded native time-dilation transitions from one caller RVA, but the values returned to 1.0 and did not show a monotonic increase. Later samples had a frozen world-time value while real time advanced, so that portion does not establish normal simulated gameplay speed. No cause or fix has been confirmed.

Frame stepping does **not** yet guarantee that input is sampled once, all subsystems are paused, or animation/physics are synchronized. Checkpoint restoration is experimental and its runtime reliability is not yet established. It is not deterministic replay. There is **no input recorder or input editor**.

## Build on Windows

Requires a legitimately installed Windows copy of Sifu, Python 3, a working WinLibs UCRT x64 `g++` toolchain, and **the matching** `Sifu-Win64-Shipping.pdb` alongside `Sifu-Win64-Shipping.exe`. The matching PDB is needed to regenerate executable-specific hooks. Do not download or share game binaries or PDBs through this repository.

```powershell
git clone --branch testing https://github.com/EgeEken/SifuTAS.git
cd SifuTAS
.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam
```

Use `-LocalBuildName epic` for Epic. The output is `build\dsound.dll`. **Do not use `-NoGen` on the first build.** If the PDB is absent, stop and report that blocker; do not guess offsets, disable build fingerprint checks, or use addresses from another executable.

For the established Steam test installation, follow the standing authorization conditions below. For other installations, review the game directory and get explicit permission before replacing a DLL:

1. Close Sifu and back up any existing `dsound.dll` or related mod files. Do not overwrite a different mod's DLL or stack multiple `dsound.dll` proxies.
2. Copy `build\dsound.dll` beside `Sifu-Win64-Shipping.exe`. Optionally copy `SifuCoop.ini.example` there as `SifuCoop.ini` (`[net] mode=off`).
3. Launch the game normally, offline, and test a short training encounter. Read `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` for `tas:` lines.
4. To uninstall, remove this project's `dsound.dll` and restore the backed-up mod files.

The build script has an optional `-Deploy` flag, but **it deliberately refuses to overwrite an existing** `dsound.dll`. Standing authorization permits this project to replace only the established Steam SifuTAS test DLL after verifying the current file is a known SifuTAS build, Sifu is closed, a rollback backup is hash-verified, and the new DLL hash matches. Stop if the target is missing, unexpected, or may belong to another mod.

## Development and Codex

- [Windows/Codex local setup prompt](docs/CODEX_SETUP_PROMPT.md)
- [Testing procedure and known risks](docs/TESTING.md)
- [Architecture, development priorities, and longer-term TAS design](docs/DEVELOPMENT.md)
- [Upstream attribution and provenance](docs/UPSTREAM.md)
- [Agent instructions](AGENTS.md)

Forked from SifuCoop's native hooking infrastructure, not rebuilt from scratch. The initial import retains upstream code for networking and co-op, but this project is intended for **single-player/offline use only**. Co-op functionality has not been removed yet and is not supported by SifuTAS.

Sifu is owned by Sloclap. This is an unofficial mod, not affiliated with or endorsed by Sloclap. License: [MIT for SifuCoop-derived source](LICENSE), with [third-party licenses](md/THIRD_PARTY.md).
