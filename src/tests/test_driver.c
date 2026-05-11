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
}

TEST_MODULE_REGISTER(driver, test_driver_module);

#endif /* DRIVER_ENABLE && TEST_ENABLE */
