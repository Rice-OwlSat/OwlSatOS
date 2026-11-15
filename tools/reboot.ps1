<#
.SYNOPSIS
    Reboot a Pico 2 running a Debug build over USB, without the BOOTSEL button.

.DESCRIPTION
    Uses the SDK's USB reset interface, which every Debug target carries (cmake/debug_usb_reset.cmake).
    With no arguments, puts the board into BOOTSEL and waits until picotool can see it.
    A board already in BOOTSEL is left there (or loaded / restarted, with -Load / -App).

    The board must be running a Debug build with USB up. A board that has halted with interrupts off
    does not answer, and still needs BOOTSEL by hand.

.PARAMETER App
    Restart the application instead of stopping in BOOTSEL.

.PARAMETER Load
    A .uf2 or .elf to load and run: reboots into BOOTSEL if needed, loads it (into the MRAM, or into
    SRAM for a no_flash image) and starts it.

.PARAMETER Picotool
    Path to picotool. Defaults to PATH, then the Pico VS Code extension's install.

.EXAMPLE
    ./tools/reboot.ps1
    ./tools/reboot.ps1 -App
    ./tools/reboot.ps1 -Load build/pico2-debug/OwlSatOS.uf2
#>
param(
    [switch]$App,
    [string]$Load,
    [string]$Picotool
)

$ErrorActionPreference = "Stop"

if (-not $Picotool) {
    $cmd = Get-Command picotool -ErrorAction SilentlyContinue
    if ($cmd) {
        $Picotool = $cmd.Source
    } else {
        $Picotool = Get-ChildItem "$HOME/.pico-sdk/picotool/*/picotool/picotool*" -File -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
        if (-not $Picotool) { throw "picotool not found; add it to PATH or pass -Picotool" }
    }
}

function Test-Bootsel {
    & $Picotool info *> $null
    return $LASTEXITCODE -eq 0
}

if ($Load) {
    if (-not (Test-Path $Load)) { throw "$Load does not exist" }
    # -f reboots a running Debug build into BOOTSEL first; harmless if it is already there
    & $Picotool load -v -x (Resolve-Path $Load).Path -f
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed (exit $LASTEXITCODE)" }
    exit 0
}

if ($App) {
    # From BOOTSEL, reboot needs no -f. From a running app it does.
    if (Test-Bootsel) { & $Picotool reboot } else { & $Picotool reboot -f }
    if ($LASTEXITCODE -ne 0) { throw "picotool reboot failed (exit $LASTEXITCODE). Is a Debug build running?" }
    exit 0
}

if (Test-Bootsel) {
    Write-Host "Already in BOOTSEL"
    exit 0
}
& $Picotool reboot -f -u
if ($LASTEXITCODE -ne 0) { throw "picotool reboot failed (exit $LASTEXITCODE). Is a Debug build running, with USB up?" }

for ($i = 0; $i -lt 50; $i++) {
    if (Test-Bootsel) { Write-Host "In BOOTSEL"; exit 0 }
    Start-Sleep -Milliseconds 200
}
throw "Rebooted, but no BOOTSEL device appeared within 10 s"
