/**
 * @file scheduler.h
 * @brief 调度器接口
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "kernel_types.h"

/*============================================================================
 * 调度器接口
 *============================================================================*/

/**
 * @brief 初始化调度器
 */
void sched_init(void);

/**
 * @brief 启动调度器 (开始运行第一个任务)
 */
void sched_start(void) __attribute__((noreturn));

/**
 * @brief 执行调度 (选择下一任务)
 */
void sched_yield(void);

/**
 * @brief 将任务加入就绪队列
 * @param tcb 任务控制块
 */
void sched_add_ready(tcb_t *tcb);

/**
 * @brief 将任务从就绪队列移除
 * @param tcb 任务控制块
 */
void sched_remove_ready(tcb_t *tcb);

/**
 * @brief 阻塞当前任务
 * @param reason 阻塞原因
 * @param obj 阻塞对象
 * @param timeout 超时 (ticks), 0 表示无限等待
 * @return 操作结果
 */
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout);

/**
 * @brief 唤醒任务
 * @param tcb 任务控制块
 * @param result 唤醒结果
 */
void sched_wakeup(tcb_t *tcb, kern_err_t result);

/**
 * @brief 获取当前任务
 * @return 当前任务 TCB
 */
tcb_t *sched_get_current(void);

/**
 * @brief 获取最高优先级就绪任务
 * @return 任务 TCB
 */
tcb_t *sched_get_highest_ready(void);

/**
 * @brief 检查是否需要调度
 * @return 非零表示需要调度
 */
int sched_need_switch(void);

/**
 * @brief 时钟滴答处理
 */
void sched_tick_handler(void);

/**
 * @brief 任务时间片用完
 */
void sched_time_slice_expired(void);

/*============================================================================
 * 调度器统计接口
 *============================================================================*/

#if KERN_TASK_STATS

/**
 * @brief 获取任务 CPU 使用率
 * @param tcb 任务控制块
 * @return CPU 使用率 (万分比)
 */
uint32_t sched_get_cpu_usage(tcb_t *tcb);

/**
 * @brief 更新 CPU 使用率统计
 */
void sched_update_stats(void);

#endif

#endif // SCHEDULER_H
