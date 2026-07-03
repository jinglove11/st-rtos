/**
 * @file irq.h
 * @brief 中断管理接口 — ISR 注册、线程化 IRQ、上下文检测
 */

#ifndef IRQ_H
#define IRQ_H

#include "kernel_types.h"

/*============================================================================
 * 初始化
 *============================================================================*/

void irq_init(void);
void irq_service_start(void);

/*============================================================================
 * ISR 注册
 *============================================================================*/

/**
 * @brief 注册 ISR 处理函数
 * @param irq      中断号 (外设 IRQ 从 0 开始)
 * @param handler  中断处理函数
 * @param priority NVIC 优先级 (0=最高, 14=最低)
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_register(int16_t irq, isr_func_t handler, uint8_t priority);

/**
 * @brief 注销 ISR
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_unregister(int16_t irq);

/**
 * @brief 使能指定中断
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_enable(int16_t irq);

/**
 * @brief 禁用指定中断
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_disable(int16_t irq);

/**
 * @brief 将 IRQ 事件绑定到 endpoint 通知
 * @param irq   中断号
 * @param ep_id 通知目标 endpoint
 * @param badge 写入通知消息首字的 badge
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_bind_endpoint(int16_t irq, ep_id_t ep_id, uint32_t badge);

/**
 * @brief 投递一次 IRQ endpoint 通知
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 *
 * @note 当前版本只能在任务上下文调用；真实 ISR 到 endpoint 的队列化
 *       mask/ack 策略后续补齐。推荐用 irq_bind_event(ISR 安全)。
 */
kern_err_t irq_notify(int16_t irq);

/**
 * @brief 将 IRQ 绑定到 event(notification)对象 —— ISR 安全通知路径
 *
 * 当该 IRQ 在 ISR 上下文触发时,内核直接 event_set(event, flags) 通知等待
 * 的用户任务。这是推荐的 IRQ→用户态投递路径(irq_notify/endpoint 路径不能
 * 在 ISR 调用,event_set 可以)。
 *
 * @param irq      中断号
 * @param event_id 目标 event 对象 id
 * @param flags    要 set 的 event flag 位(ISR 触发时写入)
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_bind_event(int16_t irq, event_id_t event_id, uint32_t flags);

/**
 * @brief 解除 IRQ 的 event 绑定
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_unbind_event(int16_t irq);

#if CAP_ENABLE
kern_err_t kirq_create_cap(int16_t irq, uint8_t rights, cap_id_t *out_cap);
kern_err_t kirq_delete_cap(cap_id_t cap);
kern_err_t kirq_get_number(cap_id_t cap, int16_t *irq);
kern_err_t kirq_bind_endpoint(cap_id_t cap, ep_id_t ep_id, uint32_t badge);
kern_err_t kirq_bind_event(cap_id_t cap, event_id_t event_id, uint32_t flags);
#endif

/*============================================================================
 * 中断上下文检测
 *============================================================================*/

/**
 * @brief 检查当前是否在中断上下文中
 * @return 1=中断上下文, 0=任务上下文
 */
int kern_is_in_isr(void);

/**
 * @brief 获取当前 IRQ 号
 * @return IRQ 号 (0-97), 或 -1 表示任务上下文
 */
int kern_irq_context(void);

/*============================================================================
 * 线程化 IRQ
 *============================================================================*/

#if IRQ_THREADED_ENABLE

/**
 * @brief 请求线程化 IRQ
 *
 * 创建一个 ISR+任务对：ISR 禁用 IRQ 并唤醒任务，
 * 任务在任务上下文中执行 handler。
 *
 * @param irq        中断号
 * @param handler    任务上下文中的处理函数
 * @param arg        用户参数
 * @param priority   任务优先级
 * @param stack_size 任务栈大小
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_request_threaded(int16_t irq, task_func_t handler,
                                void *arg, uint8_t priority,
                                uint32_t stack_size);

/**
 * @brief 释放线程化 IRQ
 * @param irq 中断号
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t irq_release_threaded(int16_t irq);

#endif /* IRQ_THREADED_ENABLE */

#endif /* IRQ_H */
