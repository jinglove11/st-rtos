/**
 * @file devfs.c
 * @brief devfs — 字符设备文件系统 + /dev/null 实现
 */

#include "devfs.h"
#include "mem.h"
#include "trace.h"
#include "stats.h"
#include "scheduler.h"
#include <string.h>

#if VFS_ENABLE

#if DRIVER_ENABLE
#include "device.h"
#endif

#ifndef TRACE_DEV_OPEN
#define TRACE_DEV_OPEN        1
#define TRACE_DEV_READ        2
#define TRACE_DEV_WRITE       3
#define TRACE_DEV_IOCTL       4
#define TRACE_DEV_REMOVE      5
#endif

#ifndef STATS_COUNTER_OK
#define STATS_COUNTER_OK         0
#define STATS_COUNTER_ERROR      1
#define STATS_COUNTER_DELETE     4
#define STATS_COUNTER_BUSY       6
#define STATS_COUNTER_NOEXIST    7
#endif

#if DRIVER_ENABLE
static uint8_t devfs_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

#if TRACE_ENABLE
static uint8_t devfs_trace_result(int32_t result) {
    if (result >= 0) return TRACE_RESULT_OK;
    switch ((kern_err_t)result) {
        case KERN_ERR_BUSY:
            return TRACE_RESULT_BUSY;
        case KERN_ERR_NOEXIST:
            return TRACE_RESULT_NOEXIST;
        case KERN_ERR_RESOURCE:
        case KERN_ERR_OVERFLOW:
            return TRACE_RESULT_FULL;
        default:
            return TRACE_RESULT_ERR;
    }
}
#endif

static uint8_t devfs_stats_counter(int32_t result) {
    if (result >= 0) return STATS_COUNTER_OK;
    if ((kern_err_t)result == KERN_ERR_BUSY) return STATS_COUNTER_BUSY;
    if ((kern_err_t)result == KERN_ERR_NOEXIST) return STATS_COUNTER_NOEXIST;
    return STATS_COUNTER_ERROR;
}

static void devfs_record_event(device_t *dev, uint8_t action, int32_t result) {
#if TRACE_ENABLE
    int16_t id = device_get_id(dev);
    uint8_t object_id = (id >= 0) ? (uint8_t)id : 0xFFU;
    trace_dev(devfs_current_task_id(), object_id, action,
              devfs_trace_result(result));
#else
    (void)dev;
    (void)action;
#endif

#if KERN_TASK_STATS
    (void)stats_record_event(STATS_SUBSYS_DEV, devfs_stats_counter(result));
#endif
    (void)result;
}
#endif

/*============================================================================
 * /dev/null — 驱动操作表
 *============================================================================*/

static int32_t null_read(void *priv, void *buf, uint32_t offset, uint32_t size) {
    (void)priv; (void)buf; (void)offset; (void)size;
    return 0;  /* EOF */
}

static int32_t null_write(void *priv, const void *buf, uint32_t offset, uint32_t size) {
    (void)priv; (void)buf; (void)offset;
    return (int32_t)size;  /* 吃掉所有数据 */
}

static dev_ops_t null_dev_ops = {
    .open  = NULL,
    .close = NULL,
    .read  = null_read,
    .write = null_write,
    .ioctl = NULL,
};

#if DRIVER_ENABLE
static device_t null_device = {
    .name   = "null",
    .type   = DEVICE_TYPE_CHAR,
    .ops    = &null_dev_ops,
    .priv   = NULL,
    .irq_num = 0,
    .in_use = 1,
};
#endif

/*============================================================================
 * cdev_fops — 所有字符设备共享
 *
 * DRIVER_ENABLE: inode->private_data = device_t*, 通过 dev->ops 分发
 * 否则:         inode->private_data = dev_ops_t*, 直接调用
 *============================================================================*/

static kern_err_t cdev_open(inode_t *inode, uint32_t flags) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops) {
        devfs_record_event(dev, TRACE_DEV_OPEN, KERN_ERR_NOEXIST);
        return KERN_ERR_NOEXIST;
    }
    kern_err_t err = KERN_OK;
    if (dev->ops->open) err = dev->ops->open(dev->priv, flags);
    if (err == KERN_OK) dev->open_count++;
    devfs_record_event(dev, TRACE_DEV_OPEN, err);
    return err;
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->open) return ops->open(NULL, flags);
#endif
    return KERN_OK;
}

static kern_err_t cdev_close(inode_t *inode) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops) return KERN_ERR_NOEXIST;
    kern_err_t err = KERN_OK;
    if (dev->ops->close) err = dev->ops->close(dev->priv);
    if (err == KERN_OK && dev->open_count > 0) dev->open_count--;
    return err;
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->close) return ops->close(NULL);
#endif
    return KERN_OK;
}

static int32_t cdev_read(inode_t *inode, void *buf, uint32_t offset, uint32_t size) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->read) {
        devfs_record_event(dev, TRACE_DEV_READ, KERN_ERR_NOEXIST);
        return KERN_ERR;
    }
    int32_t result = dev->ops->read(dev->priv, buf, offset, size);
    devfs_record_event(dev, TRACE_DEV_READ, result);
    return result;
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->read) return KERN_ERR;
    return ops->read(NULL, buf, offset, size);
#endif
}

static int32_t cdev_write(inode_t *inode, const void *buf, uint32_t offset, uint32_t size) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->write) {
        devfs_record_event(dev, TRACE_DEV_WRITE, KERN_ERR_NOEXIST);
        return KERN_ERR;
    }
    int32_t result = dev->ops->write(dev->priv, buf, offset, size);
    devfs_record_event(dev, TRACE_DEV_WRITE, result);
    return result;
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->write) return KERN_ERR;
    return ops->write(NULL, buf, offset, size);
#endif
}

static kern_err_t cdev_ioctl(inode_t *inode, uint32_t cmd, void *arg) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops) {
        devfs_record_event(dev, TRACE_DEV_IOCTL, KERN_ERR_NOEXIST);
        return KERN_ERR;
    }

    if (cmd == DEVICE_IOCTL_GET_EVENTS) {
        if (!arg) {
            devfs_record_event(dev, TRACE_DEV_IOCTL, KERN_ERR_PARAM);
            return KERN_ERR_PARAM;
        }
        *(uint32_t *)arg = device_get_events(dev);
        devfs_record_event(dev, TRACE_DEV_IOCTL, KERN_OK);
        return KERN_OK;
    }

    if (cmd == DEVICE_IOCTL_CLEAR_EVENTS) {
        uint32_t events = arg ? *(uint32_t *)arg : 0xFFFFFFFFU;
        kern_err_t err = device_clear_events(dev, events);
        devfs_record_event(dev, TRACE_DEV_IOCTL, err);
        return err;
    }

    if (!dev->ops->ioctl) {
        devfs_record_event(dev, TRACE_DEV_IOCTL, KERN_ERR_NOEXIST);
        return KERN_ERR;
    }
    kern_err_t err = dev->ops->ioctl(dev->priv, cmd, arg);
    devfs_record_event(dev, TRACE_DEV_IOCTL, err);
    return err;
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->ioctl) return KERN_ERR;
    return ops->ioctl(NULL, cmd, arg);
#endif
}

static cdev_ops_t cdev_shared_fops = {
    .open  = cdev_open,
    .close = cdev_close,
    .read  = cdev_read,
    .write = cdev_write,
    .ioctl = cdev_ioctl,
};

/*============================================================================
 * devfs 全局状态
 *============================================================================*/

static inode_t *devfs_root;

/*============================================================================
 * devfs_init
 *============================================================================*/

void devfs_init(inode_t *dev_dir) {
    if (!dev_dir) return;
    devfs_root = dev_dir;
#if DRIVER_ENABLE
    devfs_register_device("null", &null_device);
#else
    devfs_register_device("null", &null_dev_ops);
#endif
}

/*============================================================================
 * devfs_register_device
 *============================================================================*/

#if DRIVER_ENABLE
kern_err_t devfs_register_device(const char *name, device_t *dev) {
    if (!name || !dev || !devfs_root) return KERN_ERR_PARAM;

    /* 检查同名设备是否已存在 */
    if (inode_lookup_child(devfs_root, name))
        return KERN_ERR;

    /* 创建 CHRDEV inode */
    inode_t *inode = inode_alloc(INODE_TYPE_CHRDEV, name);
    if (!inode) return KERN_ERR_RESOURCE;

    inode->ops_u.cdev_ops = &cdev_shared_fops;
    inode->private_data = dev;

    inode_add_child(devfs_root, inode);
    return KERN_OK;
}
#else
kern_err_t devfs_register_device(const char *name, dev_ops_t *ops) {
    if (!name || !ops || !devfs_root) return KERN_ERR_PARAM;

    /* 检查同名设备是否已存在 */
    if (inode_lookup_child(devfs_root, name))
        return KERN_ERR;

    /* 创建 CHRDEV inode */
    inode_t *inode = inode_alloc(INODE_TYPE_CHRDEV, name);
    if (!inode) return KERN_ERR_RESOURCE;

    inode->ops_u.cdev_ops = &cdev_shared_fops;
    inode->private_data = ops;

    inode_add_child(devfs_root, inode);
    return KERN_OK;
}
#endif

kern_err_t devfs_unregister_device(const char *name) {
    if (!name || !devfs_root) return KERN_ERR_PARAM;

    inode_t *inode = inode_lookup_child(devfs_root, name);
    if (!inode) return KERN_ERR_NOEXIST;
    if (inode->type != INODE_TYPE_CHRDEV) return KERN_ERR_PARAM;
    if (inode->refcount > 2) return KERN_ERR_BUSY;

#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (dev && dev->open_count > 0) return KERN_ERR_BUSY;
    devfs_record_event(dev, TRACE_DEV_REMOVE, KERN_OK);
#endif

    kern_err_t err = inode_remove_child(devfs_root, name);
    if (err != KERN_OK) return err;

    inode_put(inode);
    return KERN_OK;
}

#endif /* VFS_ENABLE */
