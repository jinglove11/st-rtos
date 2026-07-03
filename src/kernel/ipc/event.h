/**
 * @file event.h
 * @brief Event flag group = notification object
 *
 * The event object IS the kernel's notification object (in the seL4/L4 sense):
 * a 32-bit signaled word that a waiter can block on and a signaler can post
 * from any context. It backs the notification path for drivers and the
 * supervisor's deferred-restart timer.
 *
 * ISR-safety contract:
 *   - event_set()    is ISR-safe (masks IRQs internally) — drivers/ISRs may
 *                    signal a waiter without going through a bottom half.
 *   - event_wait()   is NOT ISR-safe (it blocks) — it rejects ISR context
 *                    with KERN_ERR_ISR.
 *   - event_get()/event_clear() are ISR-safe (mask IRQs).
 */

#ifndef EVENT_H
#define EVENT_H

#include "kernel_types.h"

/*============================================================================
 * 事件标志组创建和删除
 *============================================================================*/

/**
 * @brief 创建事件标志组
 * @param initial_flags 初始标志
 * @return 事件标志组 ID, 或 KERN_INVALID_ID 失败
 */
event_id_t event_create(uint32_t initial_flags);

/**
 * @brief 删除事件标志组
 * @param event_id 事件标志组 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t event_delete(event_id_t event_id);

/*============================================================================
 * 事件标志组操作
 *============================================================================*/

/**
 * @brief 等待事件标志
 * @param event_id 事件标志组 ID
 * @param flags 请求的标志位
 * @param opt 等待选项: BIT(0)=AND, BIT(1)=清除, BIT(2)=NOWAIT(poll)
 * @param timeout 超时 (ticks, 0=无限等待)
 * @param received 接收到的标志 (可选)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
 *
 * With EVENT_OPT_NOWAIT: never blocks. Returns KERN_OK with the CURRENT word
 * copied to `received` (regardless of whether `flags` matched), so a caller
 * can poll the notification word without sleeping. If EVENT_OPT_CLEAR is also
 * set, the returned bits are cleared. This is the seL4 "poll" equivalent.
 *
 * Without NOWAIT and timeout==0: if `flags` don't match, returns
 * KERN_ERR_TIMEOUT without blocking (existing poll-by-timeout-0 behavior).
 */
kern_err_t event_wait(event_id_t event_id, uint32_t flags, uint32_t opt,
                      uint32_t timeout, uint32_t *received);

#if SYSCALL_ENABLE
/**
 * @brief 等待事件标志的 syscall continuation 版本
 */
kern_err_t event_wait_syscall(event_id_t event_id, uint32_t flags,
                              uint32_t opt, uint32_t timeout);
#endif

/**
 * @brief 设置事件标志
 * @param event_id 事件标志组 ID
 * @param flags 要设置的标志位
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t event_set(event_id_t event_id, uint32_t flags);

/**
 * @brief 清除事件标志
 * @param event_id 事件标志组 ID
 * @param flags 要清除的标志位
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t event_clear(event_id_t event_id, uint32_t flags);

/**
 * @brief 获取当前事件标志
 * @param event_id 事件标志组 ID
 * @return 当前标志, 或 0 失败
 */
uint32_t event_get(event_id_t event_id);

/*============================================================================
 * 等待选项
 *============================================================================*/

#define EVENT_OPT_AND       0   // 所有标志都必须设置
#define EVENT_OPT_OR        1   // 任一标志设置即可
#define EVENT_OPT_CLEAR     2   // 获取后清除标志
#define EVENT_OPT_NOWAIT    4   // poll: 立即返回当前 word 不阻塞(seL4 poll 等价)

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化事件标志组子系统
 */
void event_init(void);

#endif // EVENT_H
