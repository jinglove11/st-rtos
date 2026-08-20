/**
 * @file main.c
 * @brief 系统入口 — 根据 IMAGE_PROFILE 走不同启动路径
 *
 * test profile (TEST_ENABLE=1): test_runner_start (跑测试套件 + shell)
 * release profile (TEST_ENABLE=0): system_init + root/init (无测试)
 */

#include "kernel_config.h"

#if TEST_ENABLE
/* test/dev profile: 跑测试框架 */
#include "test_framework.h"

int main(void) {
    test_runner_start();
    while (1);
    return 0;
}

#else
/* release profile: 直接初始化内核 + root/init */
#include "kernel.h"
#include "task.h"
#include "system_init.h"
#include "root_bootstrap.h"
#include "shell.h"

#if INIT_PROCESS
extern void init_main(void *arg);
#endif

int main(void) {
    system_init(NULL);

#if INIT_PROCESS
    task_id_t init_tid = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create("init", init_main, NULL,
                                            3, 1024, &init_tid);
    if (err == KERN_OK && init_tid >= 0) {
        (void)root_bootstrap_start();
    }
#if FAULT_ENDPOINT && SUPERVISOR
    (void)root_bootstrap_spawn_supervisor();
#endif
#endif

#if DEV_PROFILE && SHELL_ENABLE
    /* dev profile:无测试框架的 release 路径 + 特权 shell(诊断用)。
     * shell 与 init 并存:shell 是内核任务,不是 init 的子进程。 */
    shell_start();
#endif

    kern_start();
    while (1);
    return 0;
}

#endif /* TEST_ENABLE */
