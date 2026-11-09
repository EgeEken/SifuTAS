# SifuTAS

**Experimental Windows mod for Sifu — development build, not a release.**

Goal: tool-assisted, choreographed combat footage with precise slowdown. The final process-level experiment demonstrated snapshot capture but not safe rollback; arbitrary-frame savestates are not being pursued under the current user-mode architecture. F7/F8 remain a legacy partial actor snapshot, not a gameplay savestate. This is for creative gameplay videos, **not** verified competitive TAS speedrun files.

The `testing` branch is the development branch. **Do not install from `main` or publish a release yet.** Slowdown, pause, frame stepping, gameplay mouse behavior, configurable controls, settings persistence, GUI scaling, and the continuous speed slider were reported working in-game. The independent PSS experiment preserved memory and thread contexts in a snapshot clone, but found an additional unregistered thread starting in `ntdll.dll` and correctly refused restoration. No process rollback or graphics-state strategy was demonstrated. See [process rollback feasibility](docs/PROCESS_ROLLBACK_FEASIBILITY.md).

## Current prototype

| Key | Intended action | Status |
| --- | --- | --- |
| F1 | Pause/resume world simulation | Previously tested working |
| F2 | Advance the configured 1–10 world ticks while paused | Previously tested at one tick; multi-tick setting awaits test |
| F3 | Select/toggle speed preset 1 (default 0.5×) | Previously tested working at default values |
| F4 | Select/toggle speed preset 2 (default 0.2×) | Previously tested working at default values |
| F5 | Select/toggle speed preset 3 (default 0.1×) | Previously tested working at default values |
| F6 | Toggle SifuTAS GUI and pause on open during validated active play | GUI tested; safe-state pause behavior awaits test |
| F7 | Save an experimental state snapshot | Saves selected fields and each uniquely associated enemy spawner path where available |
| F8 | Load an experimental state snapshot | Tries exact actor identity, then a strictly validated actor from the saved spawner; untested after manual reset |
| F10 | Start/stop Sifu's native replay recording | New GUI control; Free Training eligibility awaits in-game test |
| Ctrl+Alt+F12 | Trigger an armed reset trace immediately before Reset Situation | Configurable; does not consume the key; native reset hooks are monitored too |

Mouse capture and camera sensitivity were also reported fixed. Speed presets multiply Sifu's effective world time dilation. A frame step temporarily bypasses the TAS multiplier so it advances the same 1/60 s world tick at every selected preset.

The GUI has Playback, Experimental Snapshot, Recording, and Settings tabs. F6 pauses on open only if gameplay was running; closing resumes only if the GUI-owned pause remains unchanged. Playback uses a continuous 0.01×–4.0× slider; F3–F5 remain configurable presets sharing the same selected speed. TAS pause, frame advance, and speed controls require a valid world, player, and Unreal match state of `InProgress`, in addition to the existing death/game-over, world-transition, Sifu pause, and front-end checks. Results screens such as `WaitingPostMatch` are unsupported. In unsupported states effective TAS speed is 1.0×, pending steps are cleared, and TAS pause is released. GUI text, spacing, buttons and initial window dimensions scale together; the scale is saved in `SifuTAS.ini`. The Recording tab has Sifu's native replay controls through the GUI or F10, plus a separate bounded five-second input/tick diagnostic trace. The trace logs player-controller and local `UPlayerInput` key/axis callbacks, input-stack boundaries, and engine/world tick boundaries to `SifuCoop.log`; it does not inject inputs or modify timing. The native replay experiment successfully activated recording and produced a 17-second entry visible in Replay Management, but the mod's stop/finalization poll timed out. The entry's contents and playback fidelity are not verified. The inherited SifuCoop menu and external overlay are not drawn. ImGui cursor drawing is enabled only while the GUI is open; the old cursor positioning and confinement hooks remain disabled.

Bindings, speed multipliers, and frame-step increment are read from and saved to `SifuTAS.ini` alongside `dsound.dll`. If absent, the mod creates it with defaults on startup. Shortcuts support F1–F12, optionally combined with Ctrl, Alt, or Shift; duplicate bindings are rejected. See [SifuTAS.ini.example](SifuTAS.ini.example) for the default values.

The Playback tab's engine tick counter is not a verified simulation-frame count. The legacy experimental snapshot has no map-name restriction. It enumerates up to 256 current-world `AFightingCharacter` objects, excludes invalid, pooled, health-less, and inactive/dead candidates, then captures up to 32 active candidates plus the player. It also enumerates up to 256 current-world `AAISpawner` objects. Each enemy records a full spawner path/class only when exactly one spawner's `BPF_GetSpawnedAI` result is that actor; missing, ambiguous, or truncated associations are reported and mark the snapshot incomplete. On load, exact actor identity is preferred. If it fails, a replacement is accepted only through the unique saved spawner in the same world/level, with matching class, current fighter membership, active/living/idle state, and initialized attack/defense/health components. Components are resolved again from the target; no saved component pointers are reused. This rebinding path is not yet tested in-game and restores only the existing selected fields, not combat actions or encounter progression. It is not a gameplay savestate.

The compact GUI keeps Pause/Resume, Save, and Load visible with one short status. Advanced/Diagnostics uses an arm-then-trigger ProcessEvent trace with a 512-call rolling pre-trigger ring, up to 1536 detailed post-trigger calls, and aggregates for up to 256 function/class pairs over two seconds. Accepting the marker now shows a brief non-interactive notification while the GUI is closed. Function names are resolved and logged after capture. Since current input hooks cannot identify Reset Situation reliably, press the configurable Ctrl+Alt+F12 marker immediately before the game's reset command; native actor/game-mode hooks are also monitored. The trace and actor inventory do not prove reset initialization is complete. The PDB confirms `UOrderComponent::BPF_CancelAllOrders()` queues `OrderManager::CancelAllOrdersAsynch`; its animation, AI, callback, network, and paused-world effects are unknown, so SifuTAS does not invoke it. `StopLogic/RestartLogic` and combo reset are also not called. No combat-action, timer, callback, physics-history, or encounter-progression restore is implemented.

The previous test log measured `OnFrame` at roughly 57–91 μs average, with enemy updates around 9–36 μs and puppet updates around 40–48 μs. One 5-second interval peaked at 4.53 ms in enemy processing; most intervals peaked below 1 ms. Checkpoint loads measured about 0.68–0.82 ms and one save 1.83 ms. These are mod-side timings, not a full-game frame-time measurement. Across three consecutive memory samples, private bytes rose from about 3.89 GiB to 3.93 GiB while working set fell from about 3.99 GiB to 3.89 GiB. That short, fluctuating series cannot attribute growth to SifuTAS. The previously acknowledged `GetPathName` leak was confirmed in source and its compatible matching-PDB deallocator is now called. The new build's memory behavior remains untested. Repeated saves still overwrite one fixed-size snapshot.

**Acceleration status:** the diagnostic log from the user's extended test did not reproduce the reported gradual acceleration. It recorded native time-dilation transitions from one caller RVA, but the values returned to 1.0 and did not show a monotonic increase. Later samples had a frozen world-time value while real time advanced, so that portion does not establish normal simulated gameplay speed. No cause or fix has been confirmed.

Frame stepping does **not** yet guarantee that input is sampled once, all subsystems are paused, or animation/physics are synchronized. The experimental snapshot does not rewind encounter progression. There is **no input recorder or input editor**. See the process rollback report for the final arbitrary-frame savestate feasibility decision.

## Native replay experiment

The matching Steam PDB confirms `UReplaySystem` and its native/reflected eligibility, start/stop, recording-status, demo-driver, and replay-length APIs. Its generated class is in the Sifu script package; the mod resolves `/Script/Sifu.Default__ReplaySystem` and separately checks `BPF_GetReplaySystem` for the live instance. The CDO name follows Unreal's reflected class name, which omits the native `U` prefix. Runtime testing confirmed that recording became active and that Sifu created a 17-second Wuguan entry visible in Replay Management. The mod's stop/finalization poll timed out before reporting completion, so clean finalization and replay contents remain unverified. This is Sifu's native replay stream, not an input timeline or deterministic TAS playback.

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
