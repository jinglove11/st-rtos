/**
 * @file stats.c
 * @brief 内核统计模块实现
 */

#include "stats.h"

#if KERN_TASK_STATS

#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 全局统计实例
 *============================================================================*/

static kern_stats_t cpu_stats[SMP_MAX_CPUS];
static kern_stats_t stats_snapshot;
static uint32_t last_stat_tick;
static uint32_t subsys_counters[SMP_MAX_CPUS][STATS_SUBSYS_MAX]
                                [STATS_COUNTER_MAX];
static volatile uint32_t stats_event_epoch = 1U;
static uint32_t stats_cpu_event_epoch[SMP_MAX_CPUS];

static uint32_t stats_cpu_id(void) {
    uint32_t cpu = hal_get_cpu_id();
    return (cpu < SMP_MAX_CPUS) ? cpu : 0U;
}

static void stats_event_epoch_sync(uint32_t cpu) {
    uint32_t epoch = __atomic_load_n(&stats_event_epoch, __ATOMIC_ACQUIRE);
    if (stats_cpu_event_epoch[cpu] != epoch) {
        memset(subsys_counters[cpu], 0, sizeof(subsys_counters[cpu]));
        stats_cpu_event_epoch[cpu] = epoch;
    }
}

/*============================================================================
 * 外部引用
 *============================================================================*/

extern uint64_t task_get_used_bitmap(void);
/*============================================================================
 * 初始化
 *============================================================================*/

void stats_init(void) {
    last_stat_tick = 0;
    memset(cpu_stats, 0, sizeof(cpu_stats));
    memset(&stats_snapshot, 0, sizeof(stats_snapshot));
    memset(subsys_counters, 0, sizeof(subsys_counters));
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        stats_cpu_event_epoch[cpu] = stats_event_epoch;
    }
}

/*============================================================================
 * 上下文切换统计
 *============================================================================*/

void stats_task_switch(tcb_t *prev, tcb_t *next) {
    uint32_t cpu = stats_cpu_id();
    if (prev && prev->id >= 0) {
        prev->ctx_switch_count++;
    }
    if (next && next->id >= 0) {
        next->ctx_switch_count++;
    }
    cpu_stats[cpu].total_ctx_switches++;
}

/*============================================================================
 * Tick 更新 (CPU 使用率计算)
 *============================================================================*/

void stats_tick_update(void) {
    uint32_t cpu = stats_cpu_id();
    tcb_t *current = sched_get_current();
    if (current) {
        __atomic_fetch_add(&current->total_ticks, 1U, __ATOMIC_RELAXED);
    }

    if (cpu == 0U) {
        cpu_stats[0].uptime_ticks = sched_get_tick_count();
    }
}

void stats_deferred_update(void) {
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
                uint32_t ticks = __atomic_exchange_n(&tcb->total_ticks, 0U,
                                                      __ATOMIC_ACQ_REL);
                tcb->cpu_usage = ticks * 10000U / elapsed;
            }
        }
    }

    for (uint32_t idle_cpu = 0; idle_cpu < SMP_MAX_CPUS; idle_cpu++) {
        tcb_t *idle = task_get_idle_cpu(idle_cpu);
        if (idle) {
            uint32_t ticks = __atomic_exchange_n(&idle->total_ticks, 0U,
                                                  __ATOMIC_ACQ_REL);
            idle->cpu_usage = ticks * 10000U / elapsed;
        }
    }
}

/*============================================================================
 * IRQ 统计
 *============================================================================*/

void stats_record_irq(uint32_t latency_ticks) {
    uint32_t cpu = stats_cpu_id();
    uint32_t crit = hal_irq_save();
    cpu_stats[cpu].total_irqs++;
    if (latency_ticks > cpu_stats[cpu].irq_latency_max) {
        cpu_stats[cpu].irq_latency_max = latency_ticks;
    }
    hal_irq_restore(crit);
}

/*============================================================================
 * Syscall 统计
 *============================================================================*/

void stats_record_syscall(uint8_t task_id) {
    uint32_t cpu = stats_cpu_id();
    uint32_t crit = hal_irq_save();
    cpu_stats[cpu].total_syscalls++;
    if (task_id < KERNEL_MAX_TASKS) {
        tcb_t *tcb = task_get_tcb((task_id_t)task_id);
        if (tcb) {
            tcb->ctx_switch_count++;
        }
    }
    hal_irq_restore(crit);
}

/*============================================================================
 * Fault 统计
 *============================================================================*/

void stats_record_fault(void) {
    uint32_t cpu = stats_cpu_id();
    uint32_t crit = hal_irq_save();
    cpu_stats[cpu].fault_count++;
    hal_irq_restore(crit);
}

kern_err_t stats_record_event(uint8_t subsystem, uint8_t counter) {
    if (subsystem >= STATS_SUBSYS_MAX || counter >= STATS_COUNTER_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t cpu = stats_cpu_id();
    uint32_t crit = hal_irq_save();
    stats_event_epoch_sync(cpu);
    subsys_counters[cpu][subsystem][counter]++;
    hal_irq_restore(crit);
    return KERN_OK;
}

uint32_t stats_get_event_count(uint8_t subsystem, uint8_t counter) {
    if (subsystem >= STATS_SUBSYS_MAX || counter >= STATS_COUNTER_MAX) {
        return 0;
    }

    uint32_t epoch = __atomic_load_n(&stats_event_epoch, __ATOMIC_ACQUIRE);
    uint32_t total = 0U;
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if (stats_cpu_event_epoch[cpu] == epoch) {
            total += subsys_counters[cpu][subsystem][counter];
        }
    }
    return total;
}

void stats_clear_events(void) {
    uint32_t epoch = __atomic_add_fetch(&stats_event_epoch, 1U,
                                         __ATOMIC_ACQ_REL);
    uint32_t cpu = stats_cpu_id();
    uint32_t crit = hal_irq_save();
    memset(subsys_counters[cpu], 0, sizeof(subsys_counters[cpu]));
    stats_cpu_event_epoch[cpu] = epoch;
    hal_irq_restore(crit);
}

/*============================================================================
 * 查询接口
 *============================================================================*/

const kern_stats_t *stats_get_kern_stats(void) {
    kern_stats_t aggregate = {0};
    aggregate.uptime_ticks = sched_get_tick_count();
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        aggregate.total_ctx_switches += cpu_stats[cpu].total_ctx_switches;
        aggregate.total_irqs += cpu_stats[cpu].total_irqs;
        aggregate.fault_count += cpu_stats[cpu].fault_count;
        aggregate.total_syscalls += cpu_stats[cpu].total_syscalls;
        if (cpu_stats[cpu].irq_latency_max > aggregate.irq_latency_max) {
            aggregate.irq_latency_max = cpu_stats[cpu].irq_latency_max;
        }
    }
    stats_snapshot = aggregate;
    return &stats_snapshot;
}

uint32_t stats_get_uptime(void) {
    return sched_get_tick_count();
}

uint32_t stats_get_ctx_switches(void) {
    return stats_get_kern_stats()->total_ctx_switches;
}

uint32_t stats_get_irq_latency_max(void) {
    return stats_get_kern_stats()->irq_latency_max;
}

#endif /* KERN_TASK_STATS */
