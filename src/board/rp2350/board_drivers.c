#include "kernel_config.h"
#include "board.h"

#if DRIVER_ENABLE
#include "device.h"
/* Phase F2: devfs.h 移除,设备不再注册到内核 devfs。
 * 设备仍 device_alloc 注册到 device 池 (driver server 通过它发现设备)。 */

extern device_t *uart_dev_register(void);

void board_init_drivers(void) {
    /* uart_dev_register 内部 device_alloc + 填 ops,注册到 device 池。
     * 不再 devfs_register_device (内核 devfs 移除)。
     * 用户态经 driver server / fs_server 访问设备。 */
    (void)uart_dev_register();
}
#else
void board_init_drivers(void) {
}
#endif
