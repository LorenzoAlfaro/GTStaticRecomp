# Combine BMP screenshots from a directory into a grid PNG.
#   .\tools\montage.ps1 -Dir build/shots -Out build/montage.png [-Cols 4] [-Width 384]
param([string]$Dir = "build/shots", [string]$Out = "build/montage.png", [int]$Cols = 4, [int]$Width = 384)
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem $Dir -Filter *.bmp | Sort-Object Name
if (-not $files) { "no shots"; return }
$first = [System.Drawing.Image]::FromFile($files[0].FullName)
$h = [int]($Width * $first.Height / $first.Width) + 14
$first.Dispose()
$rows = [math]::Ceiling($files.Count / $Cols)
$m = New-Object System.Drawing.Bitmap ($Width * $Cols), ($h * $rows)
$g = [System.Drawing.Graphics]::FromImage($m)
$g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font "Consolas", 8
$k = 0
foreach ($f in $files) {
    $img = [System.Drawing.Image]::FromFile($f.FullName)
    $x = ($k % $Cols) * $Width; $y = [math]::Floor($k / $Cols) * $h
    $g.DrawImage($img, $x, $y + 14, $Width, $h - 14)
    $g.DrawString($f.BaseName, $font, [System.Drawing.Brushes]::White, $x + 2, $y)
    $img.Dispose(); $k++
}
$m.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"montage: $Out ($($files.Count) shots)"
