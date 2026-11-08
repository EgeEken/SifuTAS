# SifuTAS

**Experimental Windows mod for Sifu — development build, not a release.**

Goal: tool-assisted, choreographed combat footage with precise slowdown, frame stepping, and eventually input editing, replay, checkpoints and savestates. This is for creative gameplay videos, **not** verified competitive TAS speedrun files.

The `testing` branch is the development branch. **Do not install from `main` or publish a release yet.** Slowdown, pause, frame stepping, gameplay mouse behavior, configurable controls, settings persistence, GUI scaling, and the continuous speed slider were reported working in-game. The existing checkpoint restores only selected actor fields; combat actions and encounter progression are not restored. It is labeled an experimental state snapshot. Camera motion after resume and combat-action restoration remain unresolved. This pass adds a native Sifu replay recorder in the GUI and a stricter active-match guard; both await in-game verification.

## Current prototype

| Key | Intended action | Status |
| --- | --- | --- |
| F1 | Pause/resume world simulation | Previously tested working |
| F2 | Advance the configured 1–10 world ticks while paused | Previously tested at one tick; multi-tick setting awaits test |
| F3 | Select/toggle speed preset 1 (default 0.5×) | Previously tested working at default values |
| F4 | Select/toggle speed preset 2 (default 0.2×) | Previously tested working at default values |
| F5 | Select/toggle speed preset 3 (default 0.1×) | Previously tested working at default values |
| F6 | Toggle SifuTAS GUI and pause on open during validated active play | GUI tested; safe-state pause behavior awaits test |
| F7 | Save an experimental state snapshot | Partial actor fields only; not a gameplay savestate |
| F8 | Load an experimental state snapshot | Combat actions and encounter progression remain live |
| F10 | Start/stop Sifu's native replay recording | New GUI control; Free Training eligibility awaits in-game test |

Mouse capture and camera sensitivity were also reported fixed. Speed presets multiply Sifu's effective world time dilation. A frame step temporarily bypasses the TAS multiplier so it advances the same 1/60 s world tick at every selected preset.

The GUI has Playback, Experimental Snapshot, Recording, and Settings tabs. F6 pauses on open only if gameplay was running; closing resumes only if the GUI-owned pause remains unchanged. Playback uses a continuous 0.01×–4.0× slider; F3–F5 remain configurable presets sharing the same selected speed. TAS pause, frame advance, and speed controls require a valid world, player, and Unreal match state of `InProgress`, in addition to the existing death/game-over, world-transition, Sifu pause, and front-end checks. Results screens such as `WaitingPostMatch` are unsupported. In unsupported states effective TAS speed is 1.0×, pending steps are cleared, and TAS pause is released. GUI text, spacing, buttons and initial window dimensions scale together; the scale is saved in `SifuTAS.ini`. The Recording tab uses Sifu's native replay system through the GUI or F10, with asynchronous startup/finalization status; it is not an input recorder. The inherited SifuCoop menu and external overlay are not drawn. ImGui cursor drawing is enabled only while the GUI is open; the old cursor positioning and confinement hooks remain disabled.

Bindings, speed multipliers, and frame-step increment are read from and saved to `SifuTAS.ini` alongside `dsound.dll`. If absent, the mod creates it with defaults on startup. Shortcuts support F1–F12, optionally combined with Ctrl, Alt, or Shift; duplicate bindings are rejected. See [SifuTAS.ini.example](SifuTAS.ini.example) for the default values.

The Playback tab's engine tick counter is not a verified simulation-frame count. The experimental state snapshot has no map-name restriction. It enumerates up to 256 current-world `AFightingCharacter` objects, then excludes invalid, pooled (below the existing pooled-Z threshold), health-less, and inactive/dead candidates before capturing up to 32 active candidates plus the player. It reports raw world count, active candidates, excluded categories, identity truncation/lookup failures, and capacity skips. Actor and reference identities use 512-byte buffers. The matching Steam PDB resolves `FMemory::Free`, which releases the Unreal-owned `FString` returned by `GetPathName`; this also covers other callers of that helper. Per-actor state includes transform, velocity, health/max health, guard, down/death flags, time dilation, and montage/playhead availability. The snapshot does not restore combat actions or encounter progression. The new shared TAS-control guard also refuses unsupported states, restores an effective 1.0× TAS multiplier, releases TAS pause, and clears pending steps if state validation fails; it is not yet in-game verified.

The compact GUI keeps Pause/Resume, Save, and Load visible with one short status. Detailed enumeration, identities, field masks, and camera/cloth results are under Advanced/Diagnostics. Camera diagnostics now log capture, pre-load, post-load, and six post-resume samples, including controller rotation, camera caches, view-target/pending-target pointers and POV, target offset, and camera-manager transform component where readable. The earlier log only verified camera-cache writes while paused; it had no post-resume samples, so the repeatable camera jump remains undiagnosed. Pending mouse input is not measured. Cloth reset requests do not restore saved cloth-particle history. AI, combat actions, movement mode, timers, callbacks, and down/death transitions remain outside the snapshot; doors, weapons, destructibles, scripted triggers, and newly spawned enemies are not restored. Input recording and deterministic replay are not implemented.

The previous test log measured `OnFrame` at roughly 57–91 μs average, with enemy updates around 9–36 μs and puppet updates around 40–48 μs. One 5-second interval peaked at 4.53 ms in enemy processing; most intervals peaked below 1 ms. Checkpoint loads measured about 0.68–0.82 ms and one save 1.83 ms. These are mod-side timings, not a full-game frame-time measurement. Across three consecutive memory samples, private bytes rose from about 3.89 GiB to 3.93 GiB while working set fell from about 3.99 GiB to 3.89 GiB. That short, fluctuating series cannot attribute growth to SifuTAS. The previously acknowledged `GetPathName` leak was confirmed in source and its compatible matching-PDB deallocator is now called. The new build's memory behavior remains untested. Repeated saves still overwrite one fixed-size snapshot.

**Acceleration status:** the diagnostic log from the user's extended test did not reproduce the reported gradual acceleration. It recorded native time-dilation transitions from one caller RVA, but the values returned to 1.0 and did not show a monotonic increase. Later samples had a frozen world-time value while real time advanced, so that portion does not establish normal simulated gameplay speed. No cause or fix has been confirmed.

Frame stepping does **not** yet guarantee that input is sampled once, all subsystems are paused, or animation/physics are synchronized. The experimental snapshot does not rewind encounter progression. The prototype does not support deterministic playback or arbitrary-frame savestates. There is **no input recorder or input editor**.

## Native replay experiment

The matching Steam PDB confirms `UReplaySystem` and its native/reflected eligibility, start/stop, recording-status, demo-driver, and replay-length APIs. Its generated class is in the Sifu script package; the mod resolves `/Script/Sifu.Default__ReplaySystem` and separately checks `BPF_GetReplaySystem` for the live instance. The CDO name follows Unreal's reflected class name, which omits the native `U` prefix. Runtime CDO resolution and recording remain unverified. Recording controls are available in the Recording tab and by the configurable F10 action; no `SifuCoop.ini` opt-in is required. A request is not reported as active until Sifu reports recording and a valid demo driver. Stop waits for recording shutdown and a nonzero finalized replay length, then directs the user to Sifu's Replay Editor. Eligibility remains controlled by Sifu's `BPF_IsRecordingDisabled` and `BPF_CanStartRecording` checks. Free Training support, the recorded contents, editor visibility, seek fidelity, and return to live gameplay still need in-game verification. This is Sifu's native replay stream, not an input timeline or deterministic TAS playback. Which animation/gameplay state is replicated and whether recording time follows TAS dilation or wall time remain unverified.

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
