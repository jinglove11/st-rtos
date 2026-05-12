/**
 * @file kernel.c
 * @brief 内核主实现
 */

#include "kernel.h"
#include "kernel_config.h"
#include "task.h"
#include "scheduler.h"
#include "ipc.h"
#include "mem.h"
#include "mempool.h"
#include "hal.h"
#include "uart.h"
#include "board_config.h"
#include "timer.h"
#include "irq.h"
#include "bh.h"
#include "stats.h"
#include "capability.h"
#include "vfs/vfs.h"
#if DRIVER_ENABLE
#include "device.h"
#endif

void kern_init(void) {
    hal_cpu_init();
    mem_init();
    mempool_init();
    task_init();
    sched_init();
    ipc_init();
    irq_init();
    bh_init();
    timer_init();
#if KERN_TASK_STATS
    stats_init();
#endif
#if CAP_ENABLE
    cap_init();
#endif
#if VFS_ENABLE
    vfs_init();
#endif
#if DRIVER_ENABLE
    device_init();
#endif
#if KERN_WATCHDOG_ENABLE
    hal_watchdog_init(KERN_WATCHDOG_TIMEOUT);
#endif
}

void kern_start(void) {
    // 获取空闲任务并加入就绪队列
    // 空闲任务作为后备，确保就绪队列永不为空
    tcb_t *idle = task_get_idle();
    idle->state = TASK_STATE_READY;
    idle->time_slice = KERN_DEFAULT_TIME_SLICE;
    idle->time_slice_reload = KERN_DEFAULT_TIME_SLICE;

    // 将空闲任务加入就绪队列 (最低优先级)
    // 这样当没有其他任务时，空闲任务会被调度
    sched_add_ready(idle);

    // 启动定时器服务任务
    timer_service_start();
    bh_service_start();

    // 启动调度器
    sched_start();

    // 不应该到达这里
    while (1);
}

void kern_get_version(uint8_t *major, uint8_t *minor, uint8_t *patch) {
    if (major) *major = KERN_VERSION_MAJOR;
    if (minor) *minor = KERN_VERSION_MINOR;
    if (patch) *patch = KERN_VERSION_PATCH;
}

const char *kern_get_name(void) {
    return KERN_NAME;
}

uint32_t kern_get_tick(void) {
    extern uint32_t sched_get_tick_count(void);
    return sched_get_tick_count();
}

void kern_panic(const char *msg) {
    hal_irq_disable();
#if KERN_DEBUG_ENABLE
    hal_debug_puts("\n!!! KERNEL PANIC !!!\n");
    if (msg) {
        hal_debug_puts(msg);
        hal_debug_puts("\n");
    }
#else
    (void)msg;  // 避免未使用参数警告
#endif
    while (1) {
        hal_enter_lowpower();
    }
}

void kern_tick_handler(void) {
    sched_tick_handler();
}

#if KERN_TASK_STATS

uint32_t kern_get_task_count(void) {
    uint32_t count = 0;
    uint32_t bitmap = task_get_used_bitmap();

    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (bitmap & (1U << i)) {
            count++;
        }
    }
    return count;
}

void kern_foreach_task(void (*callback)(tcb_t *tcb, void *arg), void *arg) {
    if (callback == NULL) return;
    uint32_t bitmap = task_get_used_bitmap();

    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (bitmap & (1U << i)) {
            tcb_t *tcb = task_get_tcb((task_id_t)i);
            if (tcb) {
                callback(tcb, arg);
            }
        }
    }
    callback(task_get_idle(), arg);
}

#endif

#if KERN_ASSERT_ENABLE

void kern_assert_failed(const char *file, int line, const char *func, const char *expr) {
    (void)file;
    (void)line;
    (void)func;
    hal_debug_puts("\nASSERT: ");
    hal_debug_puts(expr);
    hal_debug_puts("\n");
    kern_panic("assertion failed");
}

#endif

#if !SYSCALL_ENABLE
kern_err_t kern_syscall_handler(uint32_t svc_num, uint32_t a1, uint32_t a2,
                                 uint32_t a3, uint32_t a4, uint32_t a5,
                                 uint32_t a6) {
    (void)svc_num; (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6;
    return KERN_ERR;
}
#endif
