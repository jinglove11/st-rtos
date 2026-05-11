/**
 * @file device.c
 * @brief 设备注册表 — 静态池管理
 */

#include "device.h"
#include <string.h>

#if DRIVER_ENABLE

/*============================================================================
 * 静态设备池
 *============================================================================*/

static device_t device_pool[DEVICE_MAX];

/*============================================================================
 * API
 *============================================================================*/

void device_init(void) {
    memset(device_pool, 0, sizeof(device_pool));
}

device_t *device_alloc(const char *name, device_type_t type) {
    if (!name) return NULL;

    for (int i = 0; i < DEVICE_MAX; i++) {
        if (!device_pool[i].in_use) {
            device_t *dev = &device_pool[i];
            memset(dev, 0, sizeof(device_t));
            strncpy(dev->name, name, DEVICE_NAME_LEN - 1);
            dev->name[DEVICE_NAME_LEN - 1] = '\0';
            dev->type = type;
            dev->in_use = 1;
            return dev;
        }
    }
    return NULL;  /* 池满 */
}

void device_free(device_t *dev) {
    if (!dev) return;
    memset(dev, 0, sizeof(device_t));
}

device_t *device_find(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < DEVICE_MAX; i++) {
        if (device_pool[i].in_use &&
            strcmp(device_pool[i].name, name) == 0) {
            return &device_pool[i];
        }
    }
    return NULL;
}

#endif /* DRIVER_ENABLE */
