# Build a debug variant of the runtime + generated code into build/<Name>.
#   .\tools\build_variant.ps1 -Name nr -Env @{RECOMP_PRECISE="1"} [-CMakeArgs @("-DRECOMP_CHECK_CALLS=ON")]
param([Parameter(Mandatory)][string]$Name, [hashtable]$Env = @{}, [string[]]$CMakeArgs = @())
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$pkgs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
$llvm = Get-ChildItem $pkgs -Directory -Filter "MartinStorsjo.LLVM-MinGW*" | ForEach-Object { Get-ChildItem $_.FullName -Directory } | Select-Object -First 1
$ninja = Get-ChildItem $pkgs -Directory -Filter "Ninja-build.Ninja*" | Select-Object -First 1
$env:PATH = "$($llvm.FullName)\bin;$($ninja.FullName);C:\Program Files\CMake\bin;$env:PATH"
$saved = @{}
foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Env[$k]) }
try {
    python gen/recomp.py --data data --out "build/gen_$Name" | Out-Null
    if ($LASTEXITCODE) { throw "recompiler failed" }
} finally {
    foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}
cmake -S . -B "build/$Name" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGEN_DIR=$PWD/build/gen_$Name" @CMakeArgs `
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ `
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres *> "build/$Name.log"
cmake --build "build/$Name" -j $env:NUMBER_OF_PROCESSORS *>> "build/$Name.log"
if ($LASTEXITCODE) { throw "build failed (see build/$Name.log)" }
"built build/$Name/gt.exe"
