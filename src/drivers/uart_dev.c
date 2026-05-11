/**
 * @file uart_dev.c
 * @brief UART 设备驱动 — 将 uart_stm32.c 封装为 device_t
 */

#include "device.h"
#include "uart.h"
#include "board_config.h"
#include "kernel_config.h"

#if DRIVER_ENABLE

/*============================================================================
 * UART dev_ops 实现
 *============================================================================*/

static kern_err_t uart_dev_open(void *priv, uint32_t flags) {
    (void)priv; (void)flags;
    return KERN_OK;
}

static kern_err_t uart_dev_close(void *priv) {
    (void)priv;
    return KERN_OK;
}

static int32_t uart_dev_read(void *priv, void *buf, uint32_t offset, uint32_t size) {
    (void)priv; (void)offset;
    if (!buf || size == 0) return 0;

    char *p = (char *)buf;
    uint32_t count = 0;

    while (count < size) {
        if (!uart_readable(NUCLEO_DEFAULT_UART)) break;
        p[count++] = uart_getc(NUCLEO_DEFAULT_UART);
    }

    return (int32_t)count;
}

static int32_t uart_dev_write(void *priv, const void *buf, uint32_t offset, uint32_t size) {
    (void)priv; (void)offset;
    if (!buf || size == 0) return 0;

    const char *p = (const char *)buf;
    for (uint32_t i = 0; i < size; i++) {
        uart_putc(NUCLEO_DEFAULT_UART, p[i]);
    }

    return (int32_t)size;
}

static dev_ops_t uart_dev_ops = {
    .open  = uart_dev_open,
    .close = uart_dev_close,
    .read  = uart_dev_read,
    .write = uart_dev_write,
    .ioctl = NULL,
};

/*============================================================================
 * UART 设备注册
 *============================================================================*/

static device_t *uart0_dev;

device_t *uart_dev_register(void) {
    uart0_dev = device_alloc("uart0", DEVICE_TYPE_CHAR);
    if (!uart0_dev) return NULL;

    uart0_dev->ops    = &uart_dev_ops;
    uart0_dev->priv   = (void *)NUCLEO_DEFAULT_UART;
    uart0_dev->irq_num = 0;

    return uart0_dev;
}

#endif /* DRIVER_ENABLE */
