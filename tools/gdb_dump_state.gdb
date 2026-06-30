# tools/gdb_dump_state.gdb
#
# Fault post-mortem dumper for RP2350 / Cortex-M33.
#
# Usage:
#   # terminal 1: start openocd
#   openocd -f tools/openocd.cfg
#
#   # terminal 2: load ELF + dump
#   tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gdb \
#       -q -x tools/gdb_dump_state.gdb \
#       build/rp2350-pico-sdk/my-rtos-pico2w.elf
#
# The script halts the target, dumps fault registers + active TCB + backtrace,
# then drops into interactive gdb so the user can poke further.

set pagination off
set confirm off

target remote :3333

# ---- 1. Halt target and read fault status -------------------------------------

echo \n==== TARGET HALT ====\n
monitor reset halt

echo \n==== EXCEPTION STATE ====\n
printf "CFSR  = 0x%08x\n", *(volatile unsigned int*)0xE000ED28
printf "HFSR  = 0x%08x\n", *(volatile unsigned int*)0xE000ED2C
printf "MMFAR = 0x%08x\n", *(volatile unsigned int*)0xE000ED34
printf "BFAR  = 0x%08x\n", *(volatile unsigned int*)0xE000ED38
printf "AFSR  = 0x%08x\n", *(volatile unsigned int*)0xE000ED3C
printf "SHCSR = 0x%08x\n", *(volatile unsigned int*)0xE000ED24

echo \n==== CFSR DECODE ====\n
set $cfsr = *(volatile unsigned int*)0xE000ED28
# MMFSR (low byte)
set $mmfsr = $cfsr & 0xff
printf "MMFSR: 0x%02x", $mmfsr
if ($mmfsr & (1<<0))  printf " IACCVIOL"
if ($mmfsr & (1<<1))  printf " DACCVIOL"
if ($mmfsr & (1<<3))  printf " MUNSTKERR"
if ($mmfsr & (1<<4))  printf " MSTKERR"
if ($mmfsr & (1<<5))  printf " MLSPERR"
if ($mmfsr & (1<<7))  printf " MMARVALID"
printf "\n"
# BFSR (second byte)
set $bfsr = ($cfsr >> 8) & 0xff
printf "BFSR:  0x%02x", $bfsr
if ($bfsr & (1<<0))  printf " IBUSERR"
if ($bfsr & (1<<1))  printf " PRECISERR"
if ($bfsr & (1<<2))  printf " IMPRECISERR"
if ($bfsr & (1<<3))  printf " UNSTKERR"
if ($bfsr & (1<<4))  printf " STKERR"
if ($bfsr & (1<<7))  printf " BFARVALID"
printf "\n"
# UFSR (third byte, 16-bit)
set $ufsr = ($cfsr >> 16) & 0xffff
printf "UFSR:  0x%04x", $ufsr
if ($ufsr & (1<<0))  printf " UNDEFINSTR"
if ($ufsr & (1<<1))  printf " INVSTATE"
if ($ufsr & (1<<2))  printf " INVPC"
if ($ufsr & (1<<3))  printf " NOCP"
if ($ufsr & (1<<8))  printf " UNALIGNED"
if ($ufsr & (1<<9))  printf " DIVBYZERO"
printf "\n"

echo \n==== STACK POINTERS ====\n
printf "MSP    = 0x%08x\n", $msp
printf "PSP    = 0x%08x\n", $psp
printf "MSPLIM = 0x%08x\n", $msplim
printf "PSPLIM = 0x%08x\n", $psplim
printf "LR     = 0x%08x (EXC_RETURN)\n", $lr
printf "PC     = 0x%08x\n", $pc

echo \n==== ACTIVE TASK / SCHEDULER ====\n
set $cur = *(void**)(&_current_task)
set $nxt = *(void**)(&_next_task)
printf "_current_task = %p\n", $cur
printf "_next_task    = %p\n", $nxt
if ($cur != 0)
    printf "  -> name      = %s\n", ((char**)$cur)[3]
    printf "  -> state     = %d\n", *(int*)((char*)$cur + 64)
    printf "  -> sp        = 0x%08x\n", *(unsigned int*)$cur
    printf "  -> sp_limit  = 0x%08x\n", *(unsigned int*)((char*)$cur + 56)
end

echo \n==== BACKTRACE (current PC + LR) ====\n
bt

echo \n==== MSP STACK (top 16 words) ====\n
x/16xw $msp

echo \n==== PSP STACK (top 16 words) ====\n
x/16xw $psp

echo \n==== INSTRUCTION AT PC ====\n
x/8i $pc

echo \n
echo "Dropped to interactive gdb. Try:"
echo "  p *_current_task"
echo "  x/32xw \$psp"
echo "  monitor reset run        # release target"
echo ""
