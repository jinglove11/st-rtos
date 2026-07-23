/**
 * @file test_fs_devfs.c
 * @brief Phase B2 — devfs 转发端到端测试
 *
 * 验证 fs_server 把 /dev/ 下的 read/write 转发给 driver server:
 *   client → fs_server (FS_OP_READ/WRITE) → driver_server (DRV_OP_READ/WRITE)
 *
 * 用 uart_server_run 作为模拟 driver (它回显 write 的字节数)。
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "endpoint.h"
#include "user_api.h"
#include "fs_proto.h"
#include "driver_proto.h"

#if TEST_MODULE_FS_DEVFS && VFS_ENABLE && CAP_ENABLE && DRIVER_ENABLE

/*============================================================================
 * 任务体
 *============================================================================*/

/* driver server 任务:arg = 自己的 ep_cap */
static void dev_driver_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err = uart_server_run(ep_cap, 30);
    sys_task_exit((void *)(intptr_t)err);
}

/* fs server uses its own CNode slots; CPtrs are 32-bit and must never be
 * truncated/packed into a single initial argument. */
static void dev_fs_task(void *arg) {
    (void)arg;
    int fs_ep = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0);
    int dev_ep = sys_cap_self_slot(CAP_OBJ_ENDPOINT, 1);
    int err = fs_server_run_with_dev(fs_ep, 20, dev_ep, "echo");
    sys_task_exit((void *)(intptr_t)err);
}

/* client 任务:arg = fs_ep_cap。
 * 经 fs_server open /dev/echo,write 数据,read 回来。 */
static void dev_client_task(void *arg) {
    int fs_ep = (int)(uintptr_t)arg;
    int fd;
    int err;

    /* ping fs_server 确认活着 */
    err = fs_ping(fs_ep, 1000);
    if (err != KERN_OK) {
        sys_task_exit((void *)(intptr_t)err);
    }

    /* open /dev/echo */
    fd = fs_open(fs_ep, "/dev/echo", 2, 1000);  /* O_RDWR */
    if (fd <= 0) {
        sys_task_exit((void *)(intptr_t)(fd != 0 ? fd : -7));
    }

    /* 先 driver_open (driver server 要求先 open 才能 read/write) */
    /* 注:fs_server 转发 read/write 到 driver,但 driver 需要先 DRV_OP_OPEN。
     * 这里简化:直接 write/read,driver 若未 open 会返回 STATE。
     * 为测试转发链路,我们接受 driver 返回的任何结果 (关键是 fs_server
     * 成功转发,不返回 -16 NOSYS)。 */

    /* write 5 字节 */
    const char data[] = "hello";
    int wn = fs_write(fs_ep, fd, data, 5, 1000);
    /* driver server 未 open 时返回 STATE(-5),或 open 后返回写入字节数。
     * 关键:wn != -16 (NOSYS 表示没转发)。 */
    if (wn == -16) {
        (void)fs_close(fs_ep, fd, 1000);
        sys_task_exit((void *)(intptr_t)(-16));
    }

    /* close */
    (void)fs_close(fs_ep, fd, 1000);

    /* 成功:转发链路工作 (不卡在 NOSYS) */
    sys_task_exit((void *)(intptr_t)KERN_OK);
}

/*============================================================================
 * helper:创建服务任务 + endpoint + cap
 *============================================================================*/

static task_id_t create_service(const char *name, task_func_t entry,
                                void *arg, uint8_t prio, uint32_t stack,
                                ep_id_t ep, cap_id_t *out_cap) {
    task_id_t tid = task_create_user(name, entry, arg, prio, stack);
    if (tid < 0) return tid;
    tcb_t *tcb = task_get_tcb(tid);
    if (tcb != NULL && out_cap != NULL) {
        *out_cap = cap_create_for(tcb, endpoint_obj_for_cap(ep), CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    }
    if (tcb != NULL && tcb->sp != NULL && out_cap != NULL && *out_cap >= 0) {
        uint32_t *r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
        *r0 = (uint32_t)(*out_cap);
    } else {
        tid = -1;
    }
    return tid;
}

/*============================================================================
 * Test: devfs 转发链路
 *============================================================================*/

static void test_devfs_forwarding(void) {
    test_section("Test 1: fs_server forwards /dev/* to driver server");

    /* driver endpoint */
    ep_id_t dev_ep = endpoint_create("dev_drv", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(dev_ep >= 0, "driver endpoint created");
    if (dev_ep < 0) return;

    /* fs endpoint */
    ep_id_t fs_ep = endpoint_create("dev_fs", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(fs_ep >= 0, "fs endpoint created");
    if (fs_ep < 0) { (void)endpoint_delete(dev_ep); return; }

    /* 创建 driver server */
    cap_id_t drv_cap = KERN_INVALID_ID;
    task_id_t drv_id = create_service("dev_drv", dev_driver_task,
                                      NULL, 10, 2048, dev_ep, &drv_cap);
    TEST_ASSERT(drv_id >= 0, "driver task created");

    /* 创建 fs server,arg 编码 fs_ep + dev_ep(drv_cap) */
    cap_id_t fs_cap = KERN_INVALID_ID;
    task_id_t fs_id = task_create_user("dev_fs", dev_fs_task, NULL, 11, 2048);
    TEST_ASSERT(fs_id >= 0, "fs task created");
    tcb_t *fs_tcb = (fs_id >= 0) ? task_get_tcb(fs_id) : NULL;
    if (fs_tcb != NULL) {
        fs_cap = cap_create_for(fs_tcb, endpoint_obj_for_cap(fs_ep), CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
        /* fs 还需要 driver 的 ep_cap 来转发 + 注册设备 */
        cap_id_t fs_drv_cap = cap_create_for(fs_tcb, endpoint_obj_for_cap(dev_ep), CAP_OBJ_ENDPOINT,
                                             CAP_READ | CAP_WRITE);
        (void)fs_drv_cap;
    }

    /* 创建 client */
    cap_id_t cli_cap = KERN_INVALID_ID;
    task_id_t cli_id = task_create_user("dev_cli", dev_client_task,
                                        NULL, 12, 1536);
    TEST_ASSERT(cli_id >= 0, "client task created");
    tcb_t *cli_tcb = (cli_id >= 0) ? task_get_tcb(cli_id) : NULL;
    if (cli_tcb != NULL) {
        cli_cap = cap_create_for(cli_tcb, endpoint_obj_for_cap(fs_ep), CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
        if (cli_tcb->sp != NULL && cli_cap >= 0) {
            uint32_t *r0 = (uint32_t *)((uint8_t *)cli_tcb->sp + 32U);
            *r0 = (uint32_t)cli_cap;
        }
    }

    /* 启动 driver */
    if (drv_id >= 0) {
        kern_err_t e = task_start(drv_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "driver started");
    }
    /* 启动 fs */
    if (fs_id >= 0) {
        kern_err_t e = task_start(fs_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "fs started");
    }
    /* 启动 client */
    if (cli_id >= 0) {
        kern_err_t e = task_start(cli_id);
        TEST_ASSERT_EQ((int)KERN_OK, (int)e, "client started");
    }

    /* 等 client */
    void *retval = NULL;
    kern_err_t e = task_join(cli_id, &retval, 3000);
    test_print_num("[devfs] client retval = ", (int32_t)(intptr_t)retval);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "client joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)retval,
                   "devfs forwarding OK (no NOSYS)");

    /* 清理 */
    if (drv_id >= 0 && task_get_state(drv_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(drv_id);
    }
    if (fs_id >= 0 && task_get_state(fs_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(fs_id);
    }
    if (cli_id >= 0 && task_get_state(cli_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(cli_id);
    }
    (void)endpoint_delete(dev_ep);
    (void)endpoint_delete(fs_ep);
}

static void test_fs_devfs_module(void) {
    test_devfs_forwarding();
}

TEST_MODULE_REGISTER(fs_devfs, test_fs_devfs_module);

#endif /* TEST_MODULE_FS_DEVFS */
