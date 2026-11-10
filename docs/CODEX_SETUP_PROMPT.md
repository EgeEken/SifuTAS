# Local SifuTAS development workflow

Use Codex on the Windows computer with the installed Sifu files and matching PDB. The active project scope is a dedicated time-control utility: speed from 0.1× to 2.0×, five configurable speed presets, pause, world-tick stepping, a compact fullscreen ImGui interface, configurable keyboard/mouse bindings, and an optional persistent speed indicator. Do not resume savestates, deterministic replay, recording, or multiplayer work.

Before editing, read `AGENTS.md`, `README.md`, and `docs/TESTING.md`. Preserve local changes. Work only on `testing`; never modify `main` without explicit authorization.

Build against the locally installed Sifu PDB:

```powershell
.\build.ps1 -GameDir "C:\path\to\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam
```

The build script regenerates PDB-based offsets, checks crypto test vectors, and compiles the proxy DLL. Do not guess offsets, weaken executable fingerprint guards, or commit proprietary files.

Never launch, close, kill, or control Sifu. Runtime tests are performed manually by the user. For deployment, follow the standing authorization and all rollback/hash/configuration safeguards in `AGENTS.md` and `docs/TESTING.md`; skip installation if Sifu is running or any verification fails. Keep changes focused, report offline versus in-game evidence separately, and report the commit SHA for authorized completed passes.
