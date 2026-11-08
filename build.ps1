param(
    [switch]$Deploy,
    [switch]$NoGen,
    [string]$LocalBuildName = "epic",



    [string]$GameDir = "C:\Program Files\Epic Games\Sifu\Sifu\Binaries\Win64",




    [string]$ToolchainBin = ""
)

$ErrorActionPreference = "Stop"

$Root    = $PSScriptRoot
$Exe     = Join-Path $GameDir "Sifu-Win64-Shipping.exe"
$Pdb     = Join-Path $GameDir "Sifu-Win64-Shipping.pdb"
$OutDir  = Join-Path $Root "build"



$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [Environment]::GetEnvironmentVariable("Path", "User")

if ($ToolchainBin) { $env:Path = $ToolchainBin + ";" + $env:Path }

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw ("g++ not found. Install the WinLibs UCRT toolchain (portable zip from " +
           "winlibs.com, no winget needed) and pass its bin folder with " +
           "-ToolchainBin `"C:\winlibs\mingw64\bin`".")
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $NoGen) {
    if (-not (Test-Path $Pdb)) { throw "PDB not found at $Pdb" }
    Write-Host "[1/3] regenerating offsets from PDB..." -ForegroundColor Cyan
    $BuildsDir = Join-Path $Root "builds"
    New-Item -ItemType Directory -Force $BuildsDir | Out-Null


    python (Join-Path $Root "tools\pdbdump\pdbdump.py") $Pdb `
        --exe $Exe --emit-build $LocalBuildName (Join-Path $BuildsDir "$LocalBuildName.json")
    if ($LASTEXITCODE -ne 0) { throw "pdbdump failed" }
    python (Join-Path $Root "tools\pdbdump\pdbdump.py") $Pdb `
        --gen (Join-Path $Root "src\core\offsets.g.h") --builds $BuildsDir
    if ($LASTEXITCODE -ne 0) { throw "pdbdump failed" }
}





Write-Host "[2/3] verifying crypto against published test vectors..." -ForegroundColor Cyan
$cryptoTest = Join-Path $OutDir "cryptotest.exe"
cmd /c "g++ -O2 -std=c++17 -static -Wall -Wextra -o `"$cryptoTest`" `"$(Join-Path $Root 'tools\cryptotest\cryptotest.cpp')`" `"$(Join-Path $Root 'src\net\crypto.cpp')`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "cryptotest failed to compile" }
$vectorOutput = cmd /c "`"$cryptoTest`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    $vectorOutput | ForEach-Object { Write-Host $_ }
    throw "crypto test vectors FAILED -- refusing to build an unauthenticated protocol"
}
Write-Host ($vectorOutput | Select-Object -Last 1) -ForegroundColor Green

Write-Host "[3/3] compiling dsound.dll..." -ForegroundColor Cyan

$sources = @(
    "src\dllmain.cpp"
    "src\core\log.cpp"
    "src\core\hooks.cpp"
    "src\tas\controls.cpp"
    "src\tas\input_trace.cpp"
    "src\tas\checkpoint.cpp"
    "src\core\trace.cpp"
    "src\ue\reflection.cpp"
    "src\game\actors.cpp"
    "src\game\coop.cpp"
    "src\game\selftest.cpp"
    "src\game\puppet.cpp"
    "src\game\runstate.cpp"
    "src\game\replay.cpp"
    "src\game\player2.cpp"
    "src\net\crypto.cpp"
    "src\net\session.cpp"
    "src\net\tailscale.cpp"
    "src\\game\\orders.cpp"
    "src\\game\\enemies.cpp"
    "src\\ui\\overlay.cpp"
    "src\\ui\\d3d_overlay.cpp"

    "third_party\imgui\imgui.cpp"
    "third_party\imgui\imgui_draw.cpp"
    "third_party\imgui\imgui_tables.cpp"
    "third_party\imgui\imgui_widgets.cpp"
    "third_party\imgui\backends\imgui_impl_dx11.cpp"
    "third_party\imgui\backends\imgui_impl_win32.cpp"


    "third_party\minhook\src\buffer.c"
    "third_party\minhook\src\hook.c"
    "third_party\minhook\src\trampoline.c"
    "third_party\minhook\src\hde\hde64.c"
    "src\proxy\dsound_proxy.cpp"
) | ForEach-Object { Join-Path $Root $_ }

$dll = Join-Path $OutDir "dsound.dll"









$imguiInc = "-I`"$(Join-Path $Root 'third_party\imgui')`" -I`"$(Join-Path $Root 'third_party\imgui\backends')`""

# The commit is stamped into the DLL and printed in the first log block, so a
# runtime log can be tied to a source revision without guessing.
$commit = "unknown"
try {
    $c = (& git -C $Root rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $c) {
        $commit = $c.Trim()
        $dirty = (& git -C $Root status --porcelain 2>$null)
        if ($dirty) { $commit = "$commit-dirty" }
    }
} catch {}
$flags = "-O2 -std=c++17 -static -static-libgcc -static-libstdc++ $imguiInc " +
         "-DSIFUCOOP_COMMIT=\`"$commit\`" " +
         "-Wall -Wextra -s -lkernel32 -luser32 -lws2_32 -liphlpapi -lpsapi -lgdi32 -ld3d11 -ldxgi " +
         "-ld3dcompiler -ldwmapi"
$srcArgs = ($sources | ForEach-Object { "`"$_`"" }) -join " "
$defFile = Join-Path $Root "src\proxy\dsound.def"
$logFile = Join-Path $OutDir "build-input-trace.log"

cmd /c "g++ -shared -o `"$dll`" $srcArgs `"$defFile`" $flags > `"$logFile`" 2>&1"
$compileOk = ($LASTEXITCODE -eq 0)

if (Test-Path $logFile) {
    $output = Get-Content $logFile
    if ($output) { $output | ForEach-Object { Write-Host $_ } }
}
if (-not $compileOk) { throw "compile failed (see $logFile)" }

$size = [math]::Round((Get-Item $dll).Length / 1KB, 1)
Write-Host "built $dll ($size KB)" -ForegroundColor Green

if ($Deploy) {
    $target = Join-Path $GameDir "dsound.dll"
    if (Test-Path $target) {
        throw "Existing dsound.dll detected at $target. Back up and remove it before deploying SifuTAS."
    }
    Copy-Item $dll $target
    Write-Host "deployed $target" -ForegroundColor Green
}
