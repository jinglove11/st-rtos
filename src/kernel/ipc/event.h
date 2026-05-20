/**
 * @file event.h
 * @brief 事件标志组接口
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
 * @param opt 等待选项: BIT(0)=AND, BIT(1)=清除
 * @param timeout 超时 (ticks, 0=无限等待)
 * @param received 接收到的标志 (可选)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
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

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化事件标志组子系统
 */
void event_init(void);

#endif // EVENT_H
