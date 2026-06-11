# Build the re-roller berkutx_rng.exe (.NET 8, self-contained single-file, win-x86 — RID/options in the csproj).
# Only the authored arena .sg is embedded; the tool parses the game's dBASE tables itself.
# Cross-builds the Windows exe on any OS that has the .NET SDK (used by CI on Linux).
param([string]$OutDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    dotnet publish berkutx_rng.csproj -c Release -o $OutDir
    if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
    Write-Host ("berkutx_rng.exe  {0,7} bytes" -f (Get-Item (Join-Path $OutDir 'berkutx_rng.exe')).Length)
} finally { Pop-Location }
