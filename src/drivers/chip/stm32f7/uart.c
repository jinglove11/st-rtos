/**
 * @file uart.c
 * @brief UART 驱动实现
 */

#include "uart.h"
#include "pico2.h"

/*============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 配置 UART 引脚
 */
static void uart_gpio_init(uart_t *uart) {
    uint32_t tx_pin, rx_pin;

    if (uart == UART0) {
        tx_pin = PICO2_UART_TX_PIN;
        rx_pin = PICO2_UART_RX_PIN;
    } else {
        tx_pin = 4;  // UART1 默认引脚
        rx_pin = 5;
    }

    // 设置 GPIO 功能为 UART
    volatile uint32_t *gpio_ctrl = &IO_BANK0->io[tx_pin].ctrl;
    *gpio_ctrl = GPIO_FUNC_UART;

    gpio_ctrl = &IO_BANK0->io[rx_pin].ctrl;
    *gpio_ctrl = GPIO_FUNC_UART;

    // 使能输入
    volatile uint32_t *pad_ctrl = &PADS_BANK0->io[rx_pin];
    *pad_ctrl |= PADS_IE;
}

/**
 * @brief 复位 UART 外设
 */
static void uart_reset(uart_t *uart) {
    uint32_t reset_mask = (uart == UART0) ? RESETS_RESET_UART0 : RESETS_RESET_UART1;

    // 复位
    RESETS->reset |= reset_mask;
    RESETS->reset &= ~reset_mask;

    // 等待复位完成
    while (!(RESETS->done & reset_mask));
}

/*============================================================================
 * 公开函数
 *============================================================================*/

void uart_init(uart_t *uart, uint32_t baudrate) {
    // 复位 UART
    uart_reset(uart);

    // 配置 GPIO
    uart_gpio_init(uart);

    // 禁用 UART
    uart->cr = 0;

    // 禁用 FIFO
    uart->lcr_h = 0;

    // 设置波特率
    // 公式: BAUDDIV = FCLK / (16 * BAUD_RATE)
    // 假设外设时钟为 150MHz
    uint32_t divider = PERI_CLK_HZ / (16 * baudrate);
    uart->ibrd = divider >> 6;              // 整数部分
    uart->fbrd = divider & 0x3f;            // 小数部分 (6位)

    // 配置: 8N1, FIFO 使能
    uart->lcr_h = UART_LCRH_WLEN_8 | UART_LCRH_FEN;

    // 使能发送和接收
    uart->cr = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

void uart_putc(uart_t *uart, char c) {
    // 等待发送 FIFO 不满
    while (uart->fr & UART_FR_TXFF);
    uart->dr = c;
}

char uart_getc(uart_t *uart) {
    // 等待接收 FIFO 不空
    while (uart->fr & UART_FR_RXFE);
    return (char)(uart->dr & 0xff);
}

void uart_puts(uart_t *uart, const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc(uart, '\r');
        }
        uart_putc(uart, *s++);
    }
}

void uart_puthex(uart_t *uart, uint32_t value) {
    const char hex[] = "0123456789ABCDEF";

    uart_puts(uart, "0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(uart, hex[(value >> i) & 0xf]);
    }
}

void uart_putdec(uart_t *uart, uint32_t value) {
    char buf[12];
    int i = 0;

    if (value == 0) {
        uart_putc(uart, '0');
        return;
    }

    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        uart_putc(uart, buf[--i]);
    }
}

int uart_readable(uart_t *uart) {
    return !(uart->fr & UART_FR_RXFE);
}

int uart_writable(uart_t *uart) {
    return !(uart->fr & UART_FR_TXFF);
}
