<#
.SYNOPSIS
    Load mram_probe.uf2 into the Pico's SRAM, run it, and attach to its USB serial console.

.PARAMETER Preset
    CMake build preset directory under build/ (default: pico2-debug).

.PARAMETER Port
    Skip auto-detection and use this COM port (e.g. COM5).

.PARAMETER Baud
    Baud rate (ignored by USB CDC, but set anyway). Default 115200.

.PARAMETER Picotool
    Path to picotool. Defaults to PATH, then the Pico VS Code extension's install.

.EXAMPLE
    ./run_mram_probe.ps1
    ./run_mram_probe.ps1 -Preset pico2-release -Port COM7
#>
param(
    [string]$Preset = "pico2-debug",
    [string]$Port,
    [int]$Baud = 115200,
    [string]$Picotool
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Target = "mram_probe.uf2"

# --- locate the build output ---
$BuildDir = Join-Path $RepoRoot "build/$Preset"
if (-not (Test-Path $BuildDir)) {
    throw "Build directory $BuildDir does not exist"
}
$Uf2 = Get-ChildItem -Path $BuildDir -Filter $Target -Recurse -File | Select-Object -First 1
if (-not $Uf2) {
    throw "$Target not found in $BuildDir (has the target been built?)"
}

# --- locate picotool ---
if (-not $Picotool) {
    $cmd = Get-Command picotool -ErrorAction SilentlyContinue
    if ($cmd) {
        $Picotool = $cmd.Source
    } else {
        # Fall back to the copy installed by the Pico VS Code extension
        $Picotool = Get-ChildItem "$HOME/.pico-sdk/picotool/*/picotool/picotool*" -File -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
        if (-not $Picotool) { throw "picotool not found; add it to PATH or pass -Picotool" }
    }
}

# --- load + execute ---
# -x runs the image straight from SRAM after loading (a reset would drop it).
# -f reboots a running app into BOOTSEL first, if it exposes the picotool reset interface.
Write-Host "Loading $($Uf2.FullName)"
& $Picotool load -v -x $Uf2.FullName -f
if ($LASTEXITCODE -ne 0) { throw "picotool load failed (exit $LASTEXITCODE)" }

# --- find the serial port (Raspberry Pi USB VID is 2E8A) ---
if (-not $Port) {
    Write-Host -NoNewline "Waiting for USB serial port"
    for ($i = 0; $i -lt 50 -and -not $Port; $i++) {
        $dev = Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'USB\\VID_2E8A%' AND Name LIKE '%(COM%'" |
            Select-Object -First 1
        if ($dev -and $dev.Name -match '\((COM\d+)\)') {
            $Port = $Matches[1]
        } else {
            Write-Host -NoNewline "."
            Start-Sleep -Milliseconds 200
        }
    }
    Write-Host
    if (-not $Port) { throw "No Pico serial port appeared after 10 s (does mram_probe enable stdio_usb?)" }
}

# --- connect ---
$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud)
$serial.DtrEnable = $true   # stdio_usb only sends once the host asserts DTR
$serial.RtsEnable = $true
$serial.ReadTimeout = 50

# The port can take a moment to become openable after enumeration
for ($i = 0; -not $serial.IsOpen; $i++) {
    try { $serial.Open() }
    catch {
        if ($i -ge 20) { throw "Could not open ${Port}: $($_.Exception.Message)" }
        Start-Sleep -Milliseconds 250
    }
}

Write-Host "Connected to $Port at $Baud baud. Ctrl+] to exit." -ForegroundColor Cyan
try {
    while ($true) {
        if ($serial.BytesToRead -gt 0) {
            [Console]::Write($serial.ReadExisting())
        }
        while ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Oem6 -and $key.Modifiers -band [ConsoleModifiers]::Control) {
                return
            }
            if ($key.KeyChar -ne [char]0) { $serial.Write([string]$key.KeyChar) }
        }
        Start-Sleep -Milliseconds 10
    }
} finally {
    $serial.Close()
    Write-Host "`nDisconnected." -ForegroundColor Cyan
}
