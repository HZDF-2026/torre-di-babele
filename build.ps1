# build.ps1 — Windows build (MinGW-w64). Requires g++ / mingw32-make on PATH.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
mingw32-make all
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built dist\greenroom.exe"
