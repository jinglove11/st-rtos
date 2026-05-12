/**
 * @file test_driver.c
 * @brief 设备驱动框架测试
 */

#include "test_framework.h"
#include "kernel_config.h"
#include "device.h"
#include "vfs/vfs.h"
#include "vfs/inode.h"
#include "vfs/devfs.h"
#include "trace.h"
#include "stats.h"
#include "user_api.h"
#include <string.h>

#if DRIVER_ENABLE && TEST_ENABLE

/*============================================================================
 * Test 1: device_alloc 基本分配
 *============================================================================*/

static void test_device_alloc_basic(void) {
    test_section("Test 1: device_alloc basic");

    device_t *dev = device_alloc("test0", DEVICE_TYPE_CHAR);
    TEST_ASSERT_NOT_NULL(dev, "device_alloc returns non-NULL");
    if (dev) {
        TEST_ASSERT_EQ(0, strcmp(dev->name, "test0"), "name matches");
        TEST_ASSERT_EQ(DEVICE_TYPE_CHAR, dev->type, "type is CHAR");
        TEST_ASSERT_EQ(1, dev->in_use, "in_use set");
        device_free(dev);  /* 回收，避免耗尽池 */
    }
}

/*============================================================================
 * Test 2: device_alloc 重复名称
 *============================================================================*/

static void test_device_alloc_dup(void) {
    test_section("Test 2: device_alloc duplicate name");

    device_t *d1 = device_alloc("dup0", DEVICE_TYPE_CHAR);
    device_t *d2 = device_alloc("dup0", DEVICE_TYPE_CHAR);
    TEST_ASSERT_NOT_NULL(d1, "first alloc succeeds");
    TEST_ASSERT_NOT_NULL(d2, "second alloc returns different slot");
    TEST_ASSERT(d1 != d2, "different pointers (pool slots)");
    if (d1) device_free(d1);
    if (d2) device_free(d2);
}

/*============================================================================
 * Test 3: device_find 查找
 *============================================================================*/

static void test_device_find(void) {
    test_section("Test 3: device_find");

    device_alloc("findme", DEVICE_TYPE_CHAR);
    device_t *dev = device_find("findme");
    TEST_ASSERT_NOT_NULL(dev, "found 'findme'");
    TEST_ASSERT_EQ(0, strcmp(dev->name, "findme"), "name matches");

    device_t *none = device_find("noexist");
    TEST_ASSERT_NULL(none, "nonexistent returns NULL");
}

/*============================================================================
 * Test 4: device_free 释放
 *============================================================================*/

static void test_device_free(void) {
    test_section("Test 4: device_free");

    device_t *dev = device_alloc("freeme", DEVICE_TYPE_CHAR);
    TEST_ASSERT_NOT_NULL(dev, "allocated");
    device_free(dev);
    TEST_ASSERT_EQ(0, dev->in_use, "in_use cleared");
    device_t *none = device_find("freeme");
    TEST_ASSERT_NULL(none, "no longer findable");
}

/*============================================================================
 * Test 5: device_alloc NULL 参数
 *============================================================================*/

static void test_device_alloc_null(void) {
    test_section("Test 5: device_alloc NULL name");

    device_t *dev = device_alloc(NULL, DEVICE_TYPE_CHAR);
    TEST_ASSERT_NULL(dev, "NULL name returns NULL");
}

/*============================================================================
 * Test 6: /dev/null 设备存在
 *============================================================================*/

static void test_devnull_exists(void) {
    test_section("Test 6: /dev/null exists in devfs");

    int fd = open("/dev/null", O_RDONLY);
    TEST_ASSERT(fd >= 0, "/dev/null openable");
    close(fd);
}

/*============================================================================
 * Test 7: /dev/null 读写
 *============================================================================*/

static void test_devnull_rw(void) {
    test_section("Test 7: /dev/null read/write");

    int fd = open("/dev/null", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open /dev/null for read");

    char buf[8];
    int n = read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQ(0, n, "read returns 0 (EOF)");
    close(fd);

    fd = open("/dev/null", O_WRONLY);
    TEST_ASSERT(fd >= 0, "open /dev/null for write");

    n = write(fd, "hello", 5);
    TEST_ASSERT_EQ(5, n, "write returns 5 (consumed)");
    close(fd);
}

/*============================================================================
 * Test 8: UART 设备注册
 *============================================================================*/

static void test_uart_device(void) {
    test_section("Test 8: uart0 device registered");

    device_t *dev = device_find("uart0");
    TEST_ASSERT_NOT_NULL(dev, "uart0 in device registry");
    TEST_ASSERT_EQ(DEVICE_TYPE_CHAR, dev->type, "type is CHAR");
    TEST_ASSERT_NOT_NULL(dev->ops, "ops not NULL");
    TEST_ASSERT_NOT_NULL(dev->ops->read, "has read op");
    TEST_ASSERT_NOT_NULL(dev->ops->write, "has write op");
}

/*============================================================================
 * Test 9: UART 设备在 /dev 中
 *============================================================================*/

static void test_uart_devfs(void) {
    test_section("Test 9: /dev/uart0 in devfs");

    int fd = open("/dev/uart0", O_RDWR);
    TEST_ASSERT(fd >= 0, "/dev/uart0 openable");

    uint32_t events = 0;
    kern_err_t err = ioctl(fd, DEVICE_IOCTL_GET_EVENTS, &events);
    TEST_ASSERT_EQ(KERN_OK, err, "/dev/uart0 get events ioctl OK");
    TEST_ASSERT((events & DEVICE_EVENT_WRITABLE) != 0,
                "/dev/uart0 reports writable event");

    close(fd);
}

/*============================================================================
 * Test 10: LED 设备注册
 *============================================================================*/

static void test_led_devices(void) {
    test_section("Test 10: LED devices registered");

    device_t *led1 = device_find("led1");
    TEST_ASSERT_NOT_NULL(led1, "led1 in registry");

    device_t *led2 = device_find("led2");
    TEST_ASSERT_NOT_NULL(led2, "led2 in registry");

    device_t *led3 = device_find("led3");
    TEST_ASSERT_NOT_NULL(led3, "led3 in registry");
}

/*============================================================================
 * Test 11: device_probe / device_remove 与诊断
 *============================================================================*/

static int probe_ioctl_seen = 0;

static kern_err_t probe_open(void *priv, uint32_t flags) {
    (void)priv;
    (void)flags;
    return KERN_OK;
}

static kern_err_t probe_close(void *priv) {
    (void)priv;
    return KERN_OK;
}

static int32_t probe_read(void *priv, void *buf, uint32_t offset, uint32_t size) {
    (void)priv;
    (void)offset;
    if (!buf || size == 0) return 0;
    ((char *)buf)[0] = 'x';
    return 1;
}

static int32_t probe_write(void *priv, const void *buf, uint32_t offset, uint32_t size) {
    (void)priv;
    (void)buf;
    (void)offset;
    return (int32_t)size;
}

static kern_err_t probe_ioctl(void *priv, uint32_t cmd, void *arg) {
    (void)priv;
    (void)cmd;
    (void)arg;
    probe_ioctl_seen++;
    return KERN_OK;
}

static dev_ops_t probe_ops = {
    .open = probe_open,
    .close = probe_close,
    .read = probe_read,
    .write = probe_write,
    .ioctl = probe_ioctl,
};

#if TRACE_ENABLE && KERN_TASK_STATS
static void driver_trace_count_cb(const trace_entry_t *entry, void *ctx) {
    (void)entry;
    (void)ctx;
}
#endif

static void test_device_probe_remove_diag(void) {
    test_section("Test 11: device_probe/remove diagnostics");

#if TRACE_ENABLE && KERN_TASK_STATS
    trace_clear();
    stats_clear_events();
#endif
    probe_ioctl_seen = 0;

    kern_err_t err = device_probe("probe0", DEVICE_TYPE_CHAR, &probe_ops,
                                  NULL, 0);
    TEST_ASSERT_EQ(KERN_OK, err, "device_probe OK");
    TEST_ASSERT_NOT_NULL(device_find("probe0"), "probe0 in registry");

    int fd = open("/dev/probe0", O_RDWR);
    TEST_ASSERT(fd >= 0, "/dev/probe0 openable");

    err = device_remove("probe0");
    TEST_ASSERT_EQ(KERN_ERR_BUSY, err, "remove busy while fd open");

    char ch = 0;
    int n = read(fd, &ch, 1);
    TEST_ASSERT_EQ(1, n, "probe read returns one byte");
    TEST_ASSERT_EQ('x', ch, "probe read byte");

    n = write(fd, "ab", 2);
    TEST_ASSERT_EQ(2, n, "probe write consumes bytes");

    err = ioctl(fd, 1, NULL);
    TEST_ASSERT_EQ(KERN_OK, err, "probe ioctl OK");
    TEST_ASSERT_EQ(1, probe_ioctl_seen, "probe ioctl reached driver");

    close(fd);

    err = device_remove("probe0");
    TEST_ASSERT_EQ(KERN_OK, err, "device_remove after close OK");
    TEST_ASSERT_NULL(device_find("probe0"), "probe0 removed from registry");
    TEST_ASSERT_NULL(vfs_lookup("/dev/probe0"), "probe0 removed from devfs");

#if TRACE_ENABLE && KERN_TASK_STATS
    uint16_t dev_events = trace_filter(TRACE_DEV, driver_trace_count_cb, NULL);
    TEST_ASSERT(dev_events >= 6, "device trace events recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_DEV, STATS_COUNTER_BUSY) >= 1,
                "device busy stat recorded");
    TEST_ASSERT(stats_get_event_count(STATS_SUBSYS_DEV, STATS_COUNTER_DELETE) >= 1,
                "device remove stat recorded");
#endif
}

/*============================================================================
 * Test 12: device event notification ioctls
 *============================================================================*/

static void test_device_event_ioctl(void) {
    test_section("Test 12: device event ioctls");

    kern_err_t err = device_probe("evdev", DEVICE_TYPE_CHAR, &probe_ops,
                                  NULL, 0);
    TEST_ASSERT_EQ(KERN_OK, err, "event device_probe OK");
    if (err != KERN_OK) return;

    device_t *dev = device_find("evdev");
    TEST_ASSERT_NOT_NULL(dev, "event device in registry");
    if (!dev) {
        (void)device_remove("evdev");
        return;
    }

    err = device_notify_events(dev, DEVICE_EVENT_READABLE | DEVICE_EVENT_WRITABLE);
    TEST_ASSERT_EQ(KERN_OK, err, "device_notify_events OK");

    int fd = open("/dev/evdev", O_RDWR);
    TEST_ASSERT(fd >= 0, "/dev/evdev openable");
    if (fd < 0) {
        (void)device_remove("evdev");
        return;
    }

    uint32_t events = 0;
    err = ioctl(fd, DEVICE_IOCTL_GET_EVENTS, &events);
    TEST_ASSERT_EQ(KERN_OK, err, "get events ioctl OK");
    TEST_ASSERT_EQ((int)(DEVICE_EVENT_READABLE | DEVICE_EVENT_WRITABLE),
                   (int)events, "device events visible through ioctl");

    events = DEVICE_EVENT_READABLE;
    err = ioctl(fd, DEVICE_IOCTL_CLEAR_EVENTS, &events);
    TEST_ASSERT_EQ(KERN_OK, err, "clear events ioctl OK");

    events = 0;
    err = ioctl(fd, DEVICE_IOCTL_GET_EVENTS, &events);
    TEST_ASSERT_EQ(KERN_OK, err, "get events after clear OK");
    TEST_ASSERT_EQ((int)DEVICE_EVENT_WRITABLE, (int)events,
                   "clear removed only requested event bit");

    close(fd);
    err = device_remove("evdev");
    TEST_ASSERT_EQ(KERN_OK, err, "event device_remove OK");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_driver_module(void) {
    test_device_alloc_basic();
    test_device_alloc_dup();
    test_device_find();
    test_device_free();
    test_device_alloc_null();
    test_devnull_exists();
    test_devnull_rw();
    test_uart_device();
    test_uart_devfs();
    test_led_devices();
    test_device_probe_remove_diag();
    test_device_event_ioctl();
}

TEST_MODULE_REGISTER(driver, test_driver_module);

#endif /* DRIVER_ENABLE && TEST_ENABLE */
