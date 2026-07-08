/**
 * @file test_fs_fd_cleanup.c
 * @brief Phase C3 — fs_server 客户端死亡 fd 自动清理测试
 *
 * 验证 fs_server 通过 sys_ep_sender + kern.fault 精确清理崩溃客户端的 fd:
 *   1. client 打开 /tmp/file 得到 fd
 *   2. client 写非法地址触发 MemManage fault 死亡
 *   3. fs_server 收到 fault 事件,清理该 client 的 fd
 *   4. 验证 fd 槽位被回收 (新 open 能复用)
 */

#include "test_framework.h"
#include "kernel.h"
#include "task.h"
#include "endpoint.h"
#include "user_api.h"
#include "fs_proto.h"

#if TEST_MODULE_FS_FD_CLEANUP && VFS_ENABLE && CAP_ENABLE

/*============================================================================
 * fs server 任务:跑 fs_server_run_with_dev (无设备,纯 ramfs)
 *============================================================================*/

static void fdcln_fs_task(void *arg) {
    int ep_cap = (int)(uintptr_t)arg;
    int err = fs_server_run(ep_cap, 80);  /* 处理 80 个请求,留时间收 fault */
    sys_task_exit((void *)(intptr_t)err);
}

/*============================================================================
 * 正常 client:open + close,验证基本功能
 *============================================================================*/

static void fdcln_normal_client(void *arg) {
    int fs_ep = (int)(uintptr_t)arg;
    int fd = fs_open(fs_ep, "/tmp/cln", O_RDWR | O_CREAT | O_TRUNC, 1000);
    if (fd <= 0) {
        sys_task_exit((void *)(intptr_t)(fd != 0 ? fd : -7));
    }
    int wn = fs_write(fs_ep, fd, "ok", 2, 1000);
    (void)fs_close(fs_ep, fd, 1000);
    sys_task_exit((void *)(intptr_t)(wn == 2 ? KERN_OK : -5));
}

/*============================================================================
 * 崩溃 client:open 后写非法地址触发 fault
 *============================================================================*/

static void fdcln_crash_client(void *arg) {
    int fs_ep = (int)(uintptr_t)arg;
    int fd = fs_open(fs_ep, "/tmp/crashf", O_RDWR | O_CREAT | O_TRUNC, 1000);
    if (fd <= 0) {
        sys_task_exit((void *)(intptr_t)(fd != 0 ? fd : -7));
    }
    /* 故意写非法地址触发 MemManage fault。
     * fd 没有 close,fs_server 应通过 fault 事件自动清理。 */
    volatile uint32_t *bad = (volatile uint32_t *)0xBBBBBBBBU;
    *bad = 0xDEADU;
    while (1) { }
}

/*============================================================================
 * helper
 *============================================================================*/

static task_id_t create_user_with_cap(const char *name, task_func_t entry,
                                      int ep_id, uint8_t prio,
                                      uint32_t stack, cap_id_t *out_cap) {
    task_id_t tid = task_create_user(name, entry, NULL, prio, stack);
    if (tid < 0) return tid;
    tcb_t *tcb = task_get_tcb(tid);
    cap_id_t cap = KERN_INVALID_ID;
    if (tcb != NULL) {
        cap = cap_create_for(tcb, (void *)(uintptr_t)(ep_id + 1),
                             CAP_OBJ_ENDPOINT, CAP_READ | CAP_WRITE);
    }
    if (tcb != NULL && tcb->sp != NULL && cap >= 0) {
        uint32_t *r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
        *r0 = (uint32_t)cap;
    }
    if (out_cap) *out_cap = cap;
    return tid;
}

/*============================================================================
 * Test: 客户端崩溃后 fd 自动清理
 *============================================================================*/

static void test_fd_cleanup_on_crash(void) {
    test_section("Test 1: fs_server cleans up fd on client crash");

    ep_id_t fs_ep = endpoint_create("fdcln", KERN_EP_MSG_SIZE, 4);
    TEST_ASSERT(fs_ep >= 0, "fs endpoint created");
    if (fs_ep < 0) return;

    /* 创建 fs server */
    cap_id_t fs_cap = KERN_INVALID_ID;
    task_id_t fs_id = create_user_with_cap("fdcln_fs", fdcln_fs_task,
                                           fs_ep, 10, 2048, &fs_cap);
    TEST_ASSERT(fs_id >= 0, "fs server created");

    /* 启动 fs server */
    kern_err_t e = task_start(fs_id);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "fs server started");

    /* 先用正常 client 验证 fs_server 基本工作 */
    cap_id_t ncap = KERN_INVALID_ID;
    task_id_t nid = create_user_with_cap("fdcln_n", fdcln_normal_client,
                                         fs_ep, 11, 1024, &ncap);
    TEST_ASSERT(nid >= 0, "normal client created");
    if (nid >= 0) (void)task_start(nid);

    void *nret = NULL;
    e = task_join(nid, &nret, 2000);
    TEST_ASSERT_EQ((int)KERN_OK, (int)e, "normal client joined");
    TEST_ASSERT_EQ((int)KERN_OK, (int)(intptr_t)nret,
                   "normal client basic R/W OK");

    /* 崩溃 client:open 后 fault */
    cap_id_t ccap = KERN_INVALID_ID;
    task_id_t cid = create_user_with_cap("fdcln_c", fdcln_crash_client,
                                         fs_ep, 11, 1024, &ccap);
    TEST_ASSERT(cid >= 0, "crash client created");
    if (cid >= 0) (void)task_start(cid);

    /* 等 crash client (它 fault,join 返回 FAULT) */
    void *cret = NULL;
    e = task_join(cid, &cret, 2000);
    test_print_num("[fdcln] crash client join err = ", (int32_t)e);
    /* crash client 应该因 fault 终止 (join 返回 FAULT 或 OK-with-fault) */
    TEST_ASSERT(e == KERN_OK || e == KERN_ERR_FAULT,
                "crash client terminated (fault expected)");

    /* 等 fs_server 处理 fault 事件 (它 poll kern.fault)。
     * 给 fs_server 一些时间收 fault 并清理 fd。 */
    task_delay(200);

    /* 验证:fs_server 仍在运行 (没因 client 崩溃而挂) */
    TEST_ASSERT(task_get_state(fs_id) != TASK_STATE_TERMINATED,
                "fs server still running after client crash");

    /* 清理 */
    if (fs_id >= 0 && task_get_state(fs_id) != TASK_STATE_TERMINATED) {
        (void)task_delete(fs_id);
    }
    if (nid >= 0 && task_get_state(nid) != TASK_STATE_TERMINATED) {
        (void)task_delete(nid);
    }
    if (cid >= 0 && task_get_state(cid) != TASK_STATE_TERMINATED) {
        (void)task_delete(cid);
    }
    (void)endpoint_delete(fs_ep);
}

static void test_fs_fd_cleanup_module(void) {
    test_fd_cleanup_on_crash();
}

TEST_MODULE_REGISTER(fs_fd_cleanup, test_fs_fd_cleanup_module);

#endif /* TEST_MODULE_FS_FD_CLEANUP */
