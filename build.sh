#!/usr/bin/env bash
# Quick build. Usage: ./build.sh [preset]   (default: pico2-debug)
set -euo pipefail
PRESET="${1:-pico2-debug}"
cmake --preset "$PRESET"          # configure (cheap if already configured)
cmake --build --preset "$PRESET"  # build
