#include "kernel_config.h"
#include "board.h"

#if DRIVER_ENABLE
#include "device.h"
#include "devfs.h"

extern device_t *uart_dev_register(void);

void board_init_drivers(void) {
    device_t *uart_dev = uart_dev_register();
    if (uart_dev != NULL) {
        (void)devfs_register_device("uart0", uart_dev);
    }
}
#else
void board_init_drivers(void) {
}
#endif

