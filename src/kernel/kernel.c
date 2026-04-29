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

extern uint32_t task_used_bitmap;
extern tcb_t task_pool[];

void kern_init(void) {
    hal_cpu_init();
    mem_init();
    mempool_init();
    task_init();
    sched_init();
    ipc_init();
    timer_init();
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
    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (task_used_bitmap & (1U << i)) {
            count++;
        }
    }
    return count;
}

void kern_foreach_task(void (*callback)(tcb_t *tcb, void *arg), void *arg) {
    if (callback == NULL) return;
    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (task_used_bitmap & (1U << i)) {
            callback(&task_pool[i], arg);
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

void kern_syscall_handler(uint32_t svc_num, uint32_t *args) {
    (void)svc_num;
    (void)args;
}
