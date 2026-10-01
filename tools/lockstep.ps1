# Lockstep check: build with per-instruction cycle accounting, then run the recompiled code
# and the pure interpreter for N frames and compare per-frame RAM hashes. Any divergence is
# a CPU semantics difference between recompiler and interpreter.
#   .\tools\lockstep.ps1 -Frames 1200 [-NoBuild] [-Keys "1600:start,2500:cross"]
param([int]$Frames = 1200, [switch]$NoBuild, [int]$Timeout = 900, [switch]$Checks, [string]$Keys = "")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)

$pkgs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
$llvm = Get-ChildItem $pkgs -Directory -Filter "MartinStorsjo.LLVM-MinGW*" | ForEach-Object { Get-ChildItem $_.FullName -Directory } | Select-Object -First 1
$ninja = Get-ChildItem $pkgs -Directory -Filter "Ninja-build.Ninja*" | Select-Object -First 1
$env:PATH = "$($llvm.FullName)\bin;$($ninja.FullName);C:\Program Files\CMake\bin;$env:PATH"

if (-not $NoBuild) {
    $env:RECOMP_PRECISE = "1"
    python gen/recomp.py --data data --out build/gen_precise | Out-Null
    Remove-Item Env:\RECOMP_PRECISE
    if ($LASTEXITCODE) { throw "recompiler failed" }
    $chk = if ($Checks) { "-DRECOMP_CHECK_CALLS=ON" } else { "-DRECOMP_CHECK_CALLS=OFF" }
    cmake -S . -B build/precise -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGEN_DIR=$PWD/build/gen_precise" $chk `
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ `
        -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres *> build/precise.log
    cmake --build build/precise -j $env:NUMBER_OF_PROCESSORS *>> build/precise.log
    if ($LASTEXITCODE) { throw "build failed (see build/precise.log)" }
}

$exe = "build/precise/gt.exe"
$runs = @(@{ name = "native"; args = @() }, @{ name = "interp"; args = @("--interp") })
foreach ($r in $runs) {
    $log = "build/lockstep_$($r.name).txt"
    $a = @("--turbo", "--frames", "$Frames", "--hash-log", $log) + $r.args
    if ($Keys) { $a += @("--keys", "`"$Keys`"") }
    $p = Start-Process $exe -ArgumentList $a -PassThru -RedirectStandardError "build/lockstep_$($r.name)_err.txt" -RedirectStandardOutput "build/lockstep_out.txt"
    if (-not $p.WaitForExit($Timeout * 1000)) { Stop-Process $p; "$($r.name): TIMEOUT" }
}
$n = Get-Content build/lockstep_native.txt
$i = Get-Content build/lockstep_interp.txt
$count = [Math]::Min($n.Count, $i.Count)
"frames: native $($n.Count), interp $($i.Count)"
for ($k = 0; $k -lt $count; $k++) {
    if ($n[$k] -ne $i[$k]) {
        "FIRST DIVERGENCE at line $k"
        "  native: $($n[$k])"
        "  interp: $($i[$k])"
        if ($k -gt 0) { "  previous (equal): $($n[$k-1])" }
        return
    }
}
"no divergence in $count frames"
