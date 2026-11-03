# Copies CHDK_Rewired's bend engine into native/engine, so the a6000 bends exactly as the PowerShots do.
# The four files are used unmodified; camera.h there is this project's own stand-in and is left alone.
#
#   powershell -ExecutionPolicy Bypass -File tools\sync-engine.ps1 [path\to\CHDK_Rewired\chdk-src]
param([string]$Chdk = (Join-Path $PSScriptRoot '..\..\CHDK_Rewired\chdk-src'))
$ErrorActionPreference = 'Stop'
$dst = Join-Path $PSScriptRoot '..\native\engine'
Copy-Item (Join-Path $Chdk 'core\bend.c'), (Join-Path $Chdk 'core\bendx.c'), `
          (Join-Path $Chdk 'include\bend.h'), (Join-Path $Chdk 'include\bendx.h') $dst
Write-Host "engine synced from $Chdk - rebuild host\rewire and run 'make test' before building the APK"
