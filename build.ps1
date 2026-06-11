#!/usr/bin/env pwsh
# Quick build. Usage: ./build.ps1 [preset]   (default: pico2-debug)
param([string]$Preset = "pico2-debug")
$ErrorActionPreference = "Stop"

cmake --preset $Preset           # configure (cheap if already configured)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build --preset $Preset   # build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
