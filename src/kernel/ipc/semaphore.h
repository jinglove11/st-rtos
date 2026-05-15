/**
 * @file semaphore.h
 * @brief 信号量接口
 */

#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "kernel_types.h"

/*============================================================================
 * 信号量创建和删除
 *============================================================================*/

/**
 * @brief 创建信号量
 * @param initial_count 初始计数
 * @param max_count 最大计数 (0 表示无限制)
 * @return 信号量 ID, 或 KERN_INVALID_ID 失败
 */
sem_id_t sem_create(uint32_t initial_count, uint32_t max_count);

/**
 * @brief 删除信号量
 * @param sem_id 信号量 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t sem_delete(sem_id_t sem_id);

/*============================================================================
 * 信号量操作
 *============================================================================*/

/**
 * @brief 获取信号量 (等待)
 * @param sem_id 信号量 ID
 * @param timeout 超时 (ticks, 0=无限等待)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
 */
kern_err_t sem_wait(sem_id_t sem_id, uint32_t timeout);

#if SYSCALL_ENABLE
/**
 * @brief syscall-safe semaphore wait.
 *
 * This may return KERN_SYSCALL_BLOCKED after saving continuation state in the
 * current task. The SVC handler must switch away instead of returning directly.
 */
kern_err_t sem_wait_syscall(sem_id_t sem_id, uint32_t timeout);
#endif

/**
 * @brief 尝试获取信号量 (非阻塞)
 * @param sem_id 信号量 ID
 * @return KERN_OK 成功, KERN_ERR_BUSY 不可用, 其他失败
 */
kern_err_t sem_trywait(sem_id_t sem_id);

/**
 * @brief 释放信号量
 * @param sem_id 信号量 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t sem_post(sem_id_t sem_id);

/**
 * @brief 获取信号量当前计数
 * @param sem_id 信号量 ID
 * @return 当前计数, 或 -1 失败
 */
int32_t sem_get_count(sem_id_t sem_id);

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化信号量子系统
 */
void sem_init(void);

#endif // SEMAPHORE_H
