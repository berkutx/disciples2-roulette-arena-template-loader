# Build everything into dist/ — the three files to drop into the Disciples II game folder:
#   berkutx_loader.exe  (injector)   hook.dll  (the hook)   berkutx_rng.exe  (re-roller)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
& (Join-Path $root 'loader/build.ps1')   -OutDir $dist
& (Join-Path $root 'reroller/build.ps1') -OutDir $dist
Write-Host "`ndist ($dist):"
Get-ChildItem $dist | ForEach-Object { Write-Host ("  {0,-20} {1,8} bytes" -f $_.Name, $_.Length) }
