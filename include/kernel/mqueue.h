/**
 * @file mqueue.h
 * @brief 消息队列接口
 */

#ifndef MQUEUE_H
#define MQUEUE_H

#include "kernel_types.h"

/*============================================================================
 * 消息队列创建和删除
 *============================================================================*/

/**
 * @brief 创建消息队列
 * @param msg_size 单条消息大小 (字节)
 * @param capacity 队列容量 (消息数)
 * @return 消息队列 ID, 或 KERN_INVALID_ID 失败
 */
queue_id_t mqueue_create(uint32_t msg_size, uint32_t capacity);

/**
 * @brief 删除消息队列
 * @param queue_id 消息队列 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t mqueue_delete(queue_id_t queue_id);

/*============================================================================
 * 消息队列操作
 *============================================================================*/

/**
 * @brief 发送消息 (等待)
 * @param queue_id 消息队列 ID
 * @param msg 消息指针
 * @param timeout 超时 (ticks, 0=无限等待)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
 */
kern_err_t mqueue_send(queue_id_t queue_id, const void *msg, uint32_t timeout);

#if SYSCALL_ENABLE
/**
 * @brief 发送消息的 syscall continuation 版本
 *
 * 用户态阻塞时不能停在 SVC 调用栈内等待；该接口会保存必要状态并返回
 * KERN_SYSCALL_BLOCKED，后续由调度器写回 syscall 返回值。
 */
kern_err_t mqueue_send_syscall(queue_id_t queue_id, const void *msg,
                               uint32_t timeout);
#endif

/**
 * @brief 尝试发送消息 (非阻塞)
 * @param queue_id 消息队列 ID
 * @param msg 消息指针
 * @return KERN_OK 成功, KERN_ERR_BUSY 队列满, 其他失败
 */
kern_err_t mqueue_trysend(queue_id_t queue_id, const void *msg);

/**
 * @brief 接收消息 (等待)
 * @param queue_id 消息队列 ID
 * @param msg 接收缓冲区
 * @param timeout 超时 (ticks, 0=无限等待)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
 */
kern_err_t mqueue_recv(queue_id_t queue_id, void *msg, uint32_t timeout);

#if SYSCALL_ENABLE
/**
 * @brief 接收消息的 syscall continuation 版本
 */
kern_err_t mqueue_recv_syscall(queue_id_t queue_id, void *user_msg,
                               uint32_t timeout);
#endif

/**
 * @brief 尝试接收消息 (非阻塞)
 * @param queue_id 消息队列 ID
 * @param msg 接收缓冲区
 * @return KERN_OK 成功, KERN_ERR_BUSY 队列空, 其他失败
 */
kern_err_t mqueue_tryrecv(queue_id_t queue_id, void *msg);

/**
 * @brief 获取队列中消息数量
 * @param queue_id 消息队列 ID
 * @return 消息数量, 或 -1 失败
 */
int32_t mqueue_get_count(queue_id_t queue_id);

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化消息队列子系统
 */
void mqueue_init(void);

#endif // MQUEUE_H
