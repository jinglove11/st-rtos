/**
 * @file timer.h
 * @brief 软件定时器接口
 *
 * ============================================================================
 * 模块概述
 * ============================================================================
 *
 * 软件定时器提供灵活的定时触发机制：
 *
 * 1. 单次定时器
 *    - 触发一次后自动停止
 *    - 适用于延迟执行、超时检测
 *
 * 2. 周期定时器
 *    - 按固定周期重复触发
 *    - 适用于周期性任务（采样、心跳等）
 *
 * 3. 线程安全
 *    - 使用命令队列，任意上下文可调用 API
 *    - 回调在任务上下文执行，可调用阻塞 API
 *
 * ============================================================================
 * 使用方法
 * ============================================================================
 *
 * // 创建周期定时器
 * void my_callback(void *arg) {
 *     // 定时器回调，在任务上下文执行
 * }
 *
 * timer_id_t tid = timer_create("my_timer", my_callback, NULL, 100);
 * timer_start(tid, 0);  // 立即开始
 *
 * // 停止并删除
 * timer_stop(tid);
 * timer_delete(tid);
 *
 * ============================================================================
 */

#ifndef TIMER_H
#define TIMER_H

#include "kernel_types.h"

/*============================================================================
 * 定时器创建和删除
 *============================================================================*/

/**
 * @brief 创建定时器
 *
 * @param name     定时器名称
 * @param callback 回调函数（在任务上下文执行）
 * @param arg      回调参数
 * @param period   周期（ticks），0 表示单次触发
 *
 * @return 定时器 ID，失败返回 KERN_INVALID_ID
 *
 * @note 回调在定时器服务任务上下文执行，可以调用阻塞 API
 * @note 创建后定时器处于停止状态，需要调用 timer_start() 启动
 */
timer_id_t timer_create(const char *name, timer_callback_t callback,
                        void *arg, uint32_t period);

/**
 * @brief 删除定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 如果定时器正在执行回调，会等待回调完成
 */
kern_err_t timer_delete(timer_id_t timer_id);

/*============================================================================
 * 定时器控制
 *============================================================================*/

/**
 * @brief 启动定时器
 *
 * @param timer_id 定时器 ID
 * @param delay    首次触发的延迟（ticks），0 表示立即开始
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 此函数发送命令到队列，立即返回
 * @note 可以在中断中安全调用
 */
kern_err_t timer_start(timer_id_t timer_id, uint32_t delay);

/**
 * @brief 停止定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 定时器停止后可以从堆中移除，不再触发回调
 */
kern_err_t timer_stop(timer_id_t timer_id);

/**
 * @brief 重置定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 重新开始计时，相当于 stop + start
 */
kern_err_t timer_reset(timer_id_t timer_id);

/**
 * @brief 修改定时器周期
 *
 * @param timer_id   定时器 ID
 * @param new_period 新周期（ticks）
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 修改周期后定时器会重新开始计时
 */
kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period);

/*============================================================================
 * 定时器状态查询
 *============================================================================*/

/**
 * @brief 获取定时器状态
 *
 * @param timer_id 定时器 ID
 *
 * @return 定时器状态
 */
timer_state_t timer_get_state(timer_id_t timer_id);

/**
 * @brief 获取定时器剩余时间
 *
 * @param timer_id 定时器 ID
 *
 * @return 剩余 ticks，-1 表示失败或定时器未启动
 */
int32_t timer_get_remaining(timer_id_t timer_id);

/**
 * @brief 检查定时器是否正在运行
 *
 * @param timer_id 定时器 ID
 *
 * @return 1 正在运行，0 未运行或失败
 */
int timer_is_active(timer_id_t timer_id);

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化定时器模块
 *
 * @note 由 kern_init() 内部调用
 */
void timer_init(void);

/**
 * @brief 启动定时器服务
 *
 * @note 由 kern_start() 内部调用
 */
void timer_service_start(void);

#endif /* TIMER_H */
