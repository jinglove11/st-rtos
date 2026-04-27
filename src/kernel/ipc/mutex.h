/**
 * @file mutex.h
 * @brief 互斥锁接口
 */

#ifndef MUTEX_H
#define MUTEX_H

#include "kernel_types.h"

/*============================================================================
 * 互斥锁创建和删除
 *============================================================================*/

/**
 * @brief 创建互斥锁
 * @return 互斥锁 ID, 或 KERN_INVALID_ID 失败
 */
mutex_id_t mutex_create(void);

/**
 * @brief 删除互斥锁
 * @param mutex_id 互斥锁 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t mutex_delete(mutex_id_t mutex_id);

/*============================================================================
 * 互斥锁操作
 *============================================================================*/

/**
 * @brief 获取互斥锁 (等待)
 * @param mutex_id 互斥锁 ID
 * @param timeout 超时 (ticks, 0=无限等待)
 * @return KERN_OK 成功, KERN_ERR_TIMEOUT 超时, 其他失败
 */
kern_err_t mutex_lock(mutex_id_t mutex_id, uint32_t timeout);

/**
 * @brief 尝试获取互斥锁 (非阻塞)
 * @param mutex_id 互斥锁 ID
 * @return KERN_OK 成功, KERN_ERR_BUSY 不可用, 其他失败
 */
kern_err_t mutex_trylock(mutex_id_t mutex_id);

/**
 * @brief 释放互斥锁
 * @param mutex_id 互斥锁 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t mutex_unlock(mutex_id_t mutex_id);

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化互斥锁子系统
 */
void mutex_init(void);

#endif // MUTEX_H
