/**
 * @file stats.c
 * @brief 内核统计模块实现
 */

#include "stats.h"

#if KERN_TASK_STATS

#include "scheduler.h"
#include "task.h"

/*============================================================================
 * 全局统计实例
 *============================================================================*/

static kern_stats_t kern_stats;
static uint32_t last_stat_tick;
static uint32_t subsys_counters[STATS_SUBSYS_MAX][STATS_COUNTER_MAX];

/*============================================================================
 * 外部引用
 *============================================================================*/

extern uint64_t task_get_used_bitmap(void);
/*============================================================================
 * 初始化
 *============================================================================*/

void stats_init(void) {
    last_stat_tick = 0;
    kern_stats = (kern_stats_t){0};
    for (int i = 0; i < STATS_SUBSYS_MAX; i++) {
        for (int j = 0; j < STATS_COUNTER_MAX; j++) {
            subsys_counters[i][j] = 0;
        }
    }
}

/*============================================================================
 * 上下文切换统计
 *============================================================================*/

void stats_task_switch(tcb_t *prev, tcb_t *next) {
    if (prev && prev->id >= 0) {
        prev->ctx_switch_count++;
    }
    if (next && next->id >= 0) {
        next->ctx_switch_count++;
    }
    kern_stats.total_ctx_switches++;
}

/*============================================================================
 * Tick 更新 (CPU 使用率计算)
 *============================================================================*/

void stats_tick_update(void) {
    tcb_t *current = sched_get_current();
    if (current) {
        current->total_ticks++;
    }

    uint32_t tick = sched_get_tick_count();
    uint32_t elapsed = tick - last_stat_tick;
    if (elapsed < 1000) {
        return;
    }
    last_stat_tick = tick;

    uint64_t bitmap = task_get_used_bitmap();
    for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
        if (bitmap & (1ULL << i)) {
            tcb_t *tcb = task_get_tcb((task_id_t)i);
            if (tcb) {
                tcb->cpu_usage = tcb->total_ticks * 10000 / elapsed;
                tcb->total_ticks = 0;
            }
        }
    }

    tcb_t *idle = task_get_idle();
    if (idle) {
        idle->cpu_usage = idle->total_ticks * 10000 / elapsed;
        idle->total_ticks = 0;
    }

    kern_stats.uptime_ticks = tick;
}

/*============================================================================
 * IRQ 统计
 *============================================================================*/

void stats_record_irq(uint32_t latency_ticks) {
    kern_stats.total_irqs++;
    if (latency_ticks > kern_stats.irq_latency_max) {
        kern_stats.irq_latency_max = latency_ticks;
    }
}

/*============================================================================
 * Syscall 统计
 *============================================================================*/

void stats_record_syscall(uint8_t task_id) {
    kern_stats.total_syscalls++;
    if (task_id < KERNEL_MAX_TASKS) {
        tcb_t *tcb = task_get_tcb((task_id_t)task_id);
        if (tcb) {
            tcb->ctx_switch_count++;
        }
    }
}

/*============================================================================
 * Fault 统计
 *============================================================================*/

void stats_record_fault(void) {
    kern_stats.fault_count++;
}

kern_err_t stats_record_event(uint8_t subsystem, uint8_t counter) {
    if (subsystem >= STATS_SUBSYS_MAX || counter >= STATS_COUNTER_MAX) {
        return KERN_ERR_PARAM;
    }

    subsys_counters[subsystem][counter]++;
    return KERN_OK;
}

uint32_t stats_get_event_count(uint8_t subsystem, uint8_t counter) {
    if (subsystem >= STATS_SUBSYS_MAX || counter >= STATS_COUNTER_MAX) {
        return 0;
    }

    return subsys_counters[subsystem][counter];
}

void stats_clear_events(void) {
    for (int i = 0; i < STATS_SUBSYS_MAX; i++) {
        for (int j = 0; j < STATS_COUNTER_MAX; j++) {
            subsys_counters[i][j] = 0;
        }
    }
}

/*============================================================================
 * 查询接口
 *============================================================================*/

const kern_stats_t *stats_get_kern_stats(void) {
    return &kern_stats;
}

uint32_t stats_get_uptime(void) {
    return kern_stats.uptime_ticks;
}

uint32_t stats_get_ctx_switches(void) {
    return kern_stats.total_ctx_switches;
}

uint32_t stats_get_irq_latency_max(void) {
    return kern_stats.irq_latency_max;
}

#endif /* KERN_TASK_STATS */
