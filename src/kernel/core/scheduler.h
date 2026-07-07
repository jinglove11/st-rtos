/**
 * @file scheduler.h
 * @brief 调度器接口定义
 *
 * ============================================================================
 * 模块概述
 * ============================================================================
 *
 * 调度器是 RTOS 的核心组件，负责管理任务的执行顺序。本调度器采用以下设计：
 *
 * 1. 优先级抢占调度
 *    - 高优先级任务总是抢占低优先级任务
 *    - 优先级数值越小，优先级越高（0 为最高）
 *
 * 2. 时间片轮转（可选）
 *    - 同优先级任务按时间片轮转执行
 *    - 时间片用完后任务被放回就绪队列尾部
 *
 * 3. O(1) 时间复杂度
 *    - 使用位图快速查找最高优先级任务
 *    - 就绪队列使用双向链表，插入/删除都是 O(1)
 *
 * ============================================================================
 * 使用方法
 * ============================================================================
 *
 * 1. 初始化：
 *    kern_init();           // 初始化内核（内部调用 sched_init()）
 *
 * 2. 创建任务：
 *    task_id_t task = task_create("name", func, arg, priority, stack_size);
 *    task_start(task);
 *
 * 3. 启动调度器：
 *    kern_start();          // 启动调度器，开始运行第一个任务
 *
 * ============================================================================
 * API 分类
 * ============================================================================
 *
 * 初始化与启动：
 *   - sched_init()    初始化调度器
 *   - sched_start()   启动调度器
 *
 * 调度控制：
 *   - sched_yield()   主动让出 CPU
 *   - sched_add_ready()   将任务加入就绪队列
 *   - sched_remove_ready() 从就绪队列移除任务
 *
 * 阻塞与唤醒：
 *   - sched_block()   阻塞当前任务
 *   - sched_wakeup()  唤醒阻塞的任务
 *
 * 状态查询：
 *   - sched_get_current()      获取当前任务
 *   - sched_get_highest_ready() 获取最高优先级就绪任务
 *   - sched_get_tick_count()   获取系统滴答计数
 *
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "kernel_types.h"

/*============================================================================
 * Per-CPU current/next task (accessed by PendSV/SVC assembly)
 *============================================================================*/

extern tcb_t *volatile _current_task[SMP_MAX_CPUS];
extern tcb_t *volatile _next_task[SMP_MAX_CPUS];

/*============================================================================
 * 初始化与启动接口
 *============================================================================*/

/**
 * @brief 初始化调度器
 *
 * 初始化调度器的内部数据结构：
 * - 清空所有就绪队列
 * - 清零优先级位图
 * - 重置调度器状态
 *
 * @note 此函数由 kern_init() 内部调用，用户通常不需要直接调用
 * @note 必须在创建任何任务之前调用
 *
 * @see kern_init()
 */
void sched_init(void);

/**
 * @brief 启动调度器
 *
 * 启动调度器，开始运行第一个任务。此函数执行以下操作：
 * 1. 检查是否有任务可运行
 * 2. 选择最高优先级任务
 * 3. 启动 SysTick 定时器
 * 4. 使能全局中断
 * 5. 触发 SVC 异常，切换到第一个任务
 *
 * @warning 此函数不会返回！调用后的代码不会执行
 *
 * @pre 必须先调用 kern_init() 和创建至少一个任务
 *
 * @code
 * // 典型使用流程
 * kern_init();
 * task_id_t task = task_create("main", main_task, NULL, 10, 0);
 * task_start(task);
 * kern_start();  // 不会返回
 * while (1);     // 永远不会执行
 * @endcode
 */
void sched_start(void) __attribute__((noreturn));

/*============================================================================
 * 调度控制接口
 *============================================================================*/

/**
 * @brief 主动让出 CPU
 *
 * 当前任务主动放弃 CPU 使用权，触发调度器选择下一个任务运行。
 * 当前任务会被放回就绪队列尾部，等待下次被调度。
 *
 * @note 调用此函数后，当前任务会立即暂停执行
 * @note 当任务再次被调度时，会从调用点继续执行
 *
 * @code
 * void cooperative_task(void *arg) {
 *     while (1) {
 *         do_some_work();
 *         sched_yield();  // 让出 CPU，给其他任务机会
 *     }
 * }
 * @endcode
 *
 * @see task_yield()
 */
void sched_yield(void);

/**
 * @brief 将任务加入就绪队列
 *
 * 将指定任务加入就绪队列，使其变为可运行状态。
 * 如果任务优先级高于当前运行任务，会触发抢占调度。
 *
 * @param tcb 任务控制块指针
 *
 * @note 如果任务已经在就绪队列中，此函数不会有任何效果
 * @note 通常由 task_start() 或 task_resume() 内部调用
 *
 * @see task_start()
 * @see task_resume()
 */
void sched_add_ready(tcb_t *tcb);

/**
 * @brief 将任务从就绪队列移除
 *
 * 将指定任务从就绪队列中移除，使其变为不可运行状态。
 * 通常用于挂起或删除任务。
 *
 * @param tcb 任务控制块指针
 *
 * @note 如果任务不在就绪队列中，此函数不会有任何效果
 *
 * @see task_suspend()
 * @see task_delete()
 */
void sched_remove_ready(tcb_t *tcb);

/**
 * @brief 重新插入就绪队列（优先级变化时调用）
 *
 * 当任务的优先级发生变化时，需要将其从原优先级队列移除，
 * 并插入到新优先级队列中。此函数主要用于优先级继承机制。
 *
 * @param tcb 任务控制块指针
 *
 * @note 任务必须处于 READY 状态才会被重新插入
 * @note 此函数在临界区内被调用，不会嵌套临界区
 *
 * @see mutex_priority_inherit()
 */
void sched_reinsert_by_priority(tcb_t *tcb);

/*============================================================================
 * 阻塞与唤醒接口
 *============================================================================*/

/**
 * @brief 阻塞当前任务
 *
 * 将当前任务从就绪队列移除，使其暂停执行，直到被唤醒或超时。
 * 此函数是实现 task_delay()、mutex_lock()、sem_wait() 等阻塞 API 的基础。
 *
 * @param reason  阻塞原因，参见 block_reason_t 枚举
 * @param obj     阻塞对象的指针（如互斥锁、信号量），可为 NULL
 * @param timeout 超时时间（滴答数），0 表示无限等待
 *
 * @return 唤醒时的结果码：
 *         - KERN_OK: 正常唤醒（资源可用）
 *         - KERN_ERR_TIMEOUT: 超时唤醒
 *         - KERN_ERR_DELETED: 等待的对象被删除
 *
 * @note 此函数会触发调度，当前任务立即暂停执行
 * @note 当任务被唤醒后，会从调用点继续执行，并返回唤醒结果
 *
 * @code
 * // 实现简单的延时
 * kern_err_t task_delay(uint32_t ticks) {
 *     if (ticks == 0) return KERN_OK;
 *     return sched_block(BLOCK_REASON_SLEEP, NULL, ticks);
 * }
 * @endcode
 *
 * @see sched_wakeup()
 * @see block_reason_t
 */
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout);

/**
 * @brief 唤醒阻塞的任务
 *
 * 将阻塞的任务重新加入就绪队列，使其可以继续执行。
 * 如果唤醒的任务优先级高于当前运行任务，会触发抢占调度。
 *
 * @param tcb    任务控制块指针
 * @param result 唤醒结果码，将作为 sched_block() 的返回值
 *
 * @note 如果任务不处于 BLOCKED 状态，此函数不会有任何效果
 * @note 通常由资源释放函数（如 mutex_unlock()、sem_post()）内部调用
 *
 * @code
 * // 互斥锁解锁时唤醒等待任务
 * void mutex_unlock(mutex_id_t mutex) {
 *     tcb_t *waiter = get_highest_waiter(mutex);
 *     if (waiter) {
 *         sched_wakeup(waiter, KERN_OK);
 *     }
 * }
 * @endcode
 *
 * @see sched_block()
 */
void sched_wakeup(tcb_t *tcb, kern_err_t result);

/*============================================================================
 * 状态查询接口
 *============================================================================*/

/**
 * @brief 获取当前运行任务的 TCB
 *
 * @return 当前运行任务的 TCB 指针
 * @retval NULL 如果没有任务运行（调度器未启动）
 *
 * @code
 * task_id_t task_self(void) {
 *     tcb_t *current = sched_get_current();
 *     return current ? current->id : KERN_INVALID_ID;
 * }
 * @endcode
 */
tcb_t *sched_get_current(void);

/**
 * @brief 通过任务 ID 获取任务 TCB
 *
 * @param task_id 任务 ID
 *
 * @return 任务 TCB 指针
 * @retval NULL 如果任务不存在或已终止
 *
 * @note 此函数是对 task_get_tcb() 的封装
 */
tcb_t *sched_get_tcb(task_id_t task_id);

/**
 * @brief 获取最高优先级的就绪任务
 *
 * 查找并返回当前就绪队列中优先级最高的任务。
 * 此函数使用位图实现 O(1) 时间复杂度。
 *
 * @return 最高优先级就绪任务的 TCB 指针
 * @retval NULL 如果没有就绪任务
 *
 * @note 此函数主要用于内部调度逻辑，用户通常不需要直接调用
 */
tcb_t *sched_get_highest_ready(void);

/**
 * @brief 检查是否需要调度
 *
 * @return 非零值表示需要调度，0 表示不需要
 *
 * @note 此函数主要用于内部调度逻辑
 */
int sched_need_switch(void);

/**
 * @brief 获取系统滴答计数
 *
 * 返回自调度器启动以来的滴答数。滴答频率由 KERN_TICK_RATE_HZ 定义。
 *
 * @return 当前滴答计数
 *
 * @code
 * // 计算经过的时间
 * uint32_t start = sched_get_tick_count();
 * do_something();
 * uint32_t elapsed = sched_get_tick_count() - start;
 * @endcode
 *
 * @see KERN_TICK_RATE_HZ
 */
uint32_t sched_get_tick_count(void);

/*============================================================================
 * 时钟滴答处理接口
 *============================================================================*/

/**
 * @brief 时钟滴答处理函数
 *
 * 由 SysTick 中断调用，处理以下任务：
 * 1. 更新系统滴答计数
 * 2. 处理时间片轮转（如果启用）
 * 3. 检查超时唤醒
 *
 * @note 此函数在中断上下文中调用，用户不应直接调用
 * @note 由 hal_systick_handler() 内部调用
 *
 * @see hal_systick_handler()
 */
void sched_tick_handler(void);

/*============================================================================
 * 调度器统计接口（可选）
 *============================================================================*/

#if KERN_TASK_STATS

uint32_t sched_get_cpu_usage(tcb_t *tcb);

#endif /* KERN_TASK_STATS */

#endif /* SCHEDULER_H */
