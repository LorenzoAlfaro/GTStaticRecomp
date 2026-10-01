# Run the game headless-ish for N frames, save a screenshot (PNG) and show the log tail.
#   .\tools\test.ps1 -Frames 600 [-Out shot.png] [-Timeout 120] [-Verbose]
param([int]$Frames = 300, [string]$Out = "build/shot.png", [int]$Timeout = 120, [switch]$V, [int]$Tail = 40)
Set-Location (Split-Path $PSScriptRoot)
$bmp = [IO.Path]::ChangeExtension($Out, ".bmp")
foreach ($f in @($bmp, $Out)) { if (Test-Path $f) { [IO.File]::Delete((Resolve-Path $f)) } }
$args = @("--turbo", "--frames", "$Frames", "--shot", "$Frames", $bmp)
if ($V) { $args += "-v" }
$p = Start-Process .\ctr.exe -ArgumentList $args -PassThru -RedirectStandardError build/run_err.txt -RedirectStandardOutput build/run_out.txt
if (-not $p.WaitForExit($Timeout * 1000)) { Stop-Process $p; "TIMEOUT" }
"exit code: 0x{0:X}" -f $p.ExitCode
if (Test-Path $bmp) {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile((Resolve-Path $bmp))
    $img.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    "screenshot: $Out"
}
Get-Content build/run_err.txt -Tail $Tail
