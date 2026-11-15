#!/usr/bin/env bash
# Load mram_probe.uf2 into the Pico's SRAM, run it, and attach to its USB serial console.
#
# Usage: ./run_mram_probe.sh [preset] [serial-port]
#   preset       CMake build preset (default: pico2-debug-unix, falls back to pico2-debug)
#   serial-port  skip auto-detection and use this port (e.g. /dev/ttyACM0)
#
# Env overrides: PICOTOOL (path to picotool), BAUD (default 115200)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
PRESET="${1:-}"
PORT="${2:-}"
BAUD="${BAUD:-115200}"
TARGET="mram_probe.uf2"

die() { echo "error: $*" >&2; exit 1; }

# --- locate the build directory ---
if [[ -z "$PRESET" ]]; then
    for p in pico2-debug-unix pico2-debug; do
        if [[ -d "$REPO_ROOT/build/$p" ]]; then PRESET="$p"; break; fi
    done
    [[ -n "$PRESET" ]] || die "no build directory found under $REPO_ROOT/build; pass a preset name"
fi
BUILD_DIR="$REPO_ROOT/build/$PRESET"
[[ -d "$BUILD_DIR" ]] || die "build directory $BUILD_DIR does not exist"

UF2="$(find "$BUILD_DIR" -name "$TARGET" -type f -print -quit)"
[[ -n "$UF2" ]] || die "$TARGET not found in $BUILD_DIR (has the target been built?)"

# --- locate picotool ---
if [[ -z "${PICOTOOL:-}" ]]; then
    if command -v picotool >/dev/null 2>&1; then
        PICOTOOL="picotool"
    else
        # Fall back to the copy installed by the Pico VS Code extension
        PICOTOOL="$(ls -d "$HOME"/.pico-sdk/picotool/*/picotool/picotool 2>/dev/null | sort -V | tail -n 1 || true)"
        [[ -n "$PICOTOOL" ]] || die "picotool not found; add it to PATH or set PICOTOOL"
    fi
fi

# --- load + execute ---
# -x runs the image straight from SRAM after loading (a reset would drop it).
# -f reboots a running app into BOOTSEL first, if it exposes the picotool reset interface.
echo "Loading $UF2"
"$PICOTOOL" load -v -x "$UF2" -f

# --- find the serial port ---
if [[ -z "$PORT" ]]; then
    echo -n "Waiting for USB serial port"
    for _ in $(seq 1 50); do
        PORT="$(ls /dev/ttyACM* /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
        [[ -n "$PORT" ]] && break
        echo -n "."
        sleep 0.2
    done
    echo
    [[ -n "$PORT" ]] || die "no serial port appeared after 10 s (does mram_probe enable stdio_usb?)"
fi

# --- connect ---
echo "Connecting to $PORT at $BAUD baud"
if command -v picocom >/dev/null 2>&1; then
    exec picocom -b "$BAUD" "$PORT"             # exit: Ctrl-A Ctrl-X
elif command -v minicom >/dev/null 2>&1; then
    exec minicom -D "$PORT" -b "$BAUD"          # exit: Ctrl-A X
elif command -v screen >/dev/null 2>&1; then
    exec screen "$PORT" "$BAUD"                 # exit: Ctrl-A K
else
    # No terminal program: read-only fallback
    echo "(picocom/minicom/screen not found, read-only output; Ctrl-C to exit)"
    stty -F "$PORT" "$BAUD" raw -echo 2>/dev/null || stty -f "$PORT" "$BAUD" raw -echo
    exec cat "$PORT"
fi
