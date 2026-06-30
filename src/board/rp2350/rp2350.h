#ifndef MYRTOS_RP2350_H
#define MYRTOS_RP2350_H

#include <stddef.h>
#include <stdint.h>
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/uart.h"
#include "hardware/uart.h"

/*
 * UART0 = uart_inst_t* (SDK 包装),不是 uart_hw_t*。SDK uart_init/uart_putc/...
 * 这些 inline 函数都期待 uart_inst_t*。应用层调 uart_init(UART0, baud) 即可。
 */
typedef uart_inst_t uart_t;

#define UART0 uart0
#define UART1 uart1

#define UART_FR_RXFE       (1U << 4)
#define UART_FR_TXFF       (1U << 5)
#define UART_LCRH_FEN      (1U << 4)
#define UART_LCRH_WLEN_8   (3U << 5)
#define UART_CR_UARTEN     (1U << 0)
#define UART_CR_TXE        (1U << 8)
#define UART_CR_RXE        (1U << 9)

#define GPIO_FUNC_UART     2U
#define GPIO_FUNC_SIO      5U
#define PADS_IE            (1U << 6)
#define PADS_PUE           (1U << 3)
#define PADS_PDE           (1U << 2)

typedef struct {
    volatile uint32_t CSR;
    volatile uint32_t RVR;
    volatile uint32_t CVR;
    volatile uint32_t CALIB;
} myrtos_systick_t;

typedef struct {
    volatile uint32_t ISER[16];
    uint32_t reserved0[16];
    volatile uint32_t ICER[16];
    uint32_t reserved1[16];
    volatile uint32_t ISPR[16];
    uint32_t reserved2[16];
    volatile uint32_t ICPR[16];
    uint32_t reserved3[16];
    volatile uint32_t IABR[16];
    uint32_t reserved4[48];
    volatile uint8_t IP[496];
} myrtos_nvic_t;

typedef struct {
    volatile uint32_t CPUID;
    volatile uint32_t ICSR;
    volatile uint32_t VTOR;
    volatile uint32_t AIRCR;
    volatile uint32_t SCR;
    volatile uint32_t CCR;
    volatile uint32_t SHPR[3];
    volatile uint32_t SHCSR;
} myrtos_scb_t;

#define SYSTICK ((myrtos_systick_t *)0xE000E010UL)
#define NVIC    ((myrtos_nvic_t *)0xE000E100UL)
#define SCB     ((myrtos_scb_t *)0xE000ED00UL)
#define SYSTICK_CSR_ENABLE    (1U << 0)
#define SYSTICK_CSR_TICKINT   (1U << 1)
#define SYSTICK_CSR_CLKSRC    (1U << 2)

#define NOP() __asm volatile("nop")
#define DMB() __asm volatile("dmb")
#define DSB() __asm volatile("dsb")
#define ISB() __asm volatile("isb")

#endif
