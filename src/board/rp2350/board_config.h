#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_RP2350_PICO2       1
#define BOARD_STM32F767_NUCLEO   2

#ifndef TARGET_BOARD
#define TARGET_BOARD BOARD_RP2350_PICO2
#endif

#if TARGET_BOARD != BOARD_RP2350_PICO2
#error "RP2350 board configuration selected for another target"
#endif

#include "pico2w.h"

#ifndef BOARD_DISPLAY_NAME
#define BOARD_DISPLAY_NAME "Raspberry Pi Pico 2 W"
#endif

#define MCU_NAME "RP2350"
#define CPU_CORE "Cortex-M33"

#endif

