/**
 * @file device.h
 * @brief 设备驱动框架 — device_t 抽象 + 设备注册表
 */

#ifndef DEVICE_H
#define DEVICE_H

#include "kernel_types.h"
#include "kernel_config.h"

#if DRIVER_ENABLE

#include "fs_types.h"   /* Phase F1: dev_ops_t (原在 inode.h) */

/*============================================================================
 * 常量
 *============================================================================*/

#define DEVICE_NAME_LEN   16
#define DEVICE_MAX        DRIVER_MAX_DEVICES

#define DEVICE_EVENT_READABLE  BIT(0)
#define DEVICE_EVENT_WRITABLE  BIT(1)
#define DEVICE_EVENT_ERROR     BIT(2)
#define DEVICE_EVENT_REMOVED   BIT(3)

#define DEVICE_IOCTL_GET_EVENTS    0x44560001U
#define DEVICE_IOCTL_CLEAR_EVENTS  0x44560002U

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
    uint32_t        events;                   /* DEVICE_EVENT_* bitmask */
    uint16_t        open_count;               /* 当前打开引用数 */
    uint8_t         in_use;                   /* 是否已注册 */
} device_t;

/*============================================================================
 * 设备注册表 API
 *============================================================================*/

void      device_init(void);
device_t *device_alloc(const char *name, device_type_t type);
void      device_free(device_t *dev);
device_t *device_find(const char *name);
int16_t   device_get_id(device_t *dev);
device_t *device_get_by_index(uint16_t index);
kern_err_t device_probe(const char *name, device_type_t type, dev_ops_t *ops,
                        void *priv, uint32_t irq_num);
kern_err_t device_remove(const char *name);
kern_err_t device_notify_events(device_t *dev, uint32_t events);
kern_err_t device_clear_events(device_t *dev, uint32_t events);
uint32_t   device_get_events(device_t *dev);

#endif /* DRIVER_ENABLE */
#endif /* DEVICE_H */
