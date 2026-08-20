#!/usr/bin/env bash
# scripts/ci_local.sh
#
# Local mirror of .github/workflows/build.yml. Runs the same checks the CI
# runner does, minus the upload-artifact step. Use before pushing.
#
# Usage:
#   scripts/ci_local.sh                # full matrix
#   scripts/ci_local.sh rp2350         # RP2350 only (all 4 presets)
#   scripts/ci_local.sh rp2350 default # RP2350 + specific preset
#   scripts/ci_local.sh stm32f767      # STM32 only
#   scripts/ci_local.sh docs           # kconfig doc sync check
#
# Exits non-zero if any step fails. Logs under /tmp/my-rtos-ci/.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LOG_DIR=/tmp/my-rtos-ci
mkdir -p "$LOG_DIR"

pass() { echo -e "\033[32m[PASS]\033[0m $*"; }
fail() { echo -e "\033[31m[FAIL]\033[0m $*" >&2; }
info() { echo -e "\033[34m[ .. ]\033[0m $*"; }

WHAT="${1:-all}"
PRESET="${2:-}"

run_rp2350_preset() {
    local preset="$1"
    local log="$LOG_DIR/rp2350-${preset}.log"
    info "[RP2350/$preset] configure + build (log: $log)"
    : > "$log"

    cp configs/${preset}_defconfig .config
    python3 scripts/menuconfig.py genconfig >>"$log" 2>&1

    if [[ ! -f tools/pico-sdk/pico_sdk_init.cmake ]]; then
        fail "Pico SDK missing; run 'make setup-pico-sdk' first"
        return 3
    fi

    cmake -S . -B build/rp2350-pico-sdk \
        -DPICO_SDK_PATH="$ROOT/tools/pico-sdk" \
        -DPICO_TOOLCHAIN_PATH="$ROOT/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin" \
        -Dpicotool_DIR="$ROOT/tools/picotool/picotool" \
        -DPICO_BOARD=pico2_w \
        -DCMAKE_BUILD_TYPE=Release >>"$log" 2>&1

    cmake --build build/rp2350-pico-sdk -j"${JOBS:-4}" >>"$log" 2>&1

    if [[ ! -f build/rp2350-pico-sdk/my-rtos-pico2w.elf ]]; then
        fail "Build did not produce ELF (see $log)"
        tail -20 "$log" >&2 || true
        return 4
    fi

    python3 scripts/verify_pico2w_build.py \
        --elf build/rp2350-pico-sdk/my-rtos-pico2w.elf \
        --uf2 build/rp2350-pico-sdk/my-rtos-pico2w.uf2 \
        --nm "$ROOT/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm" >>"$log" 2>&1 || {
        fail "verify_pico2w_build.py failed (see $log)"
        return 5
    }

    pass "RP2350/$preset: ELF + UF2 built, image verified"
}

run_stm32() {
    local log="$LOG_DIR/stm32f767-default.log"
    info "[STM32F767/default] configure + build (log: $log)"
    : > "$log"

    cp configs/stm32f767_defconfig .config
    python3 scripts/menuconfig.py genconfig >>"$log" 2>&1

    make BOARD=stm32f767 -j"${JOBS:-4}" >>"$log" 2>&1

    if [[ ! -f build/stm32f767/my-rtos-stm32f767.elf ]]; then
        fail "Build did not produce ELF (see $log)"
        tail -20 "$log" >&2 || true
        return 4
    fi
    pass "STM32F767/default: ELF built"
}

run_docs_check() {
    info "[docs] Kconfig doc sync"
    python3 scripts/gen_kconfig_docs.py >"$LOG_DIR/docs-gen.log" 2>&1
    if ! git diff --quiet -- docs/kconfig/; then
        fail "docs/kconfig/ is stale. Run 'python3 scripts/gen_kconfig_docs.py' and commit."
        git diff --stat -- docs/kconfig/ >&2 || true
        return 6
    fi

    info "[docs] 4 presets parse cleanly"
    for preset in tiny default release full; do
        cp configs/${preset}_defconfig .config
        python3 scripts/menuconfig.py genconfig >"$LOG_DIR/preset-${preset}.log" 2>&1 || {
            fail "$preset preset failed to parse"
            return 7
        }
    done
    pass "Kconfig docs + presets in sync"
}

# ---- dispatch ----

case "$WHAT" in
    all)
        rc=0
        for preset in tiny default release full; do
            run_rp2350_preset "$preset" || rc=$?
        done
        run_stm32 || rc=$?
        run_docs_check || rc=$?
        if [[ $rc -ne 0 ]]; then exit $rc; fi
        ;;
    rp2350)
        if [[ -n "$PRESET" ]]; then
            run_rp2350_preset "$PRESET"
        else
            rc=0
            for preset in tiny default release full; do
                run_rp2350_preset "$preset" || rc=$?
            done
            exit $rc
        fi
        ;;
    stm32|stm32f767)
        run_stm32
        ;;
    docs|kconfig)
        run_docs_check
        ;;
    *)
        fail "Unknown target: $WHAT"
        echo "Usage: $0 [all|rp2350 [preset]|stm32f767|docs]" >&2
        exit 2
        ;;
esac

echo ""
pass "ci_local OK. Logs under $LOG_DIR/"
