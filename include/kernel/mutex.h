/**
 * @file mutex.h
 * @brief 互斥锁接口
 */

#ifndef MUTEX_H
#define MUTEX_H

#define mutex_init kern_mutex_init

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

#if SYSCALL_ENABLE
/**
 * @brief syscall-safe mutex lock.
 *
 * This may return KERN_SYSCALL_BLOCKED after saving continuation state in the
 * current task. The SVC handler must switch away instead of returning directly.
 */
kern_err_t mutex_lock_syscall(mutex_id_t mutex_id, uint32_t timeout);
#endif

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

/*============================================================================
 * 死锁检测
 *============================================================================*/

/**
 * @brief 扫描所有互斥锁和任务，检测死锁循环
 *
 * 遍历所有任务，检查阻塞在互斥锁上的任务是否构成等待图环。
 *
 * @return 检测到的死锁任务数量（0 表示无死锁）
 *
 * @note mutex_lock() 已内联检测，此函数用于诊断和调试。
 */
int mutex_deadlock_check(void);

/*============================================================================
 * M2-Step3a: cap 路径 id ↔ 对象指针 转换
 *============================================================================*/

mutex_id_t mutex_id_from_obj(void *obj);
void *mutex_obj_for_cap(mutex_id_t id);
void mutex_cleanup_task(void *mutex_obj, tcb_t *tcb);

#endif // MUTEX_H
