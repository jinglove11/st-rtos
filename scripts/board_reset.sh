#!/usr/bin/env bash
# Reset the Pico 2 W and wait for its USB CDC node to come back.
#
# A hardware reset makes the board's USB CDC port disappear and re-enumerate,
# which breaks any process holding /dev/ttyACM0 open. This helper:
#   1. asserts reset via openocd (reset run),
#   2. polls until /dev/ttyACM0 reappears,
#   3. truncates the serial logs so the next read starts from fresh boot.
#
# It does NOT touch the serial_debug.py process: that daemon detects the fd
# loss (OSError on read) and its open_port retry loop reconnects once the node
# is back, which step 2 guarantees. So by the time this script returns, the
# debug daemon is reading the fresh boot output.
#
# Usage: scripts/board_reset.sh
# Env:   OPENOCD_CFG (default tools/openocd.cfg), PORT (default /dev/ttyACM0),
#        OUT_LOG / STATUS_LOG (defaults match serial_debug.py)

set -uo pipefail
cd "$(dirname "$0")/.."

CFG="${OPENOCD_CFG:-tools/openocd.cfg}"
PORT="${PORT:-/dev/ttyACM0}"

if [ ! -f "$CFG" ]; then
    echo "openocd cfg not found: $CFG" >&2
    exit 1
fi

# 1. Reset the core (deassert + run).
openocd -f "$CFG" -c "init" -c "reset run" -c "shutdown" >/dev/null 2>&1 || {
    echo "openocd reset failed" >&2
    exit 1
}

# 2. Wait for the CDC node to vanish and reappear (USB re-enumeration).
#    Give it a moment to drop, then poll for reappearance. The caller is
#    responsible for truncating the serial logs before/after as needed —
#    touching them here races with the daemon's own writes.
sleep 0.5
for i in $(seq 1 40); do
    if [ -e "$PORT" ]; then
        echo "reset OK; $PORT back after ~${i} tries"
        exit 0
    fi
    sleep 0.25
done

echo "timed out waiting for $PORT after reset" >&2
exit 2
