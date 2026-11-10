# SifuTAS

SifuTAS is an experimental Windows mod for controlling Sifu gameplay speed, pausing, and world-tick stepping. Savestates, deterministic replay, input recording, and multiplayer are not supported objectives.

It uses the existing SifuCoop-derived DLL proxy, Unreal reflection, PDB offset tooling, and Dear ImGui/Direct3D 11 integration. Runtime work is limited to gameplay-state validation, focused key polling, time dilation, world ticking, local-player input suppression, and GUI rendering.

## Controls

F1 opens and closes the fixed fullscreen SifuTAS interface. Opening it pauses active gameplay. Closing resumes only when SifuTAS caused that pause and the pause state was not changed while the GUI was open.

| Slot | Default | Action | Default setting |
| --- | --- | --- | --- |
| 1 | `1` | Pause / resume | — |
| 2 | `2` | Speed preset | 0.8× |
| 3 | `4` | Speed preset | 0.5× |
| 4 | `5` | Speed preset | 0.2× |
| 5 | `6` | Advance one world tick | — |
| 6 | `7` | Advance configured number of world ticks | 5 |
| 7–8 | Unbound | Additional speed presets | 1.0× each |

The fullscreen GUI uses compact centered panels over a dimmed game view. It has pause, one-tick and multi-tick controls, a 0.1×–2.0× speed slider in 0.1 increments, eight rebindable action slots, five editable speed presets, and an optional persistent speed indicator. Activating a preset sets its speed; activating the same preset again returns to 1.0×. F1 is reserved.

Bindings support ordinary keyboard keys, function/navigation/numpad keys, mouse buttons 1–5, and Ctrl/Alt/Shift combinations. Exact duplicate combinations are rejected. The `APlayerController::InputKey` hook suppresses assigned local-player actions while leaving unmatched events alone. While the GUI is open, local-player key/button events are suppressed and ImGui receives window input. Mouse movement is not hooked while the GUI is closed. These paths require manual in-game verification on the target Sifu build.

The speed hook multiplies Sifu's original `GetEffectiveTimeDilation()` result. It does not overwrite world settings, so native cinematic time effects remain part of the result. Pause skips the current world's `UWorld::Tick`; frame stepping forwards one or more requested world-tick calls at 1/60 s. This is not proof of full-engine pause or emulator-accurate simulation frames.

Controls are guarded by world, player, match, death/game-over, loading, front-end, and Sifu pause state checks. Unsupported states release SifuTAS pause, clear queued steps, and use native 1.0× timing.

## Configuration

`SifuTAS.ini` is stored beside `dsound.dll`. Version 2 settings use a separate `[time_control]` section and save eight bindings, five speed presets, the multi-step count, and indicator visibility. Existing files are preserved: old `[tas]`, `[net]`, and unrelated sections are left in place. Version 1 action indices are ignored; the new defaults are applied to avoid changing the meaning of existing bindings. Invalid or duplicate bindings fall back to defaults. The example file is [SifuTAS.ini.example](SifuTAS.ini.example).

## Build

Build on the Windows computer that has Sifu installed, Python 3, WinLibs UCRT x64 `g++`, and the matching `Sifu-Win64-Shipping.pdb` beside the executable:

```powershell
git clone --branch testing https://github.com/EgeEken/SifuTAS.git
cd SifuTAS
.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam
```

Use `epic` for Epic builds. The script regenerates fingerprinted offsets from the matching PDB, runs the existing crypto test vectors, and builds `build\dsound.dll`. Do not guess offsets or disable executable checks.

## Installation and testing

SifuTAS is tested manually by the user. Codex must never launch, close, kill, or control Sifu. Before replacing the established Steam test DLL, verify Sifu is closed, verify the installed DLL is a known SifuTAS build, create and hash-check a rollback copy, hash-check the new artifact, and preserve `SifuTAS.ini`. Skip installation if any check fails or Sifu is running. Do not overwrite another mod's proxy.

For the manual acceptance procedure, see [docs/TESTING.md](docs/TESTING.md). Build or offline-test success does not establish in-game behavior.

## Development notes

The `testing` branch is the development branch. Commit/push completed passes when authorized; never modify `main` without explicit authorization. Historical savestate, reset, and replay investigations remain in Git history and research docs; they are not active features. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md), [docs/UPSTREAM.md](docs/UPSTREAM.md), and [AGENTS.md](AGENTS.md).

Sifu is owned by Sloclap. This unofficial mod is not affiliated with or endorsed by Sloclap. SifuCoop-derived source remains under the [MIT license](LICENSE); third-party licenses are in [md/THIRD_PARTY.md](md/THIRD_PARTY.md).
