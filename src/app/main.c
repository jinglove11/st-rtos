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
#include "root_bootstrap.h"

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
#endif

    kern_start();
    while (1);
    return 0;
}

#endif /* TEST_ENABLE */
