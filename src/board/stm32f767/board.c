#include "board.h"
#include "board_config.h"
#include "gpio.h"
#include "uart.h"

void board_hardware_init(uint32_t uart_baudrate) {
    uart_init(BOARD_DEFAULT_UART, uart_baudrate);
    gpio_init(NUCLEO_LED_PORT, NUCLEO_LED_PIN, GPIO_DIR_OUTPUT);
}

void board_status_led_toggle(void) {
    gpio_toggle(NUCLEO_LED_PORT, NUCLEO_LED_PIN);
}
