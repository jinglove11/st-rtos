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
#include "root_bootstrap.h"
#include "task.h"
#include "capability.h"
#include "endpoint.h"
#include "driver_proto.h"
#include "nameserver.h"
#include <string.h>

#if DRIVER_ENABLE && TEST_ENABLE

static void driver_root_dummy_task(void *arg) {
    (void)arg;
    task_exit(NULL);
}

static void uart_server_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_error_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 4);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_nameserver_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_ping_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 1);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_register_client_task(void *arg) {
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    int ns_ep_cap = (int)(cap_id_t)(packed & 0xffffU);
    cap_id_t service_ep_cap = (cap_id_t)((packed >> 16) & 0xffffU);
    int err;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_register(ns_ep_cap, "dev.uart0", service_ep_cap,
                              0x55415254U, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_lookup_ping_client_task(void *arg) {
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    int ns_ep_cap = (int)(cap_id_t)(packed & 0xffffU);
    cap_id_t inbox_cap = (cap_id_t)((packed >> 16) & 0xffffU);
    cap_id_t driver_cap = KERN_INVALID_ID;
    int err;

    if (ns_ep_cap <= 0 || inbox_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_lookup_begin(ns_ep_cap, "dev.uart0", inbox_cap,
                                  &driver_cap, 1000);
    if (err == KERN_OK) {
        err = driver_ping(driver_cap, 1000);
    }
    if (err == KERN_OK) {
        err = nameserver_lookup_ack(inbox_cap);
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void driver_test_set_arg(task_id_t task_id, uint32_t arg) {
    tcb_t *tcb = task_get_tcb(task_id);
    if (tcb != NULL && tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
        *stacked_r0 = arg;
    }
}

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
 * Test 13: user-space driver server protocol ABI
 *============================================================================*/

static void test_driver_server_protocol_layout(void) {
    test_section("Test 13: driver server protocol ABI");

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_PING, 42);

    TEST_ASSERT_EQ((int)DRV_MAGIC, (int)msg.magic,
                   "driver protocol magic initialized");
    TEST_ASSERT_EQ((int)DRV_OP_PING, (int)msg.opcode,
                   "driver protocol opcode initialized");
    TEST_ASSERT_EQ((int)DRV_FLAG_NONE, (int)msg.flags,
                   "driver protocol flags initialized");
    TEST_ASSERT_EQ(42, (int)msg.seq,
                   "driver protocol sequence initialized");
    TEST_ASSERT_EQ(0, (int)msg.status,
                   "driver protocol status initialized");
    TEST_ASSERT_EQ(0, (int)msg.result,
                   "driver protocol result initialized");
    TEST_ASSERT(driver_opcode_valid(DRV_OP_PING),
                "driver protocol ping opcode valid");
    TEST_ASSERT(driver_opcode_valid(DRV_OP_WRITE),
                "driver protocol write opcode valid");
    TEST_ASSERT(!driver_opcode_valid(0),
                "driver protocol zero opcode rejected");
    TEST_ASSERT(!driver_opcode_valid(DRV_OP_POLL + 1U),
                "driver protocol unknown opcode rejected");
    TEST_ASSERT(sizeof(drv_msg_t) <= KERN_EP_MSG_SIZE,
                "driver message fits endpoint message");

    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_ping(0, 1000),
                   "driver ping rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_write(0, "x", 1, 1000),
                   "driver write rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_write(1, NULL, 1, 1000),
                   "driver write rejects NULL buffer");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_write(1, "x", DRV_PAYLOAD_MAX + 1U, 1000),
                   "driver write rejects oversized payload");
}

/*============================================================================
 * Test 14: root-created user-space UART server IPC
 *============================================================================*/

static void test_uart_user_server_ipc(void) {
    test_section("Test 14: user UART server IPC");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver server creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_srv",
                                        uart_server_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_srv_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server endpoint created");
    TEST_ASSERT(server_ep >= 0, "UART server endpoint id valid");
    TEST_ASSERT(root_ep_cap >= 0, "root receives UART server endpoint cap");
    TEST_ASSERT(server_ep_cap >= 0, "UART server receives endpoint cap");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server task started");

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_PING, 100);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server ping send OK");
    TEST_ASSERT_EQ((int)DRV_MAGIC, (int)msg.magic,
                   "UART server ping keeps magic");
    TEST_ASSERT_EQ((int)DRV_OP_PING, (int)msg.opcode,
                   "UART server ping keeps opcode");
    TEST_ASSERT_EQ(100, (int)msg.seq,
                   "UART server ping keeps sequence");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server ping status OK");

    driver_msg_init(&msg, DRV_OP_WRITE, 101);
    msg.payload[0] = 'O';
    msg.payload[1] = 'K';
    msg.length = 2;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server write send OK");
    TEST_ASSERT_EQ((int)DRV_OP_WRITE, (int)msg.opcode,
                   "UART server write keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server write status OK");
    TEST_ASSERT_EQ(2, (int)msg.result,
                   "UART server write returns byte count");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART server task joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART server task retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver server root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "driver server cleanup restored caps");
}

/*============================================================================
 * Test 15: user-space UART server protocol errors
 *============================================================================*/

static void test_uart_user_server_protocol_errors(void) {
    test_section("Test 15: user UART server protocol errors");

    root_bootstrap_init();

    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_err",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver error server creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    uint16_t cap_free_after_root = cap_free_count();
    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_srv_err",
                                        uart_server_error_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART error server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_srv_err_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     4,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART error server endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART error server task started");

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_PING, 200);
    msg.magic = 0;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server bad magic send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects bad magic");

    driver_msg_init(&msg, DRV_OP_POLL + 1U, 201);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server bad opcode send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects bad opcode");

    driver_msg_init(&msg, DRV_OP_READ, 202);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server unsupported read send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "UART server rejects unsupported read");

    driver_msg_init(&msg, DRV_OP_WRITE, 203);
    msg.length = DRV_PAYLOAD_MAX + 1U;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server oversized write send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects oversized write");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART error server task joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART error server task retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver error server root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "driver error server cleanup restored caps");
}

/*============================================================================
 * Test 16: UART driver registration through name server
 *============================================================================*/

static void test_uart_driver_nameserver_lookup(void) {
    test_section("Test 16: UART driver name-server lookup");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_ns",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver ns creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("drv_ns",
                                        driver_nameserver_task,
                                        NULL, 13, 1536,
                                        &ns_id, &ns_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver name server task created");

    ep_id_t ns_ep = KERN_INVALID_ID;
    cap_id_t root_ns_ep_cap = KERN_INVALID_ID;
    cap_id_t service_ns_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(ns_task_cap,
                                                     "drv_ns_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &ns_ep,
                                                     &root_ns_ep_cap,
                                                     &service_ns_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver name server endpoint created");

    task_id_t uart_id = KERN_INVALID_ID;
    cap_id_t uart_task_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service("drv_uart_srv",
                                            uart_server_ping_task,
                                            NULL, 13, 768,
                                            &uart_id, &uart_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver UART service task created");

    ep_id_t uart_ep = KERN_INVALID_ID;
    cap_id_t root_uart_ep_cap = KERN_INVALID_ID;
    cap_id_t service_uart_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(uart_task_cap,
                                                     "drv_uart_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &uart_ep,
                                                     &root_uart_ep_cap,
                                                     &service_uart_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver UART service endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(ns_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver name server task started");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(uart_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver UART service task started");

    ep_id_t inbox_ep = KERN_INVALID_ID;
    if (err == KERN_OK) {
        inbox_ep = endpoint_create("drv_lookup_inbox", KERN_EP_MSG_SIZE, 2);
    }
    TEST_ASSERT(inbox_ep >= 0, "driver lookup inbox endpoint created");

    task_id_t reg_client = KERN_INVALID_ID;
    task_id_t lookup_client = KERN_INVALID_ID;
    if (inbox_ep >= 0) {
        reg_client = task_create_user("drv_reg_client",
                                      driver_register_client_task,
                                      NULL, 14, 896);
        lookup_client = task_create_user("drv_lookup_client",
                                         driver_lookup_ping_client_task,
                                         NULL, 14, 1024);
    }
    TEST_ASSERT(reg_client >= 0, "driver register client created");
    TEST_ASSERT(lookup_client >= 0, "driver lookup client created");

    cap_id_t reg_ns_cap = KERN_INVALID_ID;
    cap_id_t reg_uart_cap = KERN_INVALID_ID;
    tcb_t *reg_tcb = task_get_tcb(reg_client);
    if (reg_tcb != NULL) {
        reg_ns_cap = cap_create_for(reg_tcb,
                                    (void *)(uintptr_t)(ns_ep + 1),
                                    CAP_OBJ_ENDPOINT,
                                    CAP_READ | CAP_WRITE);
        reg_uart_cap = cap_create_for(reg_tcb,
                                      (void *)(uintptr_t)(uart_ep + 1),
                                      CAP_OBJ_ENDPOINT,
                                      CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }

    cap_id_t lookup_ns_cap = KERN_INVALID_ID;
    cap_id_t lookup_inbox_cap = KERN_INVALID_ID;
    tcb_t *lookup_tcb = task_get_tcb(lookup_client);
    if (lookup_tcb != NULL) {
        lookup_ns_cap = cap_create_for(lookup_tcb,
                                       (void *)(uintptr_t)(ns_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        lookup_inbox_cap = cap_create_for(lookup_tcb,
                                          (void *)(uintptr_t)(inbox_ep + 1),
                                          CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }

    TEST_ASSERT(reg_ns_cap >= 0, "driver register client receives ns cap");
    TEST_ASSERT(reg_uart_cap >= 0, "driver register client receives UART cap");
    TEST_ASSERT(lookup_ns_cap >= 0, "driver lookup client receives ns cap");
    TEST_ASSERT(lookup_inbox_cap >= 0,
                "driver lookup client receives inbox cap");

    if (reg_client >= 0 && reg_ns_cap >= 0 && reg_uart_cap >= 0) {
        uint32_t packed = ((uint32_t)(uint16_t)reg_uart_cap << 16) |
                          (uint32_t)(uint16_t)reg_ns_cap;
        driver_test_set_arg(reg_client, packed);
        err = task_start(reg_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver register client started");
    }

    void *retval = NULL;
    if (reg_client >= 0) {
        err = task_join(reg_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver register client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver register client retval OK");
    }

    if (lookup_client >= 0 && lookup_ns_cap >= 0 && lookup_inbox_cap >= 0) {
        uint32_t packed = ((uint32_t)(uint16_t)lookup_inbox_cap << 16) |
                          (uint32_t)(uint16_t)lookup_ns_cap;
        driver_test_set_arg(lookup_client, packed);
        err = task_start(lookup_client);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver lookup client started");
    }

    retval = NULL;
    if (lookup_client >= 0) {
        err = task_join(lookup_client, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver lookup client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver lookup client retval OK");
    }

    retval = NULL;
    if (uart_id >= 0) {
        err = task_join(uart_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver UART service joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver UART service retval OK");
    }

    retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver name server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver name server retval OK");
    }

    if (reg_client >= 0 &&
        task_get_state(reg_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(reg_client);
    }
    if (lookup_client >= 0 &&
        task_get_state(lookup_client) != TASK_STATE_TERMINATED) {
        (void)task_delete(lookup_client);
    }
    if (uart_id >= 0 && task_get_state(uart_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(uart_id);
    }
    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (inbox_ep >= 0) {
        (void)endpoint_delete(inbox_ep);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver ns root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver name-server cleanup restored caps");
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
    test_driver_server_protocol_layout();
    test_uart_user_server_ipc();
    test_uart_user_server_protocol_errors();
    test_uart_driver_nameserver_lookup();
}

TEST_MODULE_REGISTER(driver, test_driver_module);

#endif /* DRIVER_ENABLE && TEST_ENABLE */
