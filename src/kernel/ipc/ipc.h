/**
 * @file ipc.h
 * @brief IPC 模块统一接口
 */

#ifndef IPC_H
#define IPC_H

#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
#include "endpoint.h"
#include "channel.h"

/**
 * @brief 初始化所有 IPC 模块
 */
static inline void ipc_init(void) {
    sem_init();
    mutex_init();
    mqueue_init();
    event_init();
    endpoint_init();
    channel_init();
}

#endif // IPC_H
