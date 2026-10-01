# One-step build: extract code from the disc, recompile it to C, and build the runtime.
#   .\build.ps1 -Disc "D:\path\Gran Turismo (USA).cue" [-Config Release|Debug]
param(
    [string]$Disc,
    [ValidateSet("Release", "Debug")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$pkgs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
$llvm = Get-ChildItem $pkgs -Directory -Filter "MartinStorsjo.LLVM-MinGW*" |
    ForEach-Object { Get-ChildItem $_.FullName -Directory } | Select-Object -First 1
$ninja = Get-ChildItem $pkgs -Directory -Filter "Ninja-build.Ninja*" | Select-Object -First 1
if ($llvm) { $env:PATH = "$($llvm.FullName)\bin;$env:PATH" }
if ($ninja) { $env:PATH = "$($ninja.FullName);$env:PATH" }
$env:PATH = "C:\Program Files\CMake\bin;$env:PATH"

if ($Disc) {
    python tools/extract.py $Disc data
    if ($LASTEXITCODE) { throw "extract failed" }
    Set-Content -Path gt_disc.txt -Value $Disc -NoNewline
}
if (-not (Test-Path data/exe/exes.json)) { throw "run with -Disc first" }

python gen/recomp.py --data data --out build/gen
if ($LASTEXITCODE) { throw "recompiler failed" }

$bdir = "build/$($Config.ToLower())"
cmake -S . -B $bdir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang" "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++" `
    "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres"
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $bdir -j $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE) { throw "build failed" }
Copy-Item "$bdir/gt.exe" . -Force
Write-Host "Built gt.exe"
