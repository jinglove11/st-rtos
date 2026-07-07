#!/usr/bin/env bash
set -euo pipefail

AGENT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$AGENT_DIR/../.." && pwd)"
cd "$REPO"

CFG="${OPENOCD_CFG:-tools/openocd.cfg}"
PORT="${PORT:-/dev/ttyACM0}"

if [ ! -f "$CFG" ]; then
    echo "openocd cfg not found: $CFG" >&2
    exit 1
fi

openocd -f "$CFG" -c "init" -c "reset run" -c "shutdown" \
    >/dev/null 2>&1 || {
    echo "openocd reset failed" >&2
    exit 1
}

sleep 0.5
for i in $(seq 1 40); do
    if [ -e "$PORT" ]; then
        echo "reset OK; $PORT available after ~$((i * 250)) ms"
        exit 0
    fi
    sleep 0.25
done

echo "timed out waiting for $PORT after reset" >&2
exit 2
