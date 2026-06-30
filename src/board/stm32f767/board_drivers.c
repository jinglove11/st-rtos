/**
 * @file board_drivers.c
 * @brief Nucleo-F767ZI 板级设备注册
 *
 * 将板载外设 (UART, GPIO) 注册到 device 子系统和 devfs。
 */

#include "kernel_config.h"
#include "device.h"
#include "vfs/devfs.h"
#include "uart.h"
#include "gpio.h"
#include "nucleo_f767.h"

#if DRIVER_ENABLE

/*============================================================================
 * GPIO ioctl 命令定义
 *============================================================================*/

#define GPIO_CMD_SET_PIN    0x01
#define GPIO_CMD_GET_PIN    0x02
#define GPIO_CMD_TOGGLE     0x03

/*============================================================================
 * GPIO 私有数据
 *============================================================================*/

typedef struct {
    gpio_t   *port;
    uint32_t  pin;
} gpio_priv_t;

static gpio_priv_t led1_priv = { .port = NUCLEO_LED1_PORT, .pin = NUCLEO_LED1_PIN };
static gpio_priv_t led2_priv = { .port = NUCLEO_LED2_PORT, .pin = NUCLEO_LED2_PIN };
static gpio_priv_t led3_priv = { .port = NUCLEO_LED3_PORT, .pin = NUCLEO_LED3_PIN };

/*============================================================================
 * GPIO dev_ops 实现
 *============================================================================*/

static kern_err_t gpio_dev_open(void *priv, uint32_t flags) {
    (void)flags;
    gpio_priv_t *gp = (gpio_priv_t *)priv;
    if (!gp) return KERN_ERR_PARAM;
    gpio_init(gp->port, gp->pin, GPIO_DIR_OUTPUT);
    return KERN_OK;
}

static kern_err_t gpio_dev_close(void *priv) {
    (void)priv;
    return KERN_OK;
}

static kern_err_t gpio_dev_ioctl(void *priv, uint32_t cmd, void *arg) {
    gpio_priv_t *gp = (gpio_priv_t *)priv;
    if (!gp) return KERN_ERR_PARAM;

    uint32_t level;

    switch (cmd) {
    case GPIO_CMD_SET_PIN:
        level = (uint32_t)(uintptr_t)arg;
        gpio_set(gp->port, gp->pin, level ? GPIO_HIGH : GPIO_LOW);
        return KERN_OK;

    case GPIO_CMD_GET_PIN:
        if (!arg) return KERN_ERR_PARAM;
        *(uint32_t *)arg = (uint32_t)gpio_get(gp->port, gp->pin);
        return KERN_OK;

    case GPIO_CMD_TOGGLE:
        gpio_toggle(gp->port, gp->pin);
        return KERN_OK;

    default:
        return KERN_ERR_PARAM;
    }
}

static dev_ops_t gpio_dev_ops = {
    .open  = gpio_dev_open,
    .close = gpio_dev_close,
    .read  = NULL,
    .write = NULL,
    .ioctl = gpio_dev_ioctl,
};

/*============================================================================
 * GPIO 设备注册
 *============================================================================*/

static device_t *led1_dev;
static device_t *led2_dev;
static device_t *led3_dev;

static void gpio_dev_register_all(void) {
    led1_dev = device_alloc("led1", DEVICE_TYPE_CHAR);
    if (led1_dev) {
        led1_dev->ops  = &gpio_dev_ops;
        led1_dev->priv = &led1_priv;
        devfs_register_device("led1", led1_dev);
    }

    led2_dev = device_alloc("led2", DEVICE_TYPE_CHAR);
    if (led2_dev) {
        led2_dev->ops  = &gpio_dev_ops;
        led2_dev->priv = &led2_priv;
        devfs_register_device("led2", led2_dev);
    }

    led3_dev = device_alloc("led3", DEVICE_TYPE_CHAR);
    if (led3_dev) {
        led3_dev->ops  = &gpio_dev_ops;
        led3_dev->priv = &led3_priv;
        devfs_register_device("led3", led3_dev);
    }
}

/*============================================================================
 * 板级驱动初始化入口
 *============================================================================*/

extern device_t *uart_dev_register(void);

void board_init_drivers(void) {
    /* UART */
    device_t *uart_dev = uart_dev_register();
    if (uart_dev)
        devfs_register_device("uart0", uart_dev);

    /* GPIO LEDs */
    gpio_dev_register_all();
}

#endif /* DRIVER_ENABLE */
