/**
 * @file hal.h
 * @brief 硬件抽象层（HAL）接口定义
 *
 * ============================================================================
 * 模块概述
 * ============================================================================
 *
 * 硬件抽象层（Hardware Abstraction Layer）是 RTOS 与硬件之间的桥梁。
 * 它将硬件相关的操作封装成统一的接口，使得内核代码可以跨平台运行。
 *
 * 主要功能：
 * 1. CPU 抽象 - 中断控制、临界区保护
 * 2. 系统时钟 - SysTick 配置和滴答计数
 * 3. 上下文切换 - 栈初始化、PendSV/SVC 触发
 * 4. 中断控制器 - NVIC 操作
 * 5. 调试支持 - 串口输出
 * 6. 看门狗 - 系统监控
 *
 * ============================================================================
 * 移植指南
 * ============================================================================
 *
 * 要将 RTOS 移植到新的硬件平台，需要实现以下接口：
 *
 * 必须实现：
 *   - hal_cpu_init()          CPU 初始化
 *   - hal_enter_critical()    进入临界区
 *   - hal_exit_critical()     退出临界区
 *   - hal_stack_init()        初始化任务栈
 *   - hal_trigger_pendsv()    触发上下文切换
 *   - hal_trigger_first_switch() 启动第一个任务
 *   - hal_systick_init()      初始化系统滴答
 *   - hal_debug_putc()        调试输出
 *
 * 可选实现：
 *   - hal_watchdog_*()        看门狗
 *   - hal_enter_lowpower()    低功耗模式
 *
 * ============================================================================
 */

#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include "kernel_config.h"

/*============================================================================
 * CPU 抽象接口
 *============================================================================*/

/**
 * @brief 初始化 CPU
 *
 * 执行 CPU 相关的初始化：
 * - 设置向量表位置
 * - 初始化中断优先级
 * - 配置缓存（如果有）
 *
 * @note 必须在调用任何其他 HAL 函数之前调用
 */
void hal_cpu_init(void);

/*============================================================================
 * 调试接口
 *============================================================================*/

/**
 * @brief 输出单个字符到调试端口
 * @param c 要输出的字符
 */
void hal_debug_putc(char c);

/**
 * @brief 输出字符串到调试端口
 * @param s 要输出的字符串（以 null 结尾）
 */
void hal_debug_puts(const char *s);

/*============================================================================
 * 系统时钟接口
 *============================================================================*/

/**
 * @brief 初始化 SysTick 定时器
 *
 * @param rate_hz 滴答频率（Hz）
 *
 * 例如：rate_hz = 1000 表示每毫秒产生一次滴答中断
 */
void hal_systick_init(uint32_t rate_hz);

/**
 * @brief 获取 SysTick 计数
 * @return 当前滴答计数
 */
uint32_t hal_systick_get(void);

/**
 * @brief 使能 SysTick
 */
void hal_systick_enable(void);

/**
 * @brief 禁用 SysTick
 */
void hal_systick_disable(void);

/*============================================================================
 * 中断控制接口
 *============================================================================*/

/**
 * @brief 使能全局中断
 *
 * 使能所有可配置优先级的中断。
 * 对应 ARM Cortex-M 的 CPSIE I 指令。
 */
void hal_irq_enable(void);

/**
 * @brief 禁用全局中断
 *
 * 禁用所有可配置优先级的中断。
 * 对应 ARM Cortex-M 的 CPSID I 指令。
 *
 * @warning 此函数会影响实时性，建议使用临界区接口代替
 */
void hal_irq_disable(void);

/**
 * @brief 保存并禁用中断
 *
 * 保存当前中断状态，然后禁用中断。
 * 返回值用于 hal_irq_restore() 恢复之前的状态。
 *
 * @return 之前的中断状态
 *
 * @code
 * uint32_t state = hal_irq_save();
 * // 临界区代码
 * hal_irq_restore(state);
 * @endcode
 */
uint32_t hal_irq_save(void);

/**
 * @brief 恢复中断状态
 * @param primask 之前由 hal_irq_save() 返回的状态值
 */
void hal_irq_restore(uint32_t primask);

/*============================================================================
 * 临界区接口（基于 BASEPRI）
 * ============================================================================
 *
 * 为什么使用 BASEPRI 而不是 PRIMASK？
 *
 * PRIMASK 方式：
 *   - 禁用所有中断
 *   - 简单但影响实时性
 *   - 临界区内无法响应任何中断
 *
 * BASEPRI 方式：
 *   - 只屏蔽优先级低于阈值的中断
 *   - 高优先级中断仍可响应
 *   - 适合 RTOS 临界区保护
 *============================================================================*/

/**
 * @brief 进入临界区
 *
 * 使用 BASEPRI 屏蔽低优先级中断，实现临界区保护。
 * 高优先级中断（如 NMI、HardFault）仍可响应。
 *
 * @return 之前的 BASEPRI 值，用于 hal_exit_critical() 恢复
 *
 * @note 支持临界区嵌套
 * @note 临界区内，优先级 >= SCHED_CRITICAL_PRIORITY 的中断被屏蔽
 *
 * @code
 * uint32_t crit = hal_enter_critical();
 * // 临界区代码
 * hal_exit_critical(crit);
 * @endcode
 */
uint32_t hal_enter_critical(void);

/**
 * @brief 退出临界区
 * @param basepri 之前由 hal_enter_critical() 返回的值
 */
void hal_exit_critical(uint32_t basepri);

/*============================================================================
 * 中断优先级接口
 *============================================================================*/

/**
 * @brief 初始化中断优先级
 *
 * 设置系统异常的优先级：
 * - SVC：用于首次任务切换
 * - PendSV：用于上下文切换
 * - SysTick：用于系统滴答
 */
void hal_interrupt_priority_init(void);

/**
 * @brief 设置中断优先级
 *
 * @param irq      中断号
 * @param priority 优先级（数值越小优先级越高）
 */
void hal_irq_set_priority(uint32_t irq, uint32_t priority);

/**
 * @brief 使能指定中断
 * @param irq 中断号
 */
void hal_irq_enable_irq(uint32_t irq);

/**
 * @brief 禁用指定中断
 * @param irq 中断号
 */
void hal_irq_disable_irq(uint32_t irq);

/**
 * @brief 清除中断挂起状态
 * @param irq 中断号
 */
void hal_irq_clear_pending(uint32_t irq);

/**
 * @brief 设置中断向量
 *
 * 将 IRQ handler 写入 RAM 向量表对应位置。
 * 需先启用 RAM 向量表 (VTOR 重映射到 SRAM)。
 *
 * @param irq     中断号 (外设 IRQ 从 0 开始)
 * @param handler 中断处理函数
 */
void hal_irq_set_vector(uint32_t irq, void (*handler)(void));

/**
 * @brief 获取当前活跃的中断号
 * @return 中断号 (外设 IRQ 0-97), 或 -1 表示任务上下文
 */
int32_t hal_irq_get_active(void);

/*============================================================================
 * 电源管理接口
 *============================================================================*/

/**
 * @brief 进入低功耗模式
 *
 * 执行 WFI（Wait For Interrupt）指令，进入睡眠模式。
 * 当有中断发生时，CPU 自动唤醒。
 */
void hal_enter_lowpower(void);

/**
 * @brief 退出低功耗模式
 *
 * WFI 指令会在中断发生时自动退出。
 */
void hal_exit_lowpower(void);

/*============================================================================
 * 上下文切换接口
 *============================================================================*/

/**
 * @brief 初始化任务栈
 *
 * 为新任务创建初始栈帧，使得任务第一次运行时能够正确恢复上下文。
 *
 * @param stack_top  栈顶地址（高地址）
 * @param stack_size 栈大小（字节）
 * @param entry      任务入口函数
 * @param arg        任务参数
 * @param exit       任务退出处理函数
 *
 * @return 初始化后的栈指针（SP）
 *
 * @note 栈帧布局符合 ARM Cortex-M 异常返回规范
 */
void *hal_stack_init(void *stack_top, uint32_t stack_size, void *entry, void *arg, void *exit);

/**
 * @brief 触发 PendSV 异常
 *
 * 设置 PendSV 挂起位，触发上下文切换。
 * PendSV 会在当前中断处理完成后执行（优先级最低）。
 */
void hal_trigger_pendsv(void);

/**
 * @brief 触发 SVC 异常
 * @param svc_num SVC 号
 */
void hal_trigger_svc(uint32_t svc_num);

/**
 * @brief 触发第一次任务切换
 *
 * 通过 SVC 异常启动第一个任务。
 * 由 sched_start() 调用。
 *
 * @warning 此函数不会返回
 */
void hal_trigger_first_switch(void);

/*============================================================================
 * 看门狗接口
 *============================================================================*/

#if KERN_WATCHDOG_ENABLE

/**
 * @brief 初始化看门狗
 * @param timeout_ms 超时时间（毫秒）
 */
void hal_watchdog_init(uint32_t timeout_ms);

#else

/**
 * @brief 初始化看门狗（空实现）
 */
void hal_watchdog_init(void);

#endif

/**
 * @brief 喂狗
 *
 * 重置看门狗计数器，防止系统复位。
 */
void hal_watchdog_feed(void);

/**
 * @brief 读取复位原因
 * @return RCC_CSR 复位标志位
 */
uint32_t hal_watchdog_reset_cause(void);

/*============================================================================
 * 时间接口
 *============================================================================*/

/**
 * @brief 获取系统滴答计数
 * @return 当前滴答计数
 */
uint32_t hal_get_tick_count(void);

/**
 * @brief 获取当前 CPU 核心 ID
 * @return 0 (core0) 或 1 (core1),单核架构始终返回 0
 */
uint32_t hal_get_cpu_id(void);

/**
 * @brief 设置系统滴答计数
 * @param count 新的计数值
 */
void hal_set_tick_count(uint32_t count);

/**
 * @brief 系统软件复位
 *
 * 通过 AIRCR.SYSRESETREQ 触发系统复位。
 */
void hal_system_reset(void);

#endif /* HAL_H */
