/**
 * @file uart.h
 * @brief UART 驱动头文件 (通用接口)
 */

#ifndef UART_H
#define UART_H

#include "board_config.h"

/*============================================================================
 * 平台相关类型定义
 *============================================================================*/

#if (TARGET_BOARD == BOARD_RP2350_PICO2)

// RP2350 使用 uart_t 类型
void uart_init(uart_t *uart, uint32_t baudrate);
void uart_putc(uart_t *uart, char c);
char uart_getc(uart_t *uart);
void uart_puts(uart_t *uart, const char *s);
void uart_puthex(uart_t *uart, uint32_t value);
void uart_putdec(uart_t *uart, uint32_t value);
int uart_readable(uart_t *uart);
int uart_writable(uart_t *uart);

#elif (TARGET_BOARD == BOARD_STM32F767_NUCLEO)

// STM32 使用 usart_t 类型
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
