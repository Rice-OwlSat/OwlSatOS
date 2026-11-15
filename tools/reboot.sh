#!/usr/bin/env sh
# Reboot a Pico 2 running a Debug build over USB, without the BOOTSEL button.
# Uses the SDK's USB reset interface, which every Debug target carries (cmake/debug_usb_reset.cmake).
#
#   tools/reboot.sh                 running app -> BOOTSEL, and wait until picotool sees it
#   tools/reboot.sh --app           restart the application
#   tools/reboot.sh --load <file>   BOOTSEL if needed, load the .uf2/.elf, run it
#
# PICOTOOL overrides the picotool to use. A board that has halted with interrupts off does not answer,
# and still needs BOOTSEL by hand.
set -eu

if [ -z "${PICOTOOL:-}" ]; then
    if command -v picotool >/dev/null 2>&1; then
        PICOTOOL=picotool
    else
        PICOTOOL=$(ls "$HOME"/.pico-sdk/picotool/*/picotool/picotool* 2>/dev/null | sort | tail -n 1)
        [ -n "$PICOTOOL" ] || { echo "picotool not found; add it to PATH or set PICOTOOL" >&2; exit 1; }
    fi
fi

in_bootsel() { "$PICOTOOL" info >/dev/null 2>&1; }

case "${1:-}" in
    --load)
        [ -f "${2:-}" ] || { echo "usage: $0 --load <file.uf2|file.elf>" >&2; exit 1; }
        exec "$PICOTOOL" load -v -x "$2" -f
        ;;
    --app)
        if in_bootsel; then exec "$PICOTOOL" reboot; else exec "$PICOTOOL" reboot -f; fi
        ;;
    "")
        if in_bootsel; then echo "Already in BOOTSEL"; exit 0; fi
        "$PICOTOOL" reboot -f -u
        i=0
        while [ $i -lt 50 ]; do
            if in_bootsel; then echo "In BOOTSEL"; exit 0; fi
            sleep 0.2
            i=$((i + 1))
        done
        echo "Rebooted, but no BOOTSEL device appeared within 10 s" >&2
        exit 1
        ;;
    *)
        echo "usage: $0 [--app | --load <file>]" >&2
        exit 1
        ;;
esac
