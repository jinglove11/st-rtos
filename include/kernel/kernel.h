/**
 * @file kernel.h
 * @brief 内核主接口
 */

#ifndef KERNEL_H
#define KERNEL_H

#include "kernel_types.h"
#include "task.h"
#include "scheduler.h"
#include "irq.h"
#include "bh.h"
#include "mpu.h"
#include "syscall.h"
#include "capability.h"
#include "fault.h"
/* Phase #23: vfs.h 移除 (内核 VFS 已删) */

/*============================================================================
 * 内核控制
 *============================================================================*/

/**
 * @brief 初始化内核
 */
void kern_init(void);

/**
 * @brief 启动内核
 */
void kern_start(void) __attribute__((noreturn));

/**
 * @brief 获取内核版本
 * @param major 存储主版本号
 * @param minor 存储次版本号
 * @param patch 存储补丁号
 */
void kern_get_version(uint8_t *major, uint8_t *minor, uint8_t *patch);

/**
 * @brief 获取内核名称
 * @return 内核名称字符串
 */
const char *kern_get_name(void);

/**
 * @brief 获取系统 tick 计数
 * @return tick 计数
 */
uint32_t kern_get_tick(void);

/**
 * @brief 内核恐慌 (致命错误)
 * @param msg 错误消息
 */
void kern_panic(const char *msg) __attribute__((noreturn));

/*============================================================================
 * 内核统计
 *============================================================================*/

#if KERN_TASK_STATS

/**
 * @brief 获取任务数量
 * @return 任务数量
 */
uint32_t kern_get_task_count(void);

/**
 * @brief 遍历所有任务
 * @param callback 回调函数
 * @param arg 回调参数
 */
void kern_foreach_task(void (*callback)(tcb_t *tcb, void *arg), void *arg);

#endif

#endif // KERNEL_H
