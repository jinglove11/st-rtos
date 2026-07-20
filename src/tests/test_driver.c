/**
 * @file test_driver.c
 * @brief 设备驱动框架测试
 */

#include "test_framework.h"
#include "kernel_config.h"
#include "device.h"
#include "fs_types.h"   /* Phase F2: dev_ops_t (原在 inode.h) */
/* Phase F2: vfs.h/devfs.h 移除 (内核 VFS 直调已清除) */
#include "trace.h"
#include "stats.h"
#include "user_api.h"
#include "root_bootstrap.h"
#include "task.h"
#include "capability.h"
#include "endpoint.h"
#include "driver_proto.h"
#include "driver_registry.h"
#include "driver_client.h"
#include "driver_runtime.h"
#include "nameserver.h"
#include "mem.h"
#include "irq.h"
#include "board_config.h"
#include <string.h>

#if DRIVER_ENABLE && TEST_ENABLE

#define DRIVER_TEST_IRQ_DETACH ((int16_t)(BOARD_IRQ_COUNT - 1U))

/* M2-Step2b: 前向声明,双 cap arg 传递机制 (定义在 driver_test_set_arg 后) */
static cap_id_t driver_test_get_cap_b(void);

static void driver_root_dummy_task(void *arg) {
    (void)arg;
    task_exit(NULL);
}

static void uart_server_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 7);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_error_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 13);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_nameserver_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_nameserver_release_task(void *arg) {
    int err = nameserver_service_run((int)(uintptr_t)arg, 5);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_ping_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 7);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 1);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_query_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 2);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_detach_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 7);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_irq_detach_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 8);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_user_irq_detach_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 4);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_rw_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 5);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_user_session_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 9);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_poll_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 6);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_attach_error_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 1);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_irq_notify_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 7);
    sys_task_exit((void *)(intptr_t)err);
}

static void uart_server_irq_user_task(void *arg) {
    int err = uart_server_run((int)(uintptr_t)arg, 8);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_attach_client_task(void *arg) {
    int ep_cap = (int)(intptr_t)arg;
    cap_id_t resource_cap = driver_test_get_cap_b();
    int err;

    if (ep_cap <= 0 || resource_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_attach_cap(ep_cap, resource_cap, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_resource_session_client_task(void *arg) {
    int ep_cap = (int)(intptr_t)arg;
    cap_id_t resource_cap = driver_test_get_cap_b();
    uint32_t events = 0;
    uint32_t resources = 0;
    int err;

    if (ep_cap <= 0 || resource_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_attach_cap(ep_cap, resource_cap, 1000);
    if (err == KERN_OK) {
        err = driver_get_resources(ep_cap, &resources, 1000);
        if (err == KERN_OK && (resources & DRV_RESOURCE_BIT_MMIO) == 0U) {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_open(ep_cap, 0, 1000);
    }
    if (err == KERN_OK) {
        err = driver_detach_resource(ep_cap, DRV_RESOURCE_MMIO, 1000);
        if (err == KERN_ERR_BUSY) {
            err = KERN_OK;
        } else {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_get_events(ep_cap, &events, 1000);
        if (err == KERN_OK && (events & DRV_EVENT_WRITABLE) == 0U) {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_write(ep_cap, "OK", 2, 1000);
        if (err == 2) {
            err = KERN_OK;
        }
    }
    if (err == KERN_OK) {
        err = driver_close(ep_cap, 1000);
    }
    if (err == KERN_OK) {
        err = driver_detach_resource(ep_cap, DRV_RESOURCE_MMIO, 1000);
    }
    if (err == KERN_OK) {
        err = driver_get_resources(ep_cap, &resources, 1000);
        if (err == KERN_OK && (resources & DRV_RESOURCE_BIT_MMIO) != 0U) {
            err = KERN_ERR_STATE;
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void driver_irq_only_client_task(void *arg) {
    int ep_cap = (int)(intptr_t)arg;
    cap_id_t resource_cap = driver_test_get_cap_b();
    uint32_t resources = 0;
    int err;

    if (ep_cap <= 0 || resource_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_attach_resource(ep_cap, DRV_RESOURCE_IRQ, resource_cap, 1000);
    if (err == KERN_OK) {
        err = driver_open(ep_cap, 0, 1000);
        if (err == KERN_ERR_CAP) {
            err = KERN_OK;
        } else {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_detach_resource(ep_cap, DRV_RESOURCE_IRQ, 1000);
    }
    if (err == KERN_OK) {
        err = driver_get_resources(ep_cap, &resources, 1000);
        if (err == KERN_OK && (resources & DRV_RESOURCE_BIT_IRQ) != 0U) {
            err = KERN_ERR_STATE;
        }
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void driver_irq_event_client_task(void *arg) {
    int ep_cap = (int)(cap_id_t)(uintptr_t)arg;
    uint32_t events = 0;
    uint8_t ch = 0xffU;
    int err;

    if (ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_get_events(ep_cap, &events, 1000);
    if (err == KERN_OK && (events & DRV_EVENT_READABLE) == 0U) {
        err = KERN_ERR_STATE;
    }
    if (err == KERN_OK) {
        err = driver_open(ep_cap, 0, 1000);
    }
    if (err == KERN_OK) {
        err = driver_read(ep_cap, &ch, 1, 1000);
        if (err == 1) {
            err = KERN_OK;
        }
    }
    if (err == KERN_OK) {
        err = driver_get_events(ep_cap, &events, 1000);
        if (err == KERN_OK && (events & DRV_EVENT_READABLE) != 0U) {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_close(ep_cap, 1000);
    }

    sys_task_exit((void *)(intptr_t)err);
}

static void driver_attach_no_transfer_client_task(void *arg) {
    int ep_cap = (int)(intptr_t)arg;
    cap_id_t resource_cap = driver_test_get_cap_b();
    int err;

    if (ep_cap <= 0 || resource_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_attach_cap(ep_cap, resource_cap, 20);
    if (err == KERN_ERR_CAP || err == KERN_ERR_TIMEOUT) {
        err = KERN_OK;
    } else {
        err = KERN_ERR_STATE;
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_attach_bad_type_client_task(void *arg) {
    int ep_cap = (int)(intptr_t)arg;
    cap_id_t resource_cap = driver_test_get_cap_b();
    drv_msg_t msg;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    int err;

    if (ep_cap <= 0 || resource_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    for (uint32_t i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }
    xfers[0].src_cap = resource_cap;
    xfers[0].rights = CAP_READ;
    xfers[0].flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 0);
    msg.command = 0xffffffffU;
    err = sys_ep_send_caps(ep_cap, &msg, xfers, 1, 1000);
    if (err == KERN_OK) {
        err = msg.status;
    }
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_register_client_task(void *arg) {
    int ns_ep_cap = (int)(intptr_t)arg;
    cap_id_t service_ep_cap = driver_test_get_cap_b();
    int err;

    if (ns_ep_cap <= 0 || service_ep_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = nameserver_register(ns_ep_cap, "dev.uart0", service_ep_cap,
                              0x55415254U, 1000);
    sys_task_exit((void *)(intptr_t)err);
}

static void driver_lookup_ping_client_task(void *arg) {
    int ns_ep_cap = (int)(intptr_t)arg;
    cap_id_t inbox_cap = driver_test_get_cap_b();
    cap_id_t driver_cap = KERN_INVALID_ID;
    uint8_t read_buf[4];
    uint32_t events = 0;
    uint32_t value = 0;
    int err;

    if (ns_ep_cap <= 0 || inbox_cap <= 0) {
        sys_task_exit((void *)(intptr_t)KERN_ERR_PARAM);
    }

    err = driver_lookup_uart(ns_ep_cap, inbox_cap, &driver_cap, 1000);
    if (err == KERN_OK) {
        err = driver_ping(driver_cap, 1000);
    }
    if (err == KERN_OK) {
        err = driver_open(driver_cap, 0, 1000);
    }
    if (err == KERN_OK) {
        err = driver_poll(driver_cap, &events, 1000);
        if (err == KERN_OK && (events & DRV_EVENT_WRITABLE) == 0U) {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_ioctl(driver_cap, DRV_IOCTL_GET_EVENTS, &value, 1000);
        if (err == KERN_OK && (value & DRV_EVENT_WRITABLE) == 0U) {
            err = KERN_ERR_STATE;
        }
    }
    if (err == KERN_OK) {
        err = driver_read(driver_cap, read_buf, sizeof(read_buf), 1000);
        if (err == 0) {
            err = KERN_OK;
        }
    }
    if (err == KERN_OK) {
        err = driver_write(driver_cap, "OK", 2, 1000);
        if (err == 2) {
            err = KERN_OK;
        }
    }
    if (err == KERN_OK) {
        err = driver_close(driver_cap, 1000);
    }
    if (err == KERN_OK) {
        err = driver_release_service(inbox_cap, driver_cap);
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
 * M2-Step2b: 双 cap arg 传递机制
 *
 * 历史问题: 旧代码用 (cap_a << 16) | cap_b 把两个 cap 打包进单个 R0,
 * 假设每个 cap <= 16 位。cap_id_t 扩到 int32_t 后此假设失效。
 *
 * 新机制: cap_a 仍走 R0 (driver_test_set_arg),cap_b 存全局 pair 表,
 * 任务入口用 sys_task_self() 查自己的 cap_b。per-task_id 索引,
 * 支持并发测试任务。
 *============================================================================*/
#define DRIVER_TEST_PAIR_MAX 16
typedef struct {
    task_id_t task_id;   /* -1 = 空闲槽 */
    cap_id_t  cap_b;
} driver_test_pair_slot_t;

static driver_test_pair_slot_t driver_test_pair_slots[DRIVER_TEST_PAIR_MAX] = {
    [0 ... DRIVER_TEST_PAIR_MAX - 1] = { .task_id = -1, .cap_b = (cap_id_t)-1 },
};

static void driver_test_set_arg_pair(task_id_t task_id, int cap_a, cap_id_t cap_b) {
    /* cap_a 经 R0 传递 (driver_test_set_arg) */
    driver_test_set_arg(task_id, (uint32_t)cap_a);
    /* cap_b 存 pair 表,任务入口凭 task_id 查。
     * 复用同 task_id 槽,否则找空槽。 */
    for (int i = 0; i < DRIVER_TEST_PAIR_MAX; i++) {
        if (driver_test_pair_slots[i].task_id == task_id) {
            driver_test_pair_slots[i].cap_b = cap_b;
            return;
        }
    }
    for (int i = 0; i < DRIVER_TEST_PAIR_MAX; i++) {
        if (driver_test_pair_slots[i].task_id < 0) {
            driver_test_pair_slots[i].task_id = task_id;
            driver_test_pair_slots[i].cap_b = cap_b;
            return;
        }
    }
}

static void driver_test_reset_pairs(void) {
    for (int i = 0; i < DRIVER_TEST_PAIR_MAX; i++) {
        driver_test_pair_slots[i].task_id = -1;
        driver_test_pair_slots[i].cap_b = (cap_id_t)-1;
    }
}

static cap_id_t driver_test_get_cap_b(void) {
    task_id_t self = (task_id_t)sys_task_self();
    for (int i = 0; i < DRIVER_TEST_PAIR_MAX; i++) {
        if (driver_test_pair_slots[i].task_id == self) {
            return driver_test_pair_slots[i].cap_b;
        }
    }
    return (cap_id_t)-1;
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
    /* Phase D:/dev/null 现在经 fs_server 访问 (sys_open 返回 NOSYS)。
     * /dev/null 在内核 devfs 注册 (devfs_init),不在 device_pool。
     * 设备文件操作测试见 test_fs_devfs。 */
    test_section("Test 6: /dev/null (devfs, access via fs_server)");
    test_pass("/dev/null in devfs (access migrated to fs_server)");
}

/*============================================================================
 * Test 7: /dev/null 读写 (Phase D:迁移到 fs_server)
 *============================================================================*/

static void test_devnull_rw(void) {
    /* Phase D:/dev/null 读写经 fs_server (sys_open 返回 NOSYS)。
     * 见 test_fs_devfs 的 devfs 转发端到端测试。 */
    test_section("Test 7: /dev/null R/W (migrated to fs_server)");
    test_pass("/dev/null R/W via fs_server (see test_fs_devfs)");
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

static void __attribute__((unused)) test_uart_devfs(void) {
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

static void __attribute__((unused)) test_led_devices(void) {
    test_section("Test 10: LED devices registered");

    device_t *led1 = device_find("led1");
#if TARGET_BOARD == BOARD_STM32F767_NUCLEO
    TEST_ASSERT_NOT_NULL(led1, "led1 in registry");

    device_t *led2 = device_find("led2");
    TEST_ASSERT_NOT_NULL(led2, "led2 in registry");

    device_t *led3 = device_find("led3");
    TEST_ASSERT_NOT_NULL(led3, "led3 in registry");
#else
    TEST_ASSERT_NULL(led1, "Pico 2 W has no direct-GPIO led1 device");
    TEST_ASSERT_NULL(device_find("led2"),
                     "Pico 2 W has no direct-GPIO led2 device");
    TEST_ASSERT_NULL(device_find("led3"),
                     "Pico 2 W has no direct-GPIO led3 device");
#endif
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

static void __attribute__((unused)) test_device_probe_remove_diag(void) {
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
    /* Phase F2: vfs_lookup("/dev/probe0") 移除 (内核 devfs 移除) */

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

static void __attribute__((unused)) test_device_event_ioctl(void) {
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
    TEST_ASSERT(driver_opcode_valid(DRV_OP_ATTACH),
                "driver protocol attach opcode valid");
    TEST_ASSERT(driver_opcode_valid(DRV_OP_DETACH),
                "driver protocol detach opcode valid");
    TEST_ASSERT(!driver_opcode_valid(0),
                "driver protocol zero opcode rejected");
    TEST_ASSERT(!driver_opcode_valid(DRV_OP_DETACH + 1U),
                "driver protocol unknown opcode rejected");
    TEST_ASSERT(sizeof(drv_msg_t) <= KERN_EP_MSG_SIZE,
                "driver message fits endpoint message");

    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_ping(0, 1000),
                   "driver ping rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_open(0, 0, 1000),
                   "driver open rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_close(0, 1000),
                   "driver close rejects invalid endpoint");
    uint32_t events = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_poll(0, &events, 1000),
                   "driver poll rejects invalid endpoint");
    TEST_ASSERT_EQ(0, (int)events,
                   "driver poll clears events before endpoint validation");
    events = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_poll(1, NULL, 1000),
                   "driver poll rejects NULL events");
    TEST_ASSERT_EQ((int)0x55aa55aaU, (int)events,
                   "driver poll leaves unrelated caller storage untouched");
    uint32_t value = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl(0, DRV_IOCTL_GET_EVENTS, &value, 1000),
                   "driver ioctl rejects invalid endpoint");
    TEST_ASSERT_EQ(0, (int)value,
                   "driver ioctl clears value before endpoint validation");
    value = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl(1, DRV_IOCTL_GET_EVENTS, NULL, 1000),
                   "driver ioctl rejects NULL output");
    TEST_ASSERT_EQ((int)0x55aa55aaU, (int)value,
                   "driver ioctl leaves unrelated caller storage untouched");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_events(0, &value, 1000),
                   "driver get-events rejects invalid endpoint");
    value = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_events(1, NULL, 1000),
                   "driver get-events rejects NULL output");
    TEST_ASSERT_EQ((int)0x55aa55aaU, (int)value,
                   "driver get-events leaves unrelated storage untouched");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl(1, DRV_IOCTL_GET_RESOURCES, NULL, 1000),
                   "driver resource ioctl rejects NULL output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_resources(0, &value, 1000),
                   "driver get-resources rejects invalid endpoint");
    value = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_resources(1, NULL, 1000),
                   "driver get-resources rejects NULL output");
    TEST_ASSERT_EQ((int)0x55aa55aaU, (int)value,
                   "driver get-resources leaves unrelated storage untouched");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl(1, DRV_IOCTL_GET_STATUS, NULL, 1000),
                   "driver status ioctl rejects NULL output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_status(0, &value, 1000),
                   "driver get-status rejects invalid endpoint");
    value = 0x55aa55aaU;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_get_status(1, NULL, 1000),
                   "driver get-status rejects NULL output");
    TEST_ASSERT_EQ((int)0x55aa55aaU, (int)value,
                   "driver get-status leaves unrelated storage untouched");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_clear_status(0, 1000),
                   "driver clear-status rejects invalid endpoint");
    uint8_t read_buf[4];
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_read(0, read_buf, 1, 1000),
                   "driver read rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_read(1, NULL, 1, 1000),
                   "driver read rejects NULL buffer");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_read(1, read_buf, DRV_PAYLOAD_MAX + 1U, 1000),
                   "driver read rejects oversized request");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_attach_cap(0, 1, 1000),
                   "driver attach rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_attach_cap(1, 0, 1000),
                   "driver attach rejects invalid resource cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_attach_resource(1, 0xffffffffU, 1, 1000),
                   "driver attach rejects unknown resource type");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_detach_resource(0, DRV_RESOURCE_MMIO, 1000),
                   "driver detach rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_detach_resource(1, 0xffffffffU, 1000),
                   "driver detach rejects unknown resource type");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_write(0, "x", 1, 1000),
                   "driver write rejects invalid endpoint");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, driver_write(1, NULL, 1, 1000),
                   "driver write rejects NULL buffer");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_write(1, "x", DRV_PAYLOAD_MAX + 1U, 1000),
                   "driver write rejects oversized payload");
}

/*============================================================================
 * Test 14: user-space driver registry descriptor
 *============================================================================*/

static void test_driver_registry_descriptor(void) {
    test_section("Test 14: driver registry descriptor");

    TEST_ASSERT(driver_registry_count() > 0,
                "driver registry has entries");

    const driver_descriptor_t *desc = driver_registry_find("dev.uart0");
    TEST_ASSERT_NOT_NULL((void *)desc,
                         "driver registry finds dev.uart0");
    if (desc == NULL) {
        return;
    }

    TEST_ASSERT_EQ((int)KERN_OK, driver_registry_validate_all(),
                   "driver registry validates all descriptors");
    TEST_ASSERT_EQ((int)KERN_OK, driver_registry_validate_desc(desc),
                   "driver registry validates uart descriptor");
    TEST_ASSERT_EQ(0, strcmp(desc->device_name, "uart0"),
                   "driver registry records uart0 device");
    TEST_ASSERT((desc->ops & DRIVER_OP_BIT_PING) != 0U,
                "driver registry records ping op");
    TEST_ASSERT((desc->ops & DRIVER_OP_BIT_OPEN) != 0U,
                "driver registry records open op");
    TEST_ASSERT((desc->ops & DRIVER_OP_BIT_IOCTL) != 0U,
                "driver registry records ioctl op");
    TEST_ASSERT((desc->ops & DRIVER_OP_BIT_ATTACH) != 0U,
                "driver registry records attach op");
    TEST_ASSERT((desc->ops & DRIVER_OP_BIT_DETACH) != 0U,
                "driver registry records detach op");
    TEST_ASSERT((desc->ioctls & DRIVER_IOCTL_BIT_GET_EVENTS) != 0U,
                "driver registry records get-events ioctl");
    TEST_ASSERT((desc->ioctls & DRIVER_IOCTL_BIT_GET_RESOURCES) != 0U,
                "driver registry records get-resources ioctl");
    TEST_ASSERT((desc->ioctls & DRIVER_IOCTL_BIT_GET_STATUS) != 0U,
                "driver registry records get-status ioctl");
    TEST_ASSERT((desc->ioctls & DRIVER_IOCTL_BIT_CLEAR_STATUS) != 0U,
                "driver registry records clear-status ioctl");
    TEST_ASSERT((desc->resources & DRV_RESOURCE_BIT_MMIO) != 0U,
                "driver registry records MMIO resource");
    TEST_ASSERT((desc->resources & DRV_RESOURCE_BIT_IRQ) != 0U,
                "driver registry records IRQ resource");
    TEST_ASSERT((desc->required_resources & DRV_RESOURCE_BIT_MMIO) != 0U,
                "driver registry records MMIO as required");
    TEST_ASSERT((desc->required_resources & DRV_RESOURCE_BIT_IRQ) == 0U,
                "driver registry leaves IRQ out of required resources");
    TEST_ASSERT((desc->optional_resources & DRV_RESOURCE_BIT_IRQ) != 0U,
                "driver registry records IRQ as optional");
    TEST_ASSERT((desc->optional_resources & DRV_RESOURCE_BIT_MMIO) == 0U,
                "driver registry leaves MMIO out of optional resources");
    TEST_ASSERT((desc->status_bits & DRV_STATUS_ERROR) != 0U,
                "driver registry records status bits");
    TEST_ASSERT_NULL((void *)driver_registry_get(driver_registry_count()),
                     "driver registry rejects out-of-range index");
    TEST_ASSERT_NULL((void *)driver_registry_find("dev.missing"),
                     "driver registry rejects missing service");
    TEST_ASSERT_NULL((void *)driver_registry_find(NULL),
                     "driver registry rejects NULL service");
    const driver_descriptor_t *named_desc = NULL;
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_registry_query("dev.uart0", &named_desc),
                   "driver registry query by name returns OK");
    TEST_ASSERT(named_desc == desc,
                "driver registry query by name returns descriptor");
    named_desc = desc;
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   driver_registry_query("dev.missing", &named_desc),
                   "driver registry query by name reports missing");
    TEST_ASSERT_NULL((void *)named_desc,
                     "driver registry query by name clears missing output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_query(NULL, &named_desc),
                   "driver registry query by name rejects NULL service");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_query("dev.uart0", NULL),
                   "driver registry query by name rejects NULL output");
    TEST_ASSERT(driver_descriptor_supports(desc,
                                           DRIVER_OP_BIT_OPEN |
                                           DRIVER_OP_BIT_WRITE,
                                           DRIVER_IOCTL_BIT_GET_STATUS,
                                           DRV_RESOURCE_BIT_MMIO),
                "driver descriptor supports required caps");
    TEST_ASSERT(driver_descriptor_supports(desc, 0, 0, 0),
                "driver descriptor supports empty caps");
    TEST_ASSERT(!driver_descriptor_supports(NULL, 0, 0, 0),
                "driver descriptor support rejects NULL descriptor");
    TEST_ASSERT(!driver_descriptor_supports(desc, (1U << 31), 0, 0),
                "driver descriptor support rejects missing op");
    TEST_ASSERT(!driver_descriptor_supports(desc, 0, (1U << 31), 0),
                "driver descriptor support rejects missing ioctl");
    TEST_ASSERT(!driver_descriptor_supports(desc, 0, 0, (1U << 31)),
                "driver descriptor support rejects missing resource");
    TEST_ASSERT(driver_registry_find_by_caps(DRIVER_OP_BIT_OPEN |
                                             DRIVER_OP_BIT_WRITE,
                                             DRIVER_IOCTL_BIT_GET_STATUS,
                                             DRV_RESOURCE_BIT_MMIO) == desc,
                "driver registry finds descriptor by required caps");
    const driver_descriptor_t *query_desc = NULL;
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_registry_query_by_caps(DRIVER_OP_BIT_OPEN,
                                                 DRIVER_IOCTL_BIT_GET_STATUS,
                                                 DRV_RESOURCE_BIT_MMIO,
                                                 &query_desc),
                   "driver registry query by caps returns OK");
    TEST_ASSERT(query_desc == desc,
                "driver registry query by caps returns descriptor");
    query_desc = desc;
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   driver_registry_query_by_caps((1U << 31), 0, 0,
                                                 &query_desc),
                   "driver registry query reports missing descriptor");
    TEST_ASSERT_NULL((void *)query_desc,
                     "driver registry query clears missing output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_query_by_caps(0, 0, 0, NULL),
                   "driver registry query rejects NULL output");
    TEST_ASSERT(driver_registry_find_by_caps(0, 0, 0) == desc,
                "driver registry accepts empty capability query");
    TEST_ASSERT_NULL((void *)driver_registry_find_by_caps((1U << 31), 0, 0),
                     "driver registry rejects unknown required op query");
    TEST_ASSERT_NULL((void *)driver_registry_find_by_caps(0, (1U << 31), 0),
                     "driver registry rejects unknown required ioctl query");
    TEST_ASSERT_NULL((void *)driver_registry_find_by_caps(0, 0, (1U << 31)),
                     "driver registry rejects unknown required resource query");
    cap_id_t service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_lookup_service(0, "dev.uart0", 0, 0, 0, 1,
                                         &service_cap, 1000),
                   "driver lookup rejects bad name-server cap");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "driver lookup clears output on bad cap");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_lookup_service(1, "dev.uart0", 0, 0, 0, 0,
                                         &service_cap, 1000),
                   "driver lookup rejects bad inbox cap");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "driver lookup clears output on bad inbox");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_lookup_service(1, "dev.uart0", 0, 0, 0, 1,
                                         NULL, 1000),
                   "driver lookup rejects NULL output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_lookup_uart(1, 1, NULL, 1000),
                   "driver UART lookup rejects NULL output");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   driver_lookup_service(1, "dev.missing", 0, 0, 0, 1,
                                         &service_cap, 1000),
                   "driver lookup rejects missing registry descriptor");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "driver lookup clears output on missing descriptor");
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   driver_lookup_service(1, "dev.uart0", (1U << 31), 0, 0,
                                         1, &service_cap, 1000),
                   "driver lookup rejects unsupported required caps");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_release_service(0, service_cap),
                   "driver release rejects bad inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   driver_name_server_status(0, 1000),
                   "driver name-server status reports unbound");
    driver_runtime_clear_name_server();
    driver_runtime_clear_inbox();
    int ns_status = KERN_OK;
    TEST_ASSERT(!driver_runtime_name_server_bound(),
                "driver runtime reports name-server unbound");
    TEST_ASSERT_EQ((int)DRIVER_RUNTIME_NS_UNBOUND,
                   (int)driver_runtime_name_server_state(0, &ns_status),
                   "driver runtime state reports name-server unbound");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, ns_status,
                   "driver runtime unbound state reports status");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID,
                   (int)driver_runtime_name_server_cap(),
                   "driver runtime clears name-server cap");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   driver_runtime_name_server_status(0),
                   "driver runtime reports unbound name-server");
    TEST_ASSERT(!driver_runtime_inbox_bound(),
                "driver runtime reports inbox unbound");
    TEST_ASSERT(!driver_runtime_inbox_owned(),
                "driver runtime reports inbox unowned");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID,
                   (int)driver_runtime_inbox_cap(),
                   "driver runtime clears inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_runtime_bind_inbox(0),
                   "driver runtime rejects bad inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_runtime_bind_owned_inbox(0),
                   "driver runtime rejects bad owned inbox cap");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_runtime_bind_name_server(0),
                   "driver runtime rejects bad name-server cap");
    const char *ready_reason = NULL;
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   driver_runtime_lookup_ready(0, &ready_reason),
                   "driver runtime lookup ready rejects missing name-server");
    TEST_ASSERT(ready_reason != NULL &&
                strcmp(ready_reason, "name-server") == 0,
                "driver runtime lookup ready reports missing name-server");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   driver_runtime_lookup_service("dev.uart0", 0, 0, 0,
                                                 1, &service_cap, 0),
                   "driver runtime lookup reports unbound name-server");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "driver runtime lookup clears unbound output");
    service_cap = 123;
    TEST_ASSERT_EQ((int)KERN_ERR_STATE,
                   driver_runtime_lookup_uart(1, &service_cap, 0),
                   "driver runtime UART lookup reports unbound name-server");
    TEST_ASSERT_EQ((int)KERN_INVALID_ID, (int)service_cap,
                   "driver runtime UART lookup clears unbound output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_runtime_lookup_service("dev.uart0", 0, 0, 0,
                                                 1, NULL, 0),
                   "driver runtime lookup rejects NULL output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_runtime_lookup_uart(1, NULL, 0),
                   "driver runtime UART lookup rejects NULL output");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_runtime_bind_name_server(123),
                   "driver runtime records name-server cap");
    TEST_ASSERT(driver_runtime_name_server_bound(),
                "driver runtime reports name-server bound");
    TEST_ASSERT_EQ((int)DRIVER_RUNTIME_NS_BOUND,
                   (int)driver_runtime_name_server_state(0, &ns_status),
                   "driver runtime state reports bound bad cap");
    TEST_ASSERT(ns_status != KERN_OK,
                "driver runtime bound bad cap reports non-OK status");
    TEST_ASSERT_EQ(123, (int)driver_runtime_name_server_cap(),
                   "driver runtime returns bound name-server cap");
    ready_reason = NULL;
    TEST_ASSERT(driver_runtime_lookup_ready(0, &ready_reason) != KERN_OK,
                "driver runtime lookup ready rejects bad name-server cap");
    TEST_ASSERT(ready_reason != NULL &&
                strcmp(ready_reason, "name-server") == 0,
                "driver runtime lookup ready reports bad name-server");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_runtime_bind_inbox(456),
                   "driver runtime records inbox cap");
    TEST_ASSERT(driver_runtime_inbox_bound(),
                "driver runtime reports inbox bound");
    TEST_ASSERT(!driver_runtime_inbox_owned(),
                "driver runtime reports manual inbox external");
    TEST_ASSERT_EQ(456, (int)driver_runtime_inbox_cap(),
                   "driver runtime returns bound inbox cap");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_runtime_bind_owned_inbox(789),
                   "driver runtime records owned inbox cap");
    TEST_ASSERT(driver_runtime_inbox_owned(),
                "driver runtime reports owned inbox");
    TEST_ASSERT_EQ(789, (int)driver_runtime_inbox_cap(),
                   "driver runtime returns owned inbox cap");
    driver_runtime_clear_inbox();
    TEST_ASSERT(!driver_runtime_inbox_owned(),
                "driver runtime clear drops inbox ownership");
    driver_runtime_clear_name_server();
    TEST_ASSERT_EQ(0, strcmp(driver_error_name(KERN_OK), "ok"),
                   "driver error names OK");
    TEST_ASSERT_EQ(0, strcmp(driver_error_name(KERN_ERR_PARAM), "param"),
                   "driver error names param");
    TEST_ASSERT_EQ(0, strcmp(driver_error_name(KERN_ERR_NOEXIST), "noexist"),
                   "driver error names missing service");
    TEST_ASSERT_EQ(0, strcmp(driver_error_name(KERN_ERR_CAP), "cap"),
                   "driver error names cap");
    TEST_ASSERT_EQ(0, strcmp(driver_error_name(12345), "unknown"),
                   "driver error names unknown status");

    driver_descriptor_t bad = *desc;
    bad.service_name = "";
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects empty service name");
    bad = *desc;
    bad.ops |= (1U << 31);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects unknown op bit");
    bad = *desc;
    bad.ioctls |= (1U << 31);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects unknown ioctl bit");
    bad = *desc;
    bad.ops &= ~DRIVER_OP_BIT_IOCTL;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects ioctls without ioctl op");
    bad = *desc;
    bad.resources |= (1U << 31);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects unknown resource bit");
    bad = *desc;
    bad.required_resources |= (1U << 31);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects required resource outside set");
    bad = *desc;
    bad.optional_resources = DRV_RESOURCE_BIT_MMIO;
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects overlapping resources");
    bad = *desc;
    bad.status_bits |= (1U << 31);
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(&bad),
                   "driver registry rejects unknown status bit");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_registry_validate_desc(NULL),
                   "driver registry rejects NULL descriptor");

    uint16_t opcode = 0;
    uint32_t op_bit = 0;
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_op_bit_to_opcode(DRIVER_OP_BIT_PING, &opcode),
                   "driver registry maps ping op bit");
    TEST_ASSERT_EQ((int)DRV_OP_PING, (int)opcode,
                   "driver registry ping opcode matches protocol");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_opcode_to_op_bit(DRV_OP_WRITE, &op_bit),
                   "driver registry maps write opcode");
    TEST_ASSERT_EQ((int)DRIVER_OP_BIT_WRITE, (int)op_bit,
                   "driver registry write bit matches descriptor");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_op_bit_to_opcode(DRIVER_OP_BIT_ATTACH, &opcode),
                   "driver registry maps attach op bit");
    TEST_ASSERT_EQ((int)DRV_OP_ATTACH, (int)opcode,
                   "driver registry attach opcode matches protocol");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_opcode_to_op_bit(DRV_OP_DETACH, &op_bit),
                   "driver registry maps detach opcode");
    TEST_ASSERT_EQ((int)DRIVER_OP_BIT_DETACH, (int)op_bit,
                   "driver registry detach bit matches descriptor");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_op_bit_to_opcode(0, &opcode),
                   "driver registry rejects zero op bit");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_opcode_to_op_bit(0xffffU, &op_bit),
                   "driver registry rejects unknown opcode");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_op_bit_to_opcode(DRIVER_OP_BIT_PING, NULL),
                   "driver registry rejects NULL opcode output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_opcode_to_op_bit(DRV_OP_PING, NULL),
                   "driver registry rejects NULL op bit output");

    uint32_t command = 0;
    uint32_t bit = 0;
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_ioctl_bit_to_command(DRIVER_IOCTL_BIT_GET_EVENTS,
                                                &command),
                   "driver registry maps events ioctl bit");
    TEST_ASSERT_EQ((int)DRV_IOCTL_GET_EVENTS, (int)command,
                   "driver registry events command matches protocol");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_ioctl_command_to_bit(DRV_IOCTL_GET_STATUS, &bit),
                   "driver registry maps status ioctl command");
    TEST_ASSERT_EQ((int)DRIVER_IOCTL_BIT_GET_STATUS, (int)bit,
                   "driver registry status bit matches descriptor");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_ioctl_bit_to_command(DRIVER_IOCTL_BIT_CLEAR_STATUS,
                                                &command),
                   "driver registry maps clear-status ioctl bit");
    TEST_ASSERT_EQ((int)DRV_IOCTL_CLEAR_STATUS, (int)command,
                   "driver registry clear-status command matches protocol");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl_bit_to_command(0, &command),
                   "driver registry rejects zero ioctl bit");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl_command_to_bit(0xffffffffU, &bit),
                   "driver registry rejects unknown ioctl command");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl_bit_to_command(DRIVER_IOCTL_BIT_GET_EVENTS,
                                                NULL),
                   "driver registry rejects NULL ioctl command output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_ioctl_command_to_bit(DRV_IOCTL_GET_EVENTS, NULL),
                   "driver registry rejects NULL ioctl bit output");

    uint32_t resource_type = 0;
    uint32_t resource_bit = 0;
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_resource_bit_to_type(DRV_RESOURCE_BIT_MMIO,
                                               &resource_type),
                   "driver registry maps MMIO resource bit");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_MMIO, (int)resource_type,
                   "driver registry MMIO resource type matches protocol");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_resource_bit_to_type(DRV_RESOURCE_BIT_IRQ,
                                               &resource_type),
                   "driver registry maps IRQ resource bit");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_IRQ, (int)resource_type,
                   "driver registry IRQ resource type matches protocol");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_resource_type_to_bit(DRV_RESOURCE_MMIO,
                                               &resource_bit),
                   "driver registry maps MMIO resource type");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_BIT_MMIO, (int)resource_bit,
                   "driver registry MMIO resource bit matches descriptor");
    TEST_ASSERT_EQ((int)KERN_OK,
                   driver_resource_type_to_bit(DRV_RESOURCE_IRQ,
                                               &resource_bit),
                   "driver registry maps IRQ resource type");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_BIT_IRQ, (int)resource_bit,
                   "driver registry IRQ resource bit matches descriptor");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_resource_bit_to_type(0, &resource_type),
                   "driver registry rejects zero resource bit");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_resource_type_to_bit(0xffffffffU, &resource_bit),
                   "driver registry rejects unknown resource type");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_resource_bit_to_type(DRV_RESOURCE_BIT_MMIO, NULL),
                   "driver registry rejects NULL resource type output");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM,
                   driver_resource_type_to_bit(DRV_RESOURCE_MMIO, NULL),
                   "driver registry rejects NULL resource bit output");

    TEST_ASSERT_EQ(0,
                   strcmp(driver_op_bit_name(DRIVER_OP_BIT_OPEN), "open"),
                   "driver registry names open op bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_op_bit_name(DRIVER_OP_BIT_IOCTL), "ioctl"),
                   "driver registry names ioctl op bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_op_bit_name(DRIVER_OP_BIT_ATTACH), "attach"),
                   "driver registry names attach op bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_op_bit_name(DRIVER_OP_BIT_DETACH), "detach"),
                   "driver registry names detach op bit");
    TEST_ASSERT_NULL((void *)driver_op_bit_name(0),
                     "driver registry rejects zero op bit name");
    TEST_ASSERT_NULL((void *)driver_op_bit_name(0xffffffffU),
                     "driver registry rejects unknown op bit name");

    TEST_ASSERT_EQ(0,
                   strcmp(driver_ioctl_bit_name(DRIVER_IOCTL_BIT_GET_EVENTS),
                          "events"),
                   "driver registry names events ioctl bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_ioctl_bit_name(DRIVER_IOCTL_BIT_GET_RESOURCES),
                          "resources"),
                   "driver registry names resources ioctl bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_ioctl_bit_name(DRIVER_IOCTL_BIT_GET_STATUS),
                          "status"),
                   "driver registry names status ioctl bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_ioctl_bit_name(DRIVER_IOCTL_BIT_CLEAR_STATUS),
                          "clear-status"),
                   "driver registry names clear-status ioctl bit");
    TEST_ASSERT_NULL((void *)driver_ioctl_bit_name(0),
                     "driver registry rejects zero ioctl bit name");
    TEST_ASSERT_NULL((void *)driver_ioctl_bit_name(0xffffffffU),
                     "driver registry rejects unknown ioctl bit name");

    TEST_ASSERT_EQ(0,
                   strcmp(driver_resource_bit_name(DRV_RESOURCE_BIT_MMIO),
                          "mmio"),
                   "driver registry names MMIO resource bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_resource_bit_name(DRV_RESOURCE_BIT_IRQ),
                          "irq"),
                   "driver registry names IRQ resource bit");
    TEST_ASSERT_NULL((void *)driver_resource_bit_name(0),
                     "driver registry rejects zero resource bit name");
    TEST_ASSERT_NULL((void *)driver_resource_bit_name(0xffffffffU),
                     "driver registry rejects unknown resource bit name");

    TEST_ASSERT_EQ(0, strcmp(driver_status_bit_name(DRV_STATUS_OPEN), "open"),
                   "driver registry names open status bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_status_bit_name(DRV_STATUS_MMIO_READY),
                          "mmio"),
                   "driver registry names MMIO status bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_status_bit_name(DRV_STATUS_IRQ_BOUND), "irq"),
                   "driver registry names IRQ status bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_status_bit_name(DRV_STATUS_IRQ_PENDING),
                          "pending"),
                   "driver registry names pending status bit");
    TEST_ASSERT_EQ(0,
                   strcmp(driver_status_bit_name(DRV_STATUS_ERROR), "error"),
                   "driver registry names error status bit");
    TEST_ASSERT_NULL((void *)driver_status_bit_name(0),
                     "driver registry rejects zero status bit name");
    TEST_ASSERT_NULL((void *)driver_status_bit_name(0xffffffffU),
                     "driver registry rejects unknown status bit name");
}

/*============================================================================
 * Test 15: root-created user-space UART server IPC
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

    driver_msg_init(&msg, DRV_OP_OPEN, 101);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server open send OK");
    TEST_ASSERT_EQ((int)DRV_OP_OPEN, (int)msg.opcode,
                   "UART server open keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server open status OK");

    driver_msg_init(&msg, DRV_OP_POLL, 102);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server poll send OK");
    TEST_ASSERT_EQ((int)DRV_OP_POLL, (int)msg.opcode,
                   "UART server poll keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server poll status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_WRITABLE) != 0,
                "UART server poll reports writable after open");

    driver_msg_init(&msg, DRV_OP_IOCTL, 103);
    msg.command = DRV_IOCTL_GET_EVENTS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server get-events ioctl send OK");
    TEST_ASSERT_EQ((int)DRV_OP_IOCTL, (int)msg.opcode,
                   "UART server ioctl keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server get-events ioctl status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_WRITABLE) != 0,
                "UART server ioctl reports writable after open");

    driver_msg_init(&msg, DRV_OP_READ, 104);
    msg.length = 4;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server read send OK");
    TEST_ASSERT_EQ((int)DRV_OP_READ, (int)msg.opcode,
                   "UART server read keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server read status OK");
    TEST_ASSERT_EQ(0, (int)msg.result,
                   "UART server empty read returns zero bytes");
    TEST_ASSERT_EQ(0, (int)msg.length,
                   "UART server empty read clears returned length");

    driver_msg_init(&msg, DRV_OP_WRITE, 105);
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

    driver_msg_init(&msg, DRV_OP_CLOSE, 106);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server close send OK");
    TEST_ASSERT_EQ((int)DRV_OP_CLOSE, (int)msg.opcode,
                   "UART server close keeps opcode");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server close status OK");

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
                                                     KERN_EP_MAX_PENDING,
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

    driver_msg_init(&msg, DRV_OP_ATTACH + 1U, 201);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server bad opcode send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects bad opcode");

    driver_msg_init(&msg, DRV_OP_CLOSE, 202);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server close-before-open send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "UART server rejects close before open");

    driver_msg_init(&msg, DRV_OP_WRITE, 203);
    msg.payload[0] = 'x';
    msg.length = 1;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server write-before-open send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "UART server rejects write before open");

    driver_msg_init(&msg, DRV_OP_READ, 204);
    msg.length = 1;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server read-before-open send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "UART server rejects read before open");

    driver_msg_init(&msg, DRV_OP_OPEN, 205);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server first open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server first open OK");

    driver_msg_init(&msg, DRV_OP_OPEN, 206);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server duplicate open send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)msg.status,
                   "UART server rejects duplicate open");

    driver_msg_init(&msg, DRV_OP_IOCTL, 207);
    msg.command = 0xffffffffU;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server unknown ioctl send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects unknown ioctl");

    driver_msg_init(&msg, DRV_OP_READ, 208);
    msg.length = DRV_PAYLOAD_MAX + 1U;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server oversized read send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects oversized read");

    driver_msg_init(&msg, DRV_OP_WRITE, 209);
    msg.length = DRV_PAYLOAD_MAX + 1U;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server oversized write send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)msg.status,
                   "UART server rejects oversized write");

    driver_msg_init(&msg, DRV_OP_IOCTL, 210);
    msg.command = DRV_IOCTL_GET_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server error status send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server error status query OK");
    TEST_ASSERT((msg.result & DRV_STATUS_ERROR) != 0,
                "UART server reports sticky error status");

    driver_msg_init(&msg, DRV_OP_IOCTL, 211);
    msg.command = DRV_IOCTL_CLEAR_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server clear status send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server clear status OK");

    driver_msg_init(&msg, DRV_OP_IOCTL, 212);
    msg.command = DRV_IOCTL_GET_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART server cleared status query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART server cleared status query OK");
    TEST_ASSERT((msg.result & DRV_STATUS_ERROR) == 0,
                "UART server clears sticky error status");

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
        driver_test_set_arg_pair(reg_client, reg_ns_cap, reg_uart_cap);
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
        driver_test_set_arg_pair(lookup_client, lookup_ns_cap, lookup_inbox_cap);
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
 * Test 17: UART driver lookup release restores caps
 *============================================================================*/

static void test_uart_driver_lookup_release_caps(void) {
    test_section("Test 17: UART driver lookup release restores caps");

    uint16_t cap_free_before = cap_free_count();
    ep_id_t ns_ep = endpoint_create("drv_rel_ns", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(ns_ep >= 0, "driver release name-server endpoint created");

    task_id_t ns_id = KERN_INVALID_ID;
    cap_id_t ns_service_cap = KERN_INVALID_ID;
    cap_id_t ns_root_cap = KERN_INVALID_ID;
    if (ns_ep >= 0) {
        ns_id = task_create_user("drv_rel_ns",
                                 driver_nameserver_release_task,
                                 NULL, 13, 1536);
        TEST_ASSERT(ns_id >= 0, "driver release name-server task created");
    }

    tcb_t *ns_tcb = task_get_tcb(ns_id);
    if (ns_tcb != NULL) {
        ns_service_cap = cap_create_for(ns_tcb,
                                        (void *)(uintptr_t)(ns_ep + 1),
                                        CAP_OBJ_ENDPOINT,
                                        CAP_READ | CAP_WRITE);
    }
    if (ns_ep >= 0) {
        ns_root_cap = cap_create((void *)(uintptr_t)(ns_ep + 1),
                                 CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    }
    TEST_ASSERT(ns_service_cap >= 0,
                "driver release name-server receives endpoint cap");
    TEST_ASSERT(ns_root_cap >= 0,
                "driver release root receives name-server cap");

    kern_err_t err = KERN_ERR_STATE;
    if (ns_id >= 0 && ns_service_cap >= 0 && ns_tcb != NULL &&
        ns_tcb->sp != NULL) {
        uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)ns_tcb->sp + 32U);
        *stacked_r0 = (uint32_t)ns_service_cap;
        err = task_start(ns_id);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver release name-server task started");

    ep_id_t service_ep = endpoint_create("drv_rel_svc", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(service_ep >= 0, "driver release service endpoint created");
    cap_id_t service_cap = KERN_INVALID_ID;
    if (service_ep >= 0) {
        service_cap = cap_create((void *)(uintptr_t)(service_ep + 1),
                                 CAP_OBJ_ENDPOINT,
                                 CAP_READ | CAP_WRITE | CAP_TRANSFER, 0);
    }
    TEST_ASSERT(service_cap >= 0, "driver release service cap created");

    if (ns_root_cap >= 0 && service_cap >= 0) {
        err = nameserver_register(ns_root_cap, "dev.uart0", service_cap,
                                  0x44520001U, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver release service registered");

    ep_id_t inbox_ep = endpoint_create("drv_rel_inbox", KERN_EP_MSG_SIZE, 2);
    TEST_ASSERT(inbox_ep >= 0, "driver release inbox endpoint created");
    cap_id_t inbox_cap = KERN_INVALID_ID;
    if (inbox_ep >= 0) {
        inbox_cap = cap_create((void *)(uintptr_t)(inbox_ep + 1),
                               CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    }
    TEST_ASSERT(inbox_cap >= 0, "driver release inbox cap created");

    uint16_t cap_free_before_lookups = cap_free_count();
    for (uint32_t i = 0; i < 4U; i++) {
        cap_id_t lookup_cap = KERN_INVALID_ID;
        err = driver_lookup_uart(ns_root_cap, inbox_cap, &lookup_cap, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver release lookup returns service cap");
        TEST_ASSERT(lookup_cap > 0, "driver release lookup cap valid");
        TEST_ASSERT(cap_free_count() < cap_free_before_lookups,
                    "driver release lookup consumes temporary cap");
        TEST_ASSERT_EQ((int)KERN_OK,
                       driver_release_service(inbox_cap, lookup_cap),
                       "driver release service ACK OK");
        if (lookup_cap > 0) {
            cap_delete(lookup_cap);
            test_pass("driver release deletes lookup cap");
        }
        for (uint32_t wait = 0;
             wait < 4U && cap_free_count() != cap_free_before_lookups;
             wait++) {
            task_delay(1);
        }
        TEST_ASSERT(cap_free_count() >= cap_free_before_lookups,
                    "driver release restores temporary cap");
    }

    void *retval = NULL;
    if (ns_id >= 0) {
        err = task_join(ns_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver release name-server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver release name-server retval OK");
    }

    if (ns_id >= 0 && task_get_state(ns_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(ns_id);
    }
    if (inbox_ep >= 0) {
        (void)endpoint_delete(inbox_ep);
    }
    if (service_ep >= 0) {
        (void)endpoint_delete(service_ep);
    }
    if (ns_ep >= 0) {
        (void)endpoint_delete(ns_ep);
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver release cleanup restored caps");
}

/*============================================================================
 * Test 18: UART driver resource cap attach
 *============================================================================*/

static void test_uart_driver_resource_attach(void) {
    test_section("Test 18: UART driver resource attach");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_srv",
                                        uart_server_attach_query_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART attach server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART attach server endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART attach server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000000UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach creates MMIO cap");
    TEST_ASSERT(mmio_cap > 0, "driver attach MMIO cap valid");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_attach_client",
                                     driver_attach_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver attach client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    cap_id_t client_mmio_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_mmio_cap = cap_copy_to(NULL, mmio_cap, client,
                                      CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "driver attach client receives endpoint cap");
    TEST_ASSERT(client_mmio_cap >= 0,
                "driver attach client receives MMIO cap");

    if (client_id >= 0 && client_ep_cap >= 0 && client_mmio_cap >= 0) {
        driver_test_set_arg_pair(client_id, client_ep_cap, client_mmio_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver attach client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver attach client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver attach client retval OK");
    }

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_IOCTL, 301);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver attach resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) != 0,
                "driver attach records MMIO resource");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_IRQ) == 0,
                "driver attach leaves IRQ resource clear");

    void *mmio_obj = NULL;
    if (mmio_cap > 0) {
        mmio_obj = cap_resolve(mmio_cap, CAP_OBJ_MMIO, CAP_READ);
    }
    TEST_ASSERT(mmio_obj != NULL,
                "driver attach MMIO object resolves");
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver attach server holds MMIO cap");
    }

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver attach server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver attach server retval OK");
    }
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver attach server released MMIO cap");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver attach deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver attach root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver attach cleanup restored caps");
}

/*============================================================================
 * Test 18: UART driver resource detach releases held cap
 *============================================================================*/

static void test_uart_driver_resource_detach(void) {
    test_section("Test 18: UART driver resource detach");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_detach",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_detach_srv",
                                        uart_server_attach_detach_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART detach server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_detach_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART detach server endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART detach server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000040UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach creates MMIO cap");
    TEST_ASSERT(mmio_cap > 0, "driver detach MMIO cap valid");

    void *mmio_obj = NULL;
    if (mmio_cap > 0) {
        mmio_obj = cap_resolve(mmio_cap, CAP_OBJ_MMIO, CAP_READ);
    }
    TEST_ASSERT(mmio_obj != NULL,
                "driver detach MMIO object resolves");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 302);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver detach attach accepted");
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver detach server holds MMIO cap");
    }

    driver_msg_init(&msg, DRV_OP_IOCTL, 303);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver detach resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) != 0,
                "driver detach sees MMIO before detach");

    driver_msg_init(&msg, DRV_OP_DETACH, 304);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver detach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_MMIO, (int)msg.result,
                   "driver detach result is MMIO");
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver detach releases MMIO cap immediately");
    }

    driver_msg_init(&msg, DRV_OP_DETACH, 305);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver duplicate detach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "driver duplicate detach rejected");

    driver_msg_init(&msg, DRV_OP_IOCTL, 306);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver detach resource-query after detach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver detach resource-query after detach status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) == 0,
                "driver detach clears MMIO resource bit");

    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 307);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver reattach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver reattach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_MMIO, (int)msg.result,
                   "driver reattach result is MMIO");
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver reattach server holds MMIO cap again");
    }

    driver_msg_init(&msg, DRV_OP_IOCTL, 308);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver reattach resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "driver reattach resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) != 0,
                "driver reattach restores MMIO resource bit");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver detach server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver detach server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver reattach server released MMIO cap on exit");
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver detach deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver detach root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver detach cleanup restored caps");
}

/*============================================================================
 * Test 18: UART driver resource attach rejects missing cap
 *============================================================================*/

static void test_uart_driver_resource_attach_missing_cap(void) {
    test_section("Test 18: UART driver resource attach missing cap");

    root_bootstrap_init();

    uint16_t cap_free_after_root = 0;
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach_bad",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach missing-cap creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }
    cap_free_after_root = cap_free_count();

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_bad",
                                        uart_server_attach_error_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART missing-cap server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_bad_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART missing-cap endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART missing-cap server task started");

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 300);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART missing-cap attach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)msg.status,
                   "UART missing-cap attach rejected");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART missing-cap server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART missing-cap server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver missing-cap root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_after_root + 2, (int)cap_free_count(),
                   "driver missing-cap cleanup restored caps");
}

/*============================================================================
 * Test 19: UART driver resource attach requires transfer right
 *============================================================================*/

static void test_uart_driver_resource_attach_requires_transfer(void) {
    test_section("Test 19: UART driver resource attach transfer right");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach_perm",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach transfer creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_perm",
                                        uart_server_attach_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART transfer server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_perm_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART transfer endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART transfer server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000100UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver transfer creates MMIO cap");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_attach_no_xfer",
                                     driver_attach_no_transfer_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver no-transfer client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    cap_id_t client_mmio_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_mmio_cap = cap_copy_to(NULL, mmio_cap, client, CAP_READ);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "no-transfer client receives endpoint cap");
    TEST_ASSERT(client_mmio_cap >= 0,
                "no-transfer client receives read-only MMIO cap");

    if (client_id >= 0 && client_ep_cap >= 0 && client_mmio_cap >= 0) {
        driver_test_set_arg_pair(client_id, client_ep_cap, client_mmio_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "no-transfer client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "no-transfer client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "no-transfer attach rejected");
    }

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1500);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "no-transfer server joined after timeout");
        TEST_ASSERT_EQ((int)KERN_ERR_TIMEOUT, (int)(intptr_t)retval,
                       "no-transfer server saw no attach request");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver no-transfer deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver no-transfer root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver no-transfer cleanup restored caps");
}

/*============================================================================
 * Test 20: UART driver resource attach rejects unknown resource type
 *============================================================================*/

static void test_uart_driver_resource_attach_bad_type(void) {
    test_section("Test 20: UART driver resource attach bad type");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach_type",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach bad-type creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_type",
                                        uart_server_attach_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART bad-type server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_type_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART bad-type endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART bad-type server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000200UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver bad-type creates MMIO cap");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_attach_bad_type",
                                     driver_attach_bad_type_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver bad-type client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    cap_id_t client_mmio_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_mmio_cap = cap_copy_to(NULL, mmio_cap, client,
                                      CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "bad-type client receives endpoint cap");
    TEST_ASSERT(client_mmio_cap >= 0,
                "bad-type client receives MMIO cap");

    if (client_id >= 0 && client_ep_cap >= 0 && client_mmio_cap >= 0) {
        driver_test_set_arg_pair(client_id, client_ep_cap, client_mmio_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "bad-type client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "bad-type client joined");
        TEST_ASSERT_EQ((int)KERN_ERR_PARAM, (int)(intptr_t)retval,
                       "bad-type attach rejected");
    }

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "bad-type server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "bad-type server retval OK");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver bad-type deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver bad-type root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver bad-type cleanup restored caps");
}

/*============================================================================
 * Test 21: UART driver resource attach rejects duplicate resource
 *============================================================================*/

static void test_uart_driver_resource_attach_duplicate(void) {
    test_section("Test 21: UART driver resource attach duplicate");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach_dup",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver attach duplicate creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_dup",
                                        uart_server_attach_query_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART duplicate server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_dup_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART duplicate endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART duplicate server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000300UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver duplicate creates MMIO cap");
    TEST_ASSERT(mmio_cap > 0, "driver duplicate MMIO cap valid");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 400);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART duplicate first attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART duplicate first attach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_MMIO, (int)msg.result,
                   "UART duplicate first attach result is MMIO");

    driver_msg_init(&msg, DRV_OP_ATTACH, 401);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART duplicate second attach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)msg.status,
                   "UART duplicate second attach rejected busy");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART duplicate server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART duplicate server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver duplicate deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver duplicate root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver duplicate cleanup restored caps");
}

/*============================================================================
 * Test 22: UART driver IRQ resource attach records IRQ state
 *============================================================================*/

static void test_uart_driver_irq_resource_attach(void) {
    test_section("Test 22: UART driver IRQ resource attach");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_attach_irq",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver IRQ attach creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_attach_irq",
                                        uart_server_attach_query_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ attach server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_attach_irq_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ attach endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ attach server task started");

    cap_id_t irq_resource_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(47,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_resource_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver IRQ attach creates transferable IRQ cap");
    TEST_ASSERT(irq_resource_cap > 0,
                "driver IRQ attach resource cap valid");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = irq_resource_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 500);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ attach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_IRQ, (int)msg.result,
                   "UART IRQ attach result is IRQ");

    driver_msg_init(&msg, DRV_OP_IOCTL, 501);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_IRQ) != 0,
                "UART IRQ attach records IRQ resource");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) == 0,
                "UART IRQ attach leaves MMIO resource clear");

    void *irq_obj = NULL;
    if (irq_resource_cap > 0) {
        irq_obj = cap_resolve(irq_resource_cap, CAP_OBJ_IRQ, CAP_READ);
    }
    TEST_ASSERT(irq_obj != NULL,
                "driver IRQ attach object resolves");
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "driver IRQ attach server holds IRQ cap");
    }

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART IRQ attach server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART IRQ attach server retval OK");
    }
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "driver IRQ attach server released IRQ cap");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_resource_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_resource_cap),
                       "driver IRQ attach deletes IRQ cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver IRQ attach root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver IRQ attach cleanup restored caps");
}

/*============================================================================
 * Test 23: UART driver IRQ detach clears notification binding
 *============================================================================*/

static void test_uart_driver_irq_resource_detach(void) {
    test_section("Test 23: UART driver IRQ resource detach");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_irq_detach",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver IRQ detach creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_irq_detach",
                                        uart_server_irq_detach_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_irq_detach_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach server task started");

    cap_id_t irq_resource_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(DRIVER_TEST_IRQ_DETACH,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_resource_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver IRQ detach creates transferable IRQ cap");
    TEST_ASSERT(irq_resource_cap > 0,
                "driver IRQ detach resource cap valid");

    void *irq_obj = NULL;
    if (irq_resource_cap > 0) {
        irq_obj = cap_resolve(irq_resource_cap, CAP_OBJ_IRQ, CAP_READ);
    }
    TEST_ASSERT(irq_obj != NULL,
                "driver IRQ detach object resolves");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = irq_resource_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 506);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ detach attach accepted");
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "UART IRQ detach server holds IRQ cap");
    }

    driver_msg_init(&msg, DRV_OP_IOCTL, 507);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ detach resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_IRQ) != 0,
                "UART IRQ detach sees IRQ before detach");

    driver_msg_init(&msg, DRV_OP_DETACH, 508);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ detach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_IRQ, (int)msg.result,
                   "UART IRQ detach result is IRQ");
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "UART IRQ detach releases IRQ cap immediately");
    }

    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST,
                   (int)irq_notify(DRIVER_TEST_IRQ_DETACH),
                   "UART IRQ detach clears endpoint binding");

    driver_msg_init(&msg, DRV_OP_DETACH, 509);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ duplicate detach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_STATE, (int)msg.status,
                   "UART IRQ duplicate detach rejected");

    driver_msg_init(&msg, DRV_OP_IOCTL, 510);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ detach resource-query after detach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ detach resource-query after detach status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_IRQ) == 0,
                "UART IRQ detach clears IRQ resource bit");

    xfer.src_cap = irq_resource_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 511);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ reattach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ reattach accepted");
    TEST_ASSERT_EQ((int)DRV_RESOURCE_IRQ, (int)msg.result,
                   "UART IRQ reattach result is IRQ");
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(2, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "UART IRQ reattach server holds IRQ cap again");
    }

    TEST_ASSERT_EQ((int)KERN_OK,
                   (int)irq_notify(DRIVER_TEST_IRQ_DETACH),
                   "UART IRQ reattach restores endpoint binding");

    driver_msg_init(&msg, DRV_OP_IOCTL, 512);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ reattach resource-query send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ reattach resource-query status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_IRQ) != 0,
                "UART IRQ reattach restores IRQ resource bit");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART IRQ detach server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART IRQ detach server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "UART IRQ reattach server released IRQ cap on exit");
    }
    if (irq_resource_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_resource_cap),
                       "driver IRQ detach deletes IRQ cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver IRQ detach root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver IRQ detach cleanup restored caps");
}

/*============================================================================
 * Test 23: UART driver resource attach validates cap object type
 *============================================================================*/

static void test_uart_driver_resource_attach_type_mismatch(void) {
    test_section("Test 23: UART driver resource attach cap type mismatch");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_type_mis",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver type-mismatch creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_type_mismatch",
                                        uart_server_attach_query_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART type-mismatch server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_type_mismatch_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART type-mismatch endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART type-mismatch server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    cap_id_t irq_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000300UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver type-mismatch creates MMIO cap");
    if (err == KERN_OK) {
        err = kirq_create_cap(48,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver type-mismatch creates IRQ cap");

    drv_msg_t msg;
    ipc_cap_xfer_t xfer;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 510);
    msg.command = DRV_RESOURCE_MMIO;
    xfer.src_cap = irq_cap;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rejects IRQ cap as MMIO send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)msg.status,
                   "UART rejects IRQ cap as MMIO");

    driver_msg_init(&msg, DRV_OP_ATTACH, 511);
    msg.command = DRV_RESOURCE_IRQ;
    xfer.src_cap = mmio_cap;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rejects MMIO cap as IRQ send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)msg.status,
                   "UART rejects MMIO cap as IRQ");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART type-mismatch server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART type-mismatch server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_cap),
                       "driver type-mismatch deletes IRQ cap");
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver type-mismatch deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver type-mismatch root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver type-mismatch cleanup restored caps");
}

/*============================================================================
 * Test 24: UART driver resource attach requires resource write rights
 *============================================================================*/

static void test_uart_driver_resource_attach_rights(void) {
    test_section("Test 24: UART driver resource attach rights");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_rights",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver rights creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_rights",
                                        uart_server_attach_error_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rights server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_rights_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rights endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rights server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000340UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver rights creates MMIO cap");

    drv_msg_t msg;
    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 520);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART rights attach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)msg.status,
                   "UART rights rejects read-only MMIO cap");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART rights server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART rights server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver rights deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver rights root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver rights cleanup restored caps");
}

/*============================================================================
 * Test 25: UART driver resource policy requires MMIO before open
 *============================================================================*/

static void test_uart_driver_open_requires_mmio_resource(void) {
    test_section("Test 23: UART driver open requires MMIO resource");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_open_res",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver open-resource creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_open_res",
                                        uart_server_attach_query_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART open-resource server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_open_res_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART open-resource endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART open-resource server task started");

    cap_id_t irq_resource_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(48,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_resource_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver open-resource creates IRQ cap");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = irq_resource_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 600);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART open-resource IRQ attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART open-resource IRQ attach accepted");

    driver_msg_init(&msg, DRV_OP_OPEN, 601);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART open-resource open send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_CAP, (int)msg.status,
                   "UART open-resource rejects IRQ-only open");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART open-resource server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART open-resource server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_resource_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_resource_cap),
                       "driver open-resource deletes IRQ cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver open-resource root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver open-resource cleanup restored caps");
}

/*============================================================================
 * Test 24: UART driver MMIO resource enables open/write path
 *============================================================================*/

static void test_uart_driver_mmio_resource_allows_rw(void) {
    test_section("Test 24: UART driver MMIO resource allows RW");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_mmio_rw",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver MMIO-RW creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_mmio_rw",
                                        uart_server_attach_rw_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_mmio_rw_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000600UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver MMIO-RW creates MMIO cap");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 700);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART MMIO-RW attach accepted");

    driver_msg_init(&msg, DRV_OP_OPEN, 701);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART MMIO-RW open accepted after MMIO");

    driver_msg_init(&msg, DRV_OP_DETACH, 702);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW active detach send OK");
    TEST_ASSERT_EQ((int)KERN_ERR_BUSY, (int)msg.status,
                   "UART MMIO-RW rejects detach while open");

    driver_msg_init(&msg, DRV_OP_WRITE, 703);
    msg.payload[0] = 'O';
    msg.payload[1] = 'K';
    msg.length = 2;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW write send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART MMIO-RW write accepted");
    TEST_ASSERT_EQ(2, (int)msg.result,
                   "UART MMIO-RW write returns byte count");

    driver_msg_init(&msg, DRV_OP_CLOSE, 704);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART MMIO-RW close send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART MMIO-RW close accepted");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART MMIO-RW server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART MMIO-RW server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver MMIO-RW deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver MMIO-RW root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver MMIO-RW cleanup restored caps");
}

/*============================================================================
 * Test 25: UART driver close clears resource-managed events
 *============================================================================*/

static void test_uart_driver_close_clears_events(void) {
    test_section("Test 25: UART driver close clears events");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_events",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver events creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_events",
                                        uart_server_attach_poll_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_events_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000700UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver events creates MMIO cap");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 800);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events attach accepted");

    driver_msg_init(&msg, DRV_OP_OPEN, 801);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events open accepted");

    driver_msg_init(&msg, DRV_OP_POLL, 802);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events poll-open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events poll-open status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_WRITABLE) != 0,
                "UART events writable while open");

    driver_msg_init(&msg, DRV_OP_CLOSE, 803);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events close send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events close accepted");

    driver_msg_init(&msg, DRV_OP_POLL, 804);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events poll-closed send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events poll-closed status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_WRITABLE) == 0,
                "UART events writable cleared after close");

    driver_msg_init(&msg, DRV_OP_IOCTL, 805);
    msg.command = DRV_IOCTL_GET_RESOURCES;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART events resource-query after close send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART events resource-query after close status OK");
    TEST_ASSERT((msg.result & DRV_RESOURCE_BIT_MMIO) != 0,
                "UART events MMIO resource remains after close");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART events server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART events server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver events deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver events root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver events cleanup restored caps");
}

/*============================================================================
 * Test 26: UART driver status ioctl reports resource/open state
 *============================================================================*/

static void test_uart_driver_status_ioctl(void) {
    test_section("Test 26: UART driver status ioctl");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_status",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver status creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_status",
                                        uart_server_attach_poll_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_status_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000980UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver status creates MMIO cap");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 850);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status attach accepted");

    driver_msg_init(&msg, DRV_OP_IOCTL, 851);
    msg.command = DRV_IOCTL_GET_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status query after attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status query after attach status OK");
    TEST_ASSERT((msg.result & DRV_STATUS_MMIO_READY) != 0,
                "UART status reports MMIO ready");
    TEST_ASSERT((msg.result & DRV_STATUS_OPEN) == 0,
                "UART status closed after attach");

    driver_msg_init(&msg, DRV_OP_OPEN, 852);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status open accepted");

    driver_msg_init(&msg, DRV_OP_IOCTL, 853);
    msg.command = DRV_IOCTL_GET_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status query open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status query open status OK");
    TEST_ASSERT((msg.result & DRV_STATUS_OPEN) != 0,
                "UART status reports open");

    driver_msg_init(&msg, DRV_OP_CLOSE, 854);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status close send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status close accepted");

    driver_msg_init(&msg, DRV_OP_IOCTL, 855);
    msg.command = DRV_IOCTL_GET_STATUS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART status query closed send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART status query closed status OK");
    TEST_ASSERT((msg.result & DRV_STATUS_OPEN) == 0,
                "UART status open cleared after close");
    TEST_ASSERT((msg.result & DRV_STATUS_MMIO_READY) != 0,
                "UART status MMIO remains ready after close");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART status server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART status server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver status deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver status root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver status cleanup restored caps");
}

/*============================================================================
 * Test 27: user client drives resource-managed UART session
 *============================================================================*/

static void test_uart_driver_user_resource_session(void) {
    test_section("Test 26: user driver resource session");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_user_sess",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver user-session creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_user_sess",
                                        uart_server_attach_user_session_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-session server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_user_sess_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-session endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-session server task started");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000800UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver user-session creates MMIO cap");

    void *mmio_obj = NULL;
    if (mmio_cap > 0) {
        mmio_obj = cap_resolve(mmio_cap, CAP_OBJ_MMIO, CAP_READ);
    }
    TEST_ASSERT(mmio_obj != NULL,
                "driver user-session MMIO object resolves");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_user_session",
                                     driver_resource_session_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver user-session client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    cap_id_t client_mmio_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_mmio_cap = cap_copy_to(NULL, mmio_cap, client,
                                      CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "user-session client receives endpoint cap");
    TEST_ASSERT(client_mmio_cap >= 0,
                "user-session client receives MMIO cap");

    if (client_id >= 0 && client_ep_cap >= 0 && client_mmio_cap >= 0) {
        driver_test_set_arg_pair(client_id, client_ep_cap, client_mmio_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-session client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-session client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver user-session client retval OK");
    }
    if (mmio_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(mmio_obj, CAP_OBJ_MMIO),
                       "driver user-session detach releases MMIO cap");
    }

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-session server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver user-session server retval OK");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver user-session deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver user-session root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver user-session cleanup restored caps");
}

/*============================================================================
 * Test 27: user client cannot open with IRQ-only resource
 *============================================================================*/

static void test_uart_driver_user_irq_only_open_rejected(void) {
    test_section("Test 27: user driver IRQ-only open rejected");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_user_irq",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver user-IRQ creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_user_irq",
                                        uart_server_user_irq_detach_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-IRQ server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_user_irq_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-IRQ endpoint created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user-IRQ server task started");

    cap_id_t irq_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(49,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver user-IRQ creates transferable IRQ cap");

    void *irq_obj = NULL;
    if (irq_cap > 0) {
        irq_obj = cap_resolve(irq_cap, CAP_OBJ_IRQ, CAP_READ);
    }
    TEST_ASSERT(irq_obj != NULL,
                "driver user-IRQ object resolves");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_user_irq",
                                     driver_irq_only_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver user-IRQ client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    cap_id_t client_irq_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
        client_irq_cap = cap_copy_to(NULL, irq_cap, client,
                                     CAP_READ | CAP_WRITE | CAP_TRANSFER);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "user-IRQ client receives endpoint cap");
    TEST_ASSERT(client_irq_cap >= 0,
                "user-IRQ client receives resource cap");

    if (client_id >= 0 && client_ep_cap >= 0 && client_irq_cap >= 0) {
        driver_test_set_arg_pair(client_id, client_ep_cap, client_irq_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-IRQ client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-IRQ client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver user-IRQ open rejected");
    }
    if (irq_obj != NULL) {
        TEST_ASSERT_EQ(1, (int)cap_object_refcount(irq_obj, CAP_OBJ_IRQ),
                       "driver user-IRQ detach releases IRQ cap");
    }
    TEST_ASSERT_EQ((int)KERN_ERR_NOEXIST, (int)irq_notify(49),
                   "driver user-IRQ detach clears binding");

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver user-IRQ server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver user-IRQ server retval OK");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_cap),
                       "driver user-IRQ deletes IRQ cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver user-IRQ root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver user-IRQ cleanup restored caps");
}

/*============================================================================
 * Test 28: IRQ notification wakes UART driver server
 *============================================================================*/

static void test_uart_driver_irq_notification_event(void) {
    test_section("Test 28: UART driver IRQ notification event");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_irq_evt",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver IRQ-event creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_irq_evt",
                                        uart_server_irq_notify_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_irq_evt_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event endpoint created");

    cap_id_t irq_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(45,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event cap created");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000900UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event MMIO cap created");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event server task started");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = irq_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 899);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event server bound IRQ cap");

    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 898);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event MMIO attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event server accepted MMIO cap");

    if (err == KERN_OK) {
        err = irq_notify(45);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event notification sent");

    driver_msg_init(&msg, DRV_OP_IOCTL, 900);
    msg.command = DRV_IOCTL_GET_EVENTS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event get-events send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event get-events status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_READABLE) != 0,
                "UART IRQ-event reports readable");

    driver_msg_init(&msg, DRV_OP_OPEN, 901);
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event open send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event open status OK");

    driver_msg_init(&msg, DRV_OP_READ, 902);
    msg.length = 1;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event read send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event read status OK");
    TEST_ASSERT_EQ(1, (int)msg.result,
                   "UART IRQ-event read consumes one event");

    driver_msg_init(&msg, DRV_OP_IOCTL, 903);
    msg.command = DRV_IOCTL_GET_EVENTS;
    if (err == KERN_OK) {
        err = endpoint_send(server_ep, &msg, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART IRQ-event get-events after read send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART IRQ-event get-events after read status OK");
    TEST_ASSERT((msg.result & DRV_EVENT_READABLE) == 0,
                "UART IRQ-event readable cleared by read");

    void *retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "UART IRQ-event server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "UART IRQ-event server retval OK");
    }

    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_cap),
                       "driver IRQ-event deletes IRQ cap");
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver IRQ-event deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver IRQ-event root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver IRQ-event cleanup restored caps");
}

/*============================================================================
 * Test 29: user client consumes UART driver IRQ event
 *============================================================================*/

static void test_uart_driver_user_irq_event_client(void) {
    test_section("Test 29: user driver IRQ event client");

    root_bootstrap_init();

    uint16_t cap_free_before = cap_free_count();
    task_id_t root_id = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("root_drv_user_irq_evt",
                                           driver_root_dummy_task,
                                           NULL, 12, 512, &root_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "driver user IRQ-event creates root");
    if (err != KERN_OK || root_id < 0) {
        return;
    }

    task_id_t server_id = KERN_INVALID_ID;
    cap_id_t server_task_cap = KERN_INVALID_ID;
    err = root_bootstrap_create_service("uart_user_irq_evt",
                                        uart_server_irq_user_task,
                                        NULL, 13, 768,
                                        &server_id, &server_task_cap);
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event server task created");

    ep_id_t server_ep = KERN_INVALID_ID;
    cap_id_t root_ep_cap = KERN_INVALID_ID;
    cap_id_t server_ep_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = root_bootstrap_create_service_endpoint(server_task_cap,
                                                     "uart_user_irq_evt_ep",
                                                     KERN_EP_MSG_SIZE,
                                                     2,
                                                     &server_ep,
                                                     &root_ep_cap,
                                                     &server_ep_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event endpoint created");

    cap_id_t irq_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kirq_create_cap(46,
                              CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER,
                              &irq_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event cap created");

    cap_id_t mmio_cap = KERN_INVALID_ID;
    if (err == KERN_OK) {
        err = kmmio_create_cap(0x40000940UL, 16, 4,
                               CAP_READ | CAP_WRITE | CAP_MANAGE |
                                   CAP_TRANSFER,
                               &mmio_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event MMIO cap created");

    task_id_t client_id = KERN_INVALID_ID;
    if (err == KERN_OK) {
        client_id = task_create_user("drv_irq_event_client",
                                     driver_irq_event_client_task,
                                     NULL, 14, 1024);
    }
    TEST_ASSERT(client_id >= 0, "driver IRQ-event user client created");

    cap_id_t client_ep_cap = KERN_INVALID_ID;
    tcb_t *client = task_get_tcb(client_id);
    if (client != NULL) {
        client_ep_cap = cap_create_for(client,
                                       (void *)(uintptr_t)(server_ep + 1),
                                       CAP_OBJ_ENDPOINT,
                                       CAP_READ | CAP_WRITE);
    }
    TEST_ASSERT(client_ep_cap >= 0,
                "driver IRQ-event user client receives endpoint cap");

    if (err == KERN_OK) {
        err = root_bootstrap_start_service(server_task_cap);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event server task started");

    ipc_cap_xfer_t xfer;
    xfer.src_cap = irq_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    drv_msg_t msg;
    driver_msg_init(&msg, DRV_OP_ATTACH, 999);
    msg.command = DRV_RESOURCE_IRQ;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART user IRQ-event server bound IRQ cap");

    xfer.src_cap = mmio_cap;
    xfer.rights = CAP_READ | CAP_WRITE;
    xfer.flags = IPC_CAP_COPY;

    driver_msg_init(&msg, DRV_OP_ATTACH, 998);
    msg.command = DRV_RESOURCE_MMIO;
    if (err == KERN_OK) {
        err = endpoint_send_caps(server_ep, &msg, &xfer, 1, 1000);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event MMIO attach send OK");
    TEST_ASSERT_EQ((int)KERN_OK, (int)msg.status,
                   "UART user IRQ-event server accepted MMIO cap");

    if (err == KERN_OK) {
        err = irq_notify(46);
    }
    TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                   "UART user IRQ-event notification sent");

    if (client_id >= 0 && client_ep_cap >= 0) {
        driver_test_set_arg(client_id, (uint32_t)client_ep_cap);
        err = task_start(client_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver IRQ-event user client started");
    }

    void *retval = NULL;
    if (client_id >= 0) {
        err = task_join(client_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver IRQ-event user client joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver IRQ-event user client retval OK");
    }

    retval = NULL;
    if (server_id >= 0) {
        err = task_join(server_id, &retval, 1000);
        TEST_ASSERT_EQ((int)KERN_OK, (int)err,
                       "driver IRQ-event user server joined");
        TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                       "driver IRQ-event user server retval OK");
    }

    if (client_id >= 0 &&
        task_get_state(client_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(client_id);
    }
    if (server_id >= 0 &&
        task_get_state(server_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(server_id);
    }
    if (irq_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kirq_delete_cap(irq_cap),
                       "driver IRQ-event user deletes IRQ cap");
    }
    if (mmio_cap > 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)kmmio_delete_cap(mmio_cap),
                       "driver IRQ-event user deletes MMIO cap");
    }
    if (root_id >= 0) {
        TEST_ASSERT_EQ((int)KERN_OK, (int)task_delete(root_id),
                       "driver IRQ-event user root deleted");
    }
    TEST_ASSERT_EQ((int)cap_free_before, (int)cap_free_count(),
                   "driver IRQ-event user cleanup restored caps");
}

/*============================================================================
 * Module registration
 *============================================================================*/

static void test_driver_module(void) {
    driver_test_reset_pairs();
    test_device_alloc_basic();
    test_device_alloc_dup();
    test_device_find();
    test_device_free();
    test_device_alloc_null();
    test_devnull_exists();
    test_devnull_rw();
    test_uart_device();
    /* Phase D:以下测试走 sys_open 访问内核 devfs (现在返回 NOSYS)。
     * 设备文件访问已迁移到 fs_server (见 test_fs_devfs)。
     * 内核 devfs 的 sys_open 路径测试注释掉。 */
    /* test_uart_devfs();       — /dev/uart0 via sys_open */
    /* test_led_devices();      — /dev/led* via sys_open */
    /* test_device_probe_remove_diag(); — /dev/probe0 via sys_open */
    /* test_device_event_ioctl(); — /dev/evdev via sys_open */
    test_driver_server_protocol_layout();
    test_driver_registry_descriptor();
    test_uart_user_server_ipc();
    test_uart_user_server_protocol_errors();
    test_uart_driver_nameserver_lookup();
    test_uart_driver_lookup_release_caps();
    test_uart_driver_resource_attach();
    test_uart_driver_resource_detach();
    test_uart_driver_resource_attach_missing_cap();
    test_uart_driver_resource_attach_requires_transfer();
    test_uart_driver_resource_attach_bad_type();
    test_uart_driver_resource_attach_duplicate();
    test_uart_driver_irq_resource_attach();
    test_uart_driver_irq_resource_detach();
    test_uart_driver_resource_attach_type_mismatch();
    test_uart_driver_resource_attach_rights();
    test_uart_driver_open_requires_mmio_resource();
    test_uart_driver_mmio_resource_allows_rw();
    test_uart_driver_close_clears_events();
    test_uart_driver_status_ioctl();
    test_uart_driver_user_resource_session();
    test_uart_driver_user_irq_only_open_rejected();
    test_uart_driver_irq_notification_event();
    test_uart_driver_user_irq_event_client();
}

TEST_MODULE_REGISTER(driver, test_driver_module);

#endif /* DRIVER_ENABLE && TEST_ENABLE */
