/**
 * @file task.h
 * @brief 任务管理接口
 */

#ifndef TASK_H
#define TASK_H

#include "kernel_types.h"

/*============================================================================
 * 任务控制
 *============================================================================*/

/**
 * @brief 初始化任务模块
 */
void task_init(void);

/**
 * @brief 创建任务
 * @param name 任务名称
 * @param entry 任务入口函数
 * @param arg 任务参数
 * @param priority 优先级 (0 最高)
 * @param stack_size 栈大小 (字节), 0 使用默认值
 * @return 任务 ID, 失败返回 KERN_INVALID_ID
 */
task_id_t task_create(const char   *name,
                      task_func_t  entry,
                      void        *arg,
                      uint8_t      priority,
                      uint32_t     stack_size);

/**
 * @brief 启动任务
 * @param task_id 任务 ID
 * @return 操作结果
 */
kern_err_t task_start(task_id_t task_id);

/**
 * @brief 终止当前任务
 * @param retval 返回值
 */
void task_exit(void *retval) __attribute__((noreturn));

/**
 * @brief 挂起任务
 * @param task_id 任务 ID
 * @return 操作结果
 */
kern_err_t task_suspend(task_id_t task_id);

/**
 * @brief 恢复任务
 * @param task_id 任务 ID
 * @return 操作结果
 */
kern_err_t task_resume(task_id_t task_id);

/**
 * @brief 删除任务
 * @param task_id 任务 ID
 * @return 操作结果
 */
kern_err_t task_delete(task_id_t task_id);

/**
 * @brief 获取当前任务 ID
 * @return 任务 ID
 */
task_id_t task_self(void);

/**
 * @brief 主动让出 CPU
 * @return 操作结果
 */
kern_err_t task_yield(void);

/**
 * @brief 获取任务 TCB
 * @param task_id 任务 ID
 * @return TCB 指针, 失败返回 NULL
 */
tcb_t *task_get_tcb(task_id_t task_id);

/**
 * @brief 获取下一个任务 ID (用于遍历)
 * @param task_id 当前任务 ID, -1 表示从头开始
 * @return 下一个任务 ID, 无更多任务返回 KERN_INVALID_ID
 */
task_id_t task_get_next(task_id_t task_id);

/**
 * @brief 设置任务优先级
 * @param task_id 任务 ID
 * @param priority 新优先级
 * @return 操作结果
 */
kern_err_t task_set_priority(task_id_t task_id, uint8_t priority);

/**
 * @brief 获取任务优先级
 * @param task_id 任务 ID
 * @return 优先级
 */
uint8_t task_get_priority(task_id_t task_id);

/*============================================================================
 * 任务延时
 *============================================================================*/

/**
 * @brief 任务延时 (ticks)
 * @param ticks 延时 tick 数
 * @return 操作结果
 */
kern_err_t task_delay(uint32_t ticks);

/**
 * @brief 任务延时 (毫秒)
 * @param ms 延时毫秒数
 * @return 操作结果
 */
kern_err_t task_delay_ms(uint32_t ms);

/**
 * @brief 任务延时直到指定 tick
 * @param tick 目标 tick
 * @return 操作结果
 */
kern_err_t task_delay_until(uint32_t tick);

/*============================================================================
 * 任务等待
 *============================================================================*/

/**
 * @brief 等待任务结束
 * @param task_id 任务 ID
 * @param retval 存储返回值的指针
 * @param timeout 超时 (ticks)
 * @return 操作结果
 */
kern_err_t task_join(task_id_t task_id, void **retval, uint32_t timeout);

/*============================================================================
 * 任务信息
 *============================================================================*/

/**
 * @brief 获取任务名称
 * @param task_id 任务 ID
 * @return 名称字符串
 */
const char *task_get_name(task_id_t task_id);

/**
 * @brief 获取任务状态
 * @param task_id 任务 ID
 * @return 任务状态
 */
task_state_t task_get_state(task_id_t task_id);

/*============================================================================
 * 空闲任务
 *============================================================================*/

/**
 * @brief 获取空闲任务 TCB
 * @return 空闲任务 TCB
 */
tcb_t *task_get_idle(void);

uint32_t task_get_used_bitmap(void);

#endif // TASK_H
