#!/usr/bin/env bash
# scripts/regression.sh
#
# One-shot regression check: configure + build + flash + capture UART +
# verify the test summary line. Exits 0 on PASS, non-zero on FAIL.
#
# Usage:
#   scripts/regression.sh                     # auto: rp2350 + ttyACM0
#   PORT=/dev/ttyACM0 scripts/regression.sh
#   BOARD=stm32f767 scripts/regression.sh     # STM32 path (classic Make)
#   SKIP_FLASH=1 scripts/regression.sh        # build-only smoke check
#
# Env vars:
#   BOARD       rp2350 (default) | stm32f767
#   PORT        serial port (default /dev/ttyACM0)
#   JOBS        parallel build jobs (default 4)
#   SKIP_FLASH  if non-empty, skip openocd + UART capture
#   TIMEOUT_S   UART capture window (default 15)

set -euo pipefail

BOARD="${BOARD:-rp2350}"
PORT="${PORT:-/dev/ttyACM0}"
JOBS="${JOBS:-4}"
TIMEOUT_S="${TIMEOUT_S:-15}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LOG_DIR="${REGRESSION_LOG_DIR:-/tmp/my-rtos-regression}"
mkdir -p "$LOG_DIR"
BUILD_LOG="$LOG_DIR/build.log"
FLASH_LOG="$LOG_DIR/flash.log"
UART_LOG="$LOG_DIR/uart.log"

pass() { echo -e "\033[32m[PASS]\033[0m $*"; }
fail() { echo -e "\033[31m[FAIL]\033[0m $*" >&2; }
info() { echo -e "\033[34m[ .. ]\033[0m $*"; }

if [[ "$BOARD" == "rp2350" ]]; then
    BUILD_DIR="build/rp2350-pico-sdk"
    ELF="$BUILD_DIR/my-rtos-pico2w.elf"
    OPENOCD_CFG="tools/openocd.cfg"
elif [[ "$BOARD" == "stm32f767" ]]; then
    BUILD_DIR="build/stm32f767"
    ELF="$BUILD_DIR/my-rtos-stm32f767.elf"
    OPENOCD_CFG="board/st_nucleo_f7.cfg"
else
    fail "Unknown BOARD=$BOARD (expected rp2350|stm32f767)"
    exit 2
fi

info "Board:      $BOARD"
info "ELF:        $ELF"
info "Port:       $PORT"
info "Logs:       $LOG_DIR/"
echo ""

# ---- Stage 1: configure + build ------------------------------------------------

info "[1/3] configure + build (log: $BUILD_LOG)"
if [[ "$BOARD" == "rp2350" ]]; then
    if [[ ! -f tools/pico-sdk/pico_sdk_init.cmake ]]; then
        fail "Pico SDK missing under tools/pico-sdk; run 'make setup-pico-sdk'"
        exit 3
    fi
    (
        set -x
        cmake -S . -B "$BUILD_DIR" \
            -DPICO_SDK_PATH="$ROOT_DIR/tools/pico-sdk" \
            -DPICO_TOOLCHAIN_PATH="$ROOT_DIR/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin" \
            -Dpicotool_DIR="$ROOT_DIR/tools/picotool/picotool" \
            -DPICO_BOARD=pico2_w \
            -DCMAKE_BUILD_TYPE=Release
    ) >"$BUILD_LOG" 2>&1
    ( set -x; cmake --build "$BUILD_DIR" -j"$JOBS" ) >>"$BUILD_LOG" 2>&1
else
    if [[ ! -f .config ]]; then
        info "Loading stm32f767_defconfig..."
        cp configs/stm32f767_defconfig .config
        python3 scripts/menuconfig.py genconfig >>"$BUILD_LOG" 2>&1
    fi
    ( set -x; make BOARD=stm32f767 -j"$JOBS" ) >>"$BUILD_LOG" 2>&1
fi

if [[ ! -f "$ELF" ]]; then
    fail "Build did not produce $ELF (see $BUILD_LOG)"
    tail -20 "$BUILD_LOG" >&2 || true
    exit 4
fi
pass "Build OK: $ELF"

if [[ -n "${SKIP_FLASH:-}" ]]; then
    info "SKIP_FLASH set; stopping after build."
    exit 0
fi

# ---- Stage 2: flash ------------------------------------------------------------

if [[ ! -e "$PORT" ]]; then
    fail "Serial port $PORT not present. Set PORT=<path> or skip with SKIP_FLASH=1."
    exit 5
fi

info "[2/3] flash via openocd (log: $FLASH_LOG)"
pkill -x openocd 2>/dev/null || true
sleep 1

if ! openocd -f "$OPENOCD_CFG" \
        -c "program $ELF verify reset exit" \
        >"$FLASH_LOG" 2>&1; then
    fail "OpenOCD flash failed (see $FLASH_LOG)"
    tail -20 "$FLASH_LOG" >&2 || true
    exit 6
fi

if ! grep -q "verified" "$FLASH_LOG" 2>/dev/null \
   && ! grep -q "Loaded image\|shutdown" "$FLASH_LOG" 2>/dev/null; then
    fail "OpenOCD did not report verify (see $FLASH_LOG)"
    exit 7
fi
pass "Flash OK"

# ---- Stage 3: capture + check --------------------------------------------------

info "[3/3] capture UART for ${TIMEOUT_S}s (log: $UART_LOG)"
: > "$UART_LOG"
stty -F "$PORT" raw -echo 115200 2>/dev/null || true

# Trigger a fresh boot by toggling DTR/RTS via openocd soft reset if available;
# otherwise the openocd program already issued reset exit, so the device is
# already running. Just capture.
timeout "$TIMEOUT_S" cat "$PORT" > "$UART_LOG" 2>/dev/null || true

if [[ ! -s "$UART_LOG" ]]; then
    fail "UART log empty. Check port $PORT / baud 115200."
    exit 8
fi

pass_line=$(grep -E "All tests PASSED" "$UART_LOG" || true)
if [[ -z "$pass_line" ]]; then
    fail "Did not see 'All tests PASSED' in UART output."
    info "Tail of UART log:"
    tail -30 "$UART_LOG" >&2 || true
    exit 9
fi

# Extract Passed/Failed/Total if possible
counts=$(grep -E "^(Passed|Failed|Total):" "$UART_LOG" || true)
if [[ -n "$counts" ]]; then
    info "Test counts:"
    echo "$counts"
fi

pass "$pass_line"
echo ""
pass "Regression OK ($BOARD). Artifacts under $LOG_DIR/"
