# SifuTAS

**Experimental Windows mod for Sifu — development build, not a release.**

Goal: tool-assisted, choreographed combat footage with precise slowdown, frame stepping, and eventually input editing, replay, checkpoints and savestates. This is for creative gameplay videos, **not** verified competitive TAS speedrun files.

The `testing` branch is the development branch. **Do not install from `main` or publish a release yet.** Slowdown, pause, frame stepping, gameplay mouse behavior, the dedicated GUI, configurable controls, shortcut rebinding, and settings persistence were tested in-game by the user. Checkpoint v1 is implemented and compiled, but has not yet been installed or tested in-game.

## Current prototype

| Key | Intended action | Status |
| --- | --- | --- |
| F1 | Pause/resume world simulation | Previously tested working |
| F2 | Advance the configured 1–10 world ticks while paused | Previously tested at one tick; multi-tick setting awaits test |
| F3 | Select/toggle speed preset 1 (default 0.5×) | Previously tested working at default values |
| F4 | Select/toggle speed preset 2 (default 0.2×) | Previously tested working at default values |
| F5 | Select/toggle speed preset 3 (default 0.1×) | Previously tested working at default values |
| F6 | Toggle SifuTAS GUI | User tested working |
| F7 | Save the in-memory checkpoint | Implemented; awaits in-game test |
| F8 | Load the in-memory checkpoint | Implemented; awaits in-game test |

Mouse capture and camera sensitivity were also reported fixed. Speed presets multiply Sifu's effective world time dilation. A frame step temporarily bypasses the TAS multiplier so it advances the same 1/60 s world tick at every selected preset.

The GUI has Playback, Savestates, Recording, and Settings tabs. The Savestates tab exposes one in-memory checkpoint slot; the Recording tab remains unavailable. The inherited SifuCoop menu and external overlay are not drawn. ImGui cursor drawing is enabled only while the GUI is open; the old cursor positioning and confinement hooks remain disabled.

Bindings, speed multipliers, and frame-step increment are read from and saved to `SifuTAS.ini` alongside `dsound.dll`. If absent, the mod creates it with defaults on startup. Shortcuts support F1–F12, optionally combined with Ctrl, Alt, or Shift; duplicate bindings are rejected. See [SifuTAS.ini.example](SifuTAS.ini.example) for the default values.

The Playback tab's engine tick counter is not a verified simulation-frame count. Checkpoint v1 is restricted to a paused training-room world, the same player and nearest tracked enemy, both alive/standing/quiet, and no active montage. It stores transforms, velocity, health/max health, guard, actor time dilation, and timeline reference metadata. Loading remains paused. It does not restore movement mode, behavior-tree state, pending enemy actions, timers, or callback queues. The checkpoint reference preserves fields for level, actor identities, engine tick, future verified simulation frame, and recording branch ID. Input recording and deterministic replay are not implemented.

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

For now, **install manually after reviewing the game directory**:

1. Close Sifu and back up any existing `dsound.dll` or related mod files. Do not overwrite a different mod's DLL or stack multiple `dsound.dll` proxies.
2. Copy `build\dsound.dll` beside `Sifu-Win64-Shipping.exe`. Optionally copy `SifuCoop.ini.example` there as `SifuCoop.ini` (`[net] mode=off`).
3. Launch the game normally, offline, and test a short training encounter. Read `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log` for `tas:` lines.
4. To uninstall, remove this project's `dsound.dll` and restore the backed-up mod files.

The build script has an optional `-Deploy` flag, but **it deliberately refuses to overwrite an existing** `dsound.dll`. Do not deploy or replace an installed DLL without explicit approval.

## Development and Codex

- [Windows/Codex local setup prompt](docs/CODEX_SETUP_PROMPT.md)
- [Testing procedure and known risks](docs/TESTING.md)
- [Architecture, development priorities, and longer-term TAS design](docs/DEVELOPMENT.md)
- [Upstream attribution and provenance](docs/UPSTREAM.md)
- [Agent instructions](AGENTS.md)

Forked from SifuCoop's native hooking infrastructure, not rebuilt from scratch. The initial import retains upstream code for networking and co-op, but this project is intended for **single-player/offline use only**. Co-op functionality has not been removed yet and is not supported by SifuTAS.

Sifu is owned by Sloclap. This is an unofficial mod, not affiliated with or endorsed by Sloclap. License: [MIT for SifuCoop-derived source](LICENSE), with [third-party licenses](md/THIRD_PARTY.md).
