/**
 * @file bh.h
 * @brief 底半部 (Bottom Half) 接口 — ISR 安全延迟处理
 */

#ifndef BH_H
#define BH_H

#include "kernel_types.h"

/*============================================================================
 * 初始化
 *============================================================================*/

void bh_init(void);
void bh_service_start(void);

/*============================================================================
 * 底半部生命周期
 *============================================================================*/

/**
 * @brief 创建底半部槽位
 * @param handler 处理函数
 * @param arg     参数
 * @return bh ID (>=0), 或 KERN_INVALID_ID 失败
 */
int16_t bh_create(bh_handler_t handler, void *arg);

/**
 * @brief 调度底半部执行 (ISR 安全)
 * @param bh_id 底半部 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t bh_schedule(int16_t bh_id);

/**
 * @brief 删除底半部
 * @param bh_id 底半部 ID
 * @return KERN_OK 成功, 其他失败
 */
kern_err_t bh_delete(int16_t bh_id);

#endif /* BH_H */
