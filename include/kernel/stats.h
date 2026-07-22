/**
 * @file stats.h
 * @brief 内核统计模块 — CPU 使用率、IRQ 延迟、syscall 计数
 */

#ifndef STATS_H
#define STATS_H

#include "kernel_config.h"

#if KERN_TASK_STATS

#include <stdint.h>
#include "kernel_types.h"

/*============================================================================
 * 全局内核统计
 *============================================================================*/

typedef struct {
    uint32_t uptime_ticks;
    uint32_t total_ctx_switches;
    uint32_t total_irqs;
    uint32_t irq_latency_max;
    uint32_t fault_count;
    uint32_t total_syscalls;
} kern_stats_t;

typedef enum {
    STATS_SUBSYS_TIMER = 0,
    STATS_SUBSYS_IRQ,
    STATS_SUBSYS_BH,
    STATS_SUBSYS_DEV,
    STATS_SUBSYS_MEM,
    STATS_SUBSYS_IPC,
    STATS_SUBSYS_CAP,
    STATS_SUBSYS_VFS,
    STATS_SUBSYS_MAX
} stats_subsys_t;

typedef enum {
    STATS_COUNTER_OK = 0,
    STATS_COUNTER_ERROR,
    STATS_COUNTER_QUEUE_FULL,
    STATS_COUNTER_TIMEOUT,
    STATS_COUNTER_DELETE,
    STATS_COUNTER_CANCEL,
    STATS_COUNTER_BUSY,
    STATS_COUNTER_NOEXIST,
    STATS_COUNTER_MAX
} stats_counter_t;

/*============================================================================
 * API
 *============================================================================*/

void stats_init(void);

/**
 * @brief 上下文切换时更新统计
 * @param prev 被切换出的任务 (可为 NULL)
 * @param next 被切换入的任务
 */
void stats_task_switch(tcb_t *prev, tcb_t *next);

/**
 * @brief 每 tick 更新 CPU 使用率 (替代 sched_update_stats)
 */
void stats_tick_update(void);

/** @brief Thread-context CPU-usage aggregation (never called from SysTick). */
void stats_deferred_update(void);

/** @brief 记录一次 IRQ (含延迟) */
void stats_record_irq(uint32_t latency_ticks);

/** @brief 记录一次 syscall */
void stats_record_syscall(uint8_t task_id);

/** @brief 记录一次 fault */
void stats_record_fault(void);

/** @brief 记录一次子系统事件 */
kern_err_t stats_record_event(uint8_t subsystem, uint8_t counter);

/** @brief 读取子系统事件计数 */
uint32_t stats_get_event_count(uint8_t subsystem, uint8_t counter);

/** @brief 清空子系统事件计数 */
void stats_clear_events(void);

/** @brief 获取全局统计 */
const kern_stats_t *stats_get_kern_stats(void);

/** @brief 获取系统运行 ticks */
uint32_t stats_get_uptime(void);

/** @brief 获取总上下文切换次数 */
uint32_t stats_get_ctx_switches(void);

/** @brief 获取最大 IRQ 延迟 (ticks) */
uint32_t stats_get_irq_latency_max(void);

#endif /* KERN_TASK_STATS */
#endif /* STATS_H */
