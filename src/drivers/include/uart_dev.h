/**
 * @file uart_dev.h
 * @brief UART 设备驱动头文件
 */

#ifndef UART_DEV_H
#define UART_DEV_H

#include "kernel_config.h"

#if DRIVER_ENABLE

#include "device.h"

device_t *uart_dev_register(void);

#endif /* DRIVER_ENABLE */
#endif /* UART_DEV_H */
