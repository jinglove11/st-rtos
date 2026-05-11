/**
 * @file device.h
 * @brief 设备驱动框架 — device_t 抽象 + 设备注册表
 */

#ifndef DEVICE_H
#define DEVICE_H

#include "kernel_types.h"
#include "kernel_config.h"

#if DRIVER_ENABLE

#include "vfs/inode.h"   /* dev_ops_t */

/*============================================================================
 * 常量
 *============================================================================*/

#define DEVICE_NAME_LEN   16
#define DEVICE_MAX        DRIVER_MAX_DEVICES

/*============================================================================
 * 设备类型
 *============================================================================*/

typedef enum {
    DEVICE_TYPE_CHAR  = 0,   /* 字符设备 (UART, GPIO, SPI, I2C) */
    DEVICE_TYPE_BLOCK = 1,   /* 块设备 (预留) */
} device_type_t;

/*============================================================================
 * 设备描述符
 *============================================================================*/

typedef struct {
    char            name[DEVICE_NAME_LEN];   /* 设备名 ("uart0", "gpio") */
    device_type_t   type;                     /* 设备类型 */
    dev_ops_t      *ops;                      /* 设备操作表 */
    void           *priv;                     /* 驱动私有数据 (寄存器基址等) */
    uint32_t        irq_num;                  /* 硬件 IRQ 号 (0 = 无中断) */
    uint8_t         in_use;                   /* 是否已注册 */
} device_t;

/*============================================================================
 * 设备注册表 API
 *============================================================================*/

void      device_init(void);
device_t *device_alloc(const char *name, device_type_t type);
void      device_free(device_t *dev);
device_t *device_find(const char *name);

#endif /* DRIVER_ENABLE */
#endif /* DEVICE_H */
