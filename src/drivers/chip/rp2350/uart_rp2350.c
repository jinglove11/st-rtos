/*
 * RP2350 UART — 全部交给 Pico SDK,除了 puthex/putdec SDK 没有提供。
 * SDK 的 uart_init 会:
 *   reset_block + unreset_block_wait + baud 分频 + LCR_H + CR。
 * CLK_PERI 由 runtime_init_clocks 启用。
 *
 * 应用层 GPIO 配置也在这里提供。
 */

#include "uart.h"
#include "pico2w.h"
#include "hardware/gpio.h"

void board_uart_configure_gpio(uart_t *uart) {
    uint32_t tx_pin = (uart == UART0) ? PICO2W_UART_TX_PIN : 4U;
    uint32_t rx_pin = (uart == UART0) ? PICO2W_UART_RX_PIN : 5U;
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);
    gpio_pull_up(rx_pin);
}

void uart_puthex(uart_t *uart, uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    uart_puts(uart, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(uart, hex[(value >> shift) & 0xfU]);
    }
}

void uart_putdec(uart_t *uart, uint32_t value) {
    char buffer[10];
    uint32_t length = 0U;

    if (value == 0U) {
        uart_putc(uart, '0');
        return;
    }
    while (value != 0U) {
        buffer[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (length != 0U) {
        uart_putc(uart, buffer[--length]);
    }
}
