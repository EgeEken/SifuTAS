# Upstream provenance

SifuTAS testing began from a **source import** of [Zanci19/SifuCoop](https://github.com/Zanci19/SifuCoop), commit `78c438e9d6f21afb16647a4600e5d1e2316aa859` (2026-10-26). This is a **selected source snapshot**, not a GitHub fork relationship or a full history import.

Imported: `src/`, the native `dsound.dll` proxy and Unreal reflection code, hook/overlay code, `tools/pdbdump/`, `tools/cryptotest/`, `builds/*.json`, and the specific Dear ImGui / MinHook sources and headers used to compile the DLL. Upstream multiplayer distribution binaries, launcher, installer, long research notes and unused third-party examples were intentionally not copied.

The initial SifuTAS-specific changes are in `src/core/hooks.cpp` (hotkey polling, time dilation, experimental `UWorld::Tick` interception), `tools/pdbdump/pdbdump.py` (new `UWorld_Tick` symbol), and `build.ps1` (DLL-only build). The rest is largely upstream code for future cleanup.

The original project is licensed under MIT. Preserve [LICENSE](../LICENSE) and the associated third-party licenses at `third_party/imgui/LICENSE.txt` and `third_party/minhook/LICENSE.txt`. See [upstream's third-party notes](../md/THIRD_PARTY.md).

No Sloclap game binaries, PDBs or assets are included.
