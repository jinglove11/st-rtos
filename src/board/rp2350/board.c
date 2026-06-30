#include "board.h"
#include "board_config.h"
#include "uart.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico2w.h"

extern void board_uart_configure_gpio(uart_t *uart);

void board_hardware_init(uint32_t uart_baudrate) {
    /*
     * SDK 默认 sys clk 150 MHz,PERI_CLK 也跟着配。SDK uart_init 内部用
     * clock_get_hz(peri_clk) 自动算 baud 分频,所以这里不必再写死 PERI_CLK_HZ。
     */
    uart_init(BOARD_DEFAULT_UART, uart_baudrate);
    board_uart_configure_gpio(BOARD_DEFAULT_UART);
}

void board_status_led_toggle(void) {
    /* Pico 2 W's onboard LED is connected through CYW43, not a direct GPIO. */
}

uint32_t hal_get_sysclk(void) {
    return clock_get_hz(clk_sys);
}

