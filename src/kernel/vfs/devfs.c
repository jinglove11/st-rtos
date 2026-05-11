/**
 * @file devfs.c
 * @brief devfs — 字符设备文件系统 + /dev/null 实现
 */

#include "devfs.h"
#include "mem.h"
#include <string.h>

#if VFS_ENABLE

#if DRIVER_ENABLE
#include "device.h"
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
    if (dev && dev->ops && dev->ops->open) return dev->ops->open(dev->priv, flags);
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->open) return ops->open(NULL, flags);
#endif
    return KERN_OK;
}

static kern_err_t cdev_close(inode_t *inode) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (dev && dev->ops && dev->ops->close) return dev->ops->close(dev->priv);
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (ops && ops->close) return ops->close(NULL);
#endif
    return KERN_OK;
}

static int32_t cdev_read(inode_t *inode, void *buf, uint32_t offset, uint32_t size) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->read) return KERN_ERR;
    return dev->ops->read(dev->priv, buf, offset, size);
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->read) return KERN_ERR;
    return ops->read(NULL, buf, offset, size);
#endif
}

static int32_t cdev_write(inode_t *inode, const void *buf, uint32_t offset, uint32_t size) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->write) return KERN_ERR;
    return dev->ops->write(dev->priv, buf, offset, size);
#else
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->write) return KERN_ERR;
    return ops->write(NULL, buf, offset, size);
#endif
}

static kern_err_t cdev_ioctl(inode_t *inode, uint32_t cmd, void *arg) {
#if DRIVER_ENABLE
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->ioctl) return KERN_ERR;
    return dev->ops->ioctl(dev->priv, cmd, arg);
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

#endif /* VFS_ENABLE */
