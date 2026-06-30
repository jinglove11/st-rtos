/**
 * @file board_config.h
 * @brief 板级配置选择
 *
 * 使用方法:
 *   1. 在 Makefile 中定义 BOARD=xxx
 *   2. 或在此文件中直接选择
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/*============================================================================
 * 目标板选择
 *
 * 可选值:
 *   BOARD_RP2350_PICO2    - Raspberry Pi Pico 2 (RP2350)
 *   BOARD_STM32F767_NUCLEO - STM32 Nucleo-F767ZI
 *============================================================================*/

// 默认开发板 (可通过 Makefile BOARD=xxx 覆盖)
#ifndef BOARD_RP2350_PICO2
#define BOARD_RP2350_PICO2      1
#endif

#ifndef BOARD_STM32F767_NUCLEO
#define BOARD_STM32F767_NUCLEO  2
#endif

// 当前目标板
#ifndef TARGET_BOARD
#define TARGET_BOARD    BOARD_STM32F767_NUCLEO
#endif

/*============================================================================
 * 根据目标板包含对应头文件
 *============================================================================*/

#if (TARGET_BOARD == BOARD_RP2350_PICO2)
    #include "pico2.h"
    #ifndef BOARD_DISPLAY_NAME
    #define BOARD_DISPLAY_NAME   "RP2350 (Pico 2)"
    #endif
    #define MCU_NAME             "RP2350"
    #define CPU_CORE             "Cortex-M33"

#elif (TARGET_BOARD == BOARD_STM32F767_NUCLEO)
    #include "nucleo_f767.h"
    #ifndef BOARD_DISPLAY_NAME
    #define BOARD_DISPLAY_NAME   "Nucleo-F767ZI"
    #endif
    #define MCU_NAME             "STM32F767ZIT6"
    #define CPU_CORE             "Cortex-M7"
    #define BOARD_DEFAULT_UART   NUCLEO_DEFAULT_UART
    #define BOARD_UART_BAUDRATE  NUCLEO_UART_BAUDRATE
    #define BOARD_SRAM_BASE      SRAM_BASE
    #define BOARD_SRAM_SIZE      SRAM_SIZE
    #define BOARD_FLASH_BASE     FLASH_BASE
    #define BOARD_FLASH_SIZE     FLASH_SIZE
    #define BOARD_IRQ_COUNT      98U
    #define BOARD_MPU_ARMV8      0

#else
    #error "Unknown target board!"
#endif

#endif // BOARD_CONFIG_H
