/**
 * @file uart.h
 * @brief UART 驱动头文件 (通用接口)
 *
 * RP2350: 直接用 Pico SDK 的 uart_init/uart_putc/... (硬件层 inline + uart.c)
 *   所以这里只 include board header,SDK 函数就直接可见。
 *
 * STM32F767: 仍是自己的接口。
 */

#ifndef UART_H
#define UART_H

#include "board_config.h"

#if (TARGET_BOARD == BOARD_RP2350_PICO2)

/* RP2350: uart_t + 函数全部由 board header (rp2350.h) 经 SDK 引入 */
#include "rp2350.h"

/* SDK 没提供这两个,见 uart_rp2350.c */
void uart_puthex(uart_t *uart, uint32_t value);
void uart_putdec(uart_t *uart, uint32_t value);

/* 旧代码用 uart_readable/uart_writable,SDK 是 uart_is_readable/uart_is_writable */
static inline int uart_readable(uart_t *uart) { return uart_is_readable(uart); }
static inline int uart_writable(uart_t *uart) { return uart_is_writable(uart); }

#elif (TARGET_BOARD == BOARD_STM32F767_NUCLEO)

void uart_init(usart_t *usart, uint32_t baudrate);
void uart_putc(usart_t *usart, char c);
char uart_getc(usart_t *usart);
void uart_puts(usart_t *usart, const char *s);
void uart_puthex(usart_t *usart, uint32_t value);
void uart_putdec(usart_t *usart, uint32_t value);
int uart_readable(usart_t *usart);
int uart_writable(usart_t *usart);

#endif

#endif // UART_H
