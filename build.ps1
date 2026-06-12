# Build the single self-contained file to drop into the Disciples II game folder:
#   berkutx_loader.exe  — injector with hook.dll + berkutx_rng.exe embedded (unpacked at runtime).
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$dist  = Join-Path $root 'dist'
$stage = Join-Path $root 'build-stage'
foreach ($d in $dist, $stage) { if (Test-Path $d) { Remove-Item (Join-Path $d '*') -Recurse -Force -ErrorAction SilentlyContinue } }
New-Item -ItemType Directory -Force -Path $dist, $stage | Out-Null
# the re-roller first (the loader embeds it), then the loader embeds hook.dll + the re-roller
& (Join-Path $root 'reroller/build.ps1') -OutDir $stage
& (Join-Path $root 'loader/build.ps1')   -OutDir $dist -RngExe (Join-Path $stage 'berkutx_rng.exe')
Write-Host "`ndist ($dist):"
Get-ChildItem $dist | ForEach-Object { Write-Host ("  {0,-22} {1,9} bytes" -f $_.Name, $_.Length) }
