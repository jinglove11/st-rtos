#ifndef MYRTOS_BOARD_H
#define MYRTOS_BOARD_H

#include <stdint.h>

void board_hardware_init(uint32_t uart_baudrate);
void board_status_led_toggle(void);
void board_init_drivers(void);

#endif

