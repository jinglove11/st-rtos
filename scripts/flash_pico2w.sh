#!/usr/bin/env bash
set -euo pipefail

UF2=${1:?UF2 path required}
MOUNT_HINT=${2:-}
PICOTOOL=${3:-picotool}

if [[ ! -f "$UF2" ]]; then
    echo "UF2 not found: $UF2" >&2
    exit 1
fi

mounts=()
[[ -n "$MOUNT_HINT" ]] && mounts+=("$MOUNT_HINT")
mounts+=("/media/${USER:-}/RP2350" "/run/media/${USER:-}/RP2350")

for mount_point in "${mounts[@]}"; do
    if [[ -d "$mount_point" && -w "$mount_point" ]]; then
        cp "$UF2" "$mount_point/"
        sync
        echo "Flashed via BOOTSEL volume: $mount_point"
        exit 0
    fi
done

if [[ -x "$PICOTOOL" ]] && "$PICOTOOL" load -x "$UF2"; then
    echo "Flashed with picotool"
    exit 0
fi

echo "Pico 2 W not found. Hold BOOTSEL while connecting USB, then rerun make flash." >&2
echo "You can override the mount path with: make flash MOUNT=/path/to/RP2350" >&2
exit 1

