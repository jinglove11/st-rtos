#!/usr/bin/env bash
# Run a one-shot gdb command against the live RP2350 via openocd gdbserver.
#
# Default action: halt, show key regs + bt, resume. Override with one or
# more -ex style args. Halts the target while running, resumes on exit so
# the live kernel keeps ticking between probes. Set NORESUME=1 to leave
# target halted (useful when the chip is already in fault and you want
# to poke around without triggering another reset cycle).
#
# Usage:
#   scripts/gdb_probe.sh                          # default probe
#   scripts/gdb_probe.sh "info reg"               # one command
#   scripts/gdb_probe.sh "x/16i \$pc" "info threads"   # multiple
#   NORESUME=1 scripts/gdb_probe.sh "info reg"
#
# Env overrides:
#   GDB_PORT (default 3333)
#   PICO_TOOLCHAIN_PATH (default tools/arm-gnu-toolchain-.../bin)

set -uo pipefail
cd "$(dirname "$0")/.."

TOOLCHAIN="${PICO_TOOLCHAIN_PATH:-tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin}"
GDB="$TOOLCHAIN/arm-none-eabi-gdb"
ELF="build/rp2350-pico-sdk/my-rtos-pico2w.elf"
PORT="${GDB_PORT:-3333}"

if [ ! -x "$GDB" ]; then
    echo "gdb not found: $GDB" >&2
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "ELF missing: $ELF (run 'make' first)" >&2
    exit 1
fi

args=(
    -q -batch
    -ex 'set pagination off'
    -ex 'target remote :'"$PORT"
    -ex 'monitor halt'
)

if [ "$#" -eq 0 ]; then
    args+=(
        -ex 'info reg pc lr sp xpsr cfsr hfsr'
        -ex 'bt'
    )
else
    for cmd in "$@"; do
        args+=( -ex "$cmd" )
    done
fi

if [ -z "${NORESUME:-}" ]; then
    args+=( -ex 'monitor resume' )
fi

args+=( "$ELF" )

exec "$GDB" "${args[@]}"
