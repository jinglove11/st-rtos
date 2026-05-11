# Software Timer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a complete software timer module with min-heap scheduling and command queue for thread-safe timer operations.

**Architecture:** Min-heap organizes active timers by expiration time. Command queue receives timer operations from any context. Timer service task processes commands and executes callbacks in task context (safe to call blocking APIs).

**Tech Stack:** C (embedded), ARM Cortex-M7, My-RTOS kernel APIs (scheduler, mqueue, task)

---

## File Structure

| File | Purpose |
|------|---------|
| `src/kernel/timer/timer.h` | Timer API interface definitions |
| `src/kernel/timer/timer.c` | Timer implementation (heap, service task, commands) |
| `Kconfig` | Add timer configuration options |
| `src/kernel/include/kernel_types.h` | Update timer_t structure |
| `src/kernel/include/kernel_config.h` | Add timer config macros (auto-generated) |
| `src/kernel/kernel.c` | Integrate timer_init() and timer_service_start() |
| `src/tests/test_timer.c` | Timer test module |

---

## Task 1: Add Kconfig Timer Options

**Files:**
- Modify: `Kconfig` (append after IPC menu)

- [ ] **Step 1: Add timer configuration menu to Kconfig**

Append to `/home/five/my-rtos/Kconfig` after the Test Configuration menu:

```kconfig
#=============================================================================
# 定时器配置
#=============================================================================

menu "Timer Configuration"

config TIMER_ENABLE
    bool "Enable software timer"
    default y
    help
      启用软件定时器支持。

config TIMER_MAX
    int "Maximum timers"
    depends on TIMER_ENABLE
    default 16
    range 4 64
    help
      最大定时器数量。

config TIMER_CMD_QUEUE_SIZE
    int "Timer command queue size"
    depends on TIMER_ENABLE
    default 8
    range 4 32
    help
      定时器命令队列大小。

config TIMER_TASK_PRIORITY
    int "Timer service task priority"
    depends on TIMER_ENABLE
    default 1
    range 0 31
    help
      定时器服务任务优先级。
      数值越小优先级越高。
      建议设为较高优先级以确保定时器响应及时。

config TIMER_TASK_STACK_SIZE
    int "Timer service task stack size (bytes)"
    depends on TIMER_ENABLE
    default 512
    range 256 2048
    help
      定时器服务任务栈大小（字节）。

config TIMER_NAME_LEN
    int "Timer name length"
    depends on TIMER_ENABLE
    default 16
    range 8 32
    help
      定时器名称最大长度。

endmenu
```

- [ ] **Step 2: Run menuconfig to regenerate config**

```bash
cd /home/five/my-rtos
python scripts/menuconfig.py defconfig
python scripts/menuconfig.py genconfig
```

Expected: `src/kernel/include/kernel_config.h` updated with timer macros.

- [ ] **Step 3: Verify generated config**

Read `src/kernel/include/kernel_config.h` and confirm these macros exist:
- `TIMER_ENABLE`
- `TIMER_MAX`
- `TIMER_CMD_QUEUE_SIZE`
- `TIMER_TASK_PRIORITY`
- `TIMER_TASK_STACK_SIZE`
- `TIMER_NAME_LEN`

- [ ] **Step 4: Commit**

```bash
git add Kconfig src/kernel/include/kernel_config.h .config
git commit -m "feat(timer): add timer configuration options"
```

---

## Task 2: Update kernel_types.h for Timer

**Files:**
- Modify: `src/kernel/include/kernel_types.h`

- [ ] **Step 1: Update timer_t structure**

Replace the existing timer_t definition (lines 181-194) with:

```c
/*============================================================================
 * 软件定时器
 *============================================================================*/

/**
 * @brief 定时器状态
 */
typedef enum {
    TIMER_STATE_IDLE = 0,      // 空闲（未启动）
    TIMER_STATE_ACTIVE,        // 活跃（在堆中）
    TIMER_STATE_RUNNING,       // 回调执行中
    TIMER_STATE_DELETED        // 已删除
} timer_state_t;

/**
 * @brief 定时器命令类型
 */
typedef enum {
    TIMER_CMD_START,           // 启动定时器
    TIMER_CMD_STOP,            // 停止定时器
    TIMER_CMD_RESET,           // 重置定时器
    TIMER_CMD_CHANGE_PERIOD,   // 修改周期
    TIMER_CMD_DELETE           // 删除定时器
} timer_cmd_type_t;

/**
 * @brief 定时器回调函数类型
 */
typedef void (*timer_callback_t)(void *arg);

/**
 * @brief 定时器控制块
 */
typedef struct {
    // --- 基本信息 ---
    char            name[KERN_TASK_NAME_LEN];  // 定时器名称
    timer_id_t      id;                         // 定时器 ID
    timer_state_t   state;                      // 当前状态

    // --- 时间参数 ---
    uint32_t        period;                     // 周期（ticks），0 表示单次
    uint32_t        expire;                     // 到期时间（ticks）

    // --- 回调信息 ---
    timer_callback_t callback;                  // 回调函数
    void           *arg;                        // 回调参数

    // --- 堆索引 ---
    int             heap_index;                 // 在最小堆中的索引，-1 表示不在堆中

    // --- 标志 ---
    uint8_t         one_shot;                   // 单次触发标志
    uint8_t         in_use;                     // 使用标志
} timer_t;

/**
 * @brief 定时器命令消息
 */
typedef struct {
    timer_cmd_type_t    type;       // 命令类型
    timer_id_t          timer_id;   // 目标定时器
    uint32_t            param;      // 参数（周期、延迟等）
} timer_cmd_t;
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/include/kernel_types.h
git commit -m "feat(timer): update timer_t structure with heap support"
```

---

## Task 3: Create timer.h Interface

**Files:**
- Create: `src/kernel/timer/timer.h`

- [ ] **Step 1: Create timer.h**

```c
/**
 * @file timer.h
 * @brief 软件定时器接口
 *
 * ============================================================================
 * 模块概述
 * ============================================================================
 *
 * 软件定时器提供灵活的定时触发机制：
 *
 * 1. 单次定时器
 *    - 触发一次后自动停止
 *    - 适用于延迟执行、超时检测
 *
 * 2. 周期定时器
 *    - 按固定周期重复触发
 *    - 适用于周期性任务（采样、心跳等）
 *
 * 3. 线程安全
 *    - 使用命令队列，任意上下文可调用 API
 *    - 回调在任务上下文执行，可调用阻塞 API
 *
 * ============================================================================
 * 使用方法
 * ============================================================================
 *
 * // 创建周期定时器
 * void my_callback(void *arg) {
 *     // 定时器回调，在任务上下文执行
 * }
 *
 * timer_id_t tid = timer_create("my_timer", my_callback, NULL, 100);
 * timer_start(tid, 0);  // 立即开始
 *
 * // 停止并删除
 * timer_stop(tid);
 * timer_delete(tid);
 *
 * ============================================================================
 */

#ifndef TIMER_H
#define TIMER_H

#include "kernel_types.h"

/*============================================================================
 * 定时器创建和删除
 *============================================================================*/

/**
 * @brief 创建定时器
 *
 * @param name     定时器名称
 * @param callback 回调函数（在任务上下文执行）
 * @param arg      回调参数
 * @param period   周期（ticks），0 表示单次触发
 *
 * @return 定时器 ID，失败返回 KERN_INVALID_ID
 *
 * @note 回调在定时器服务任务上下文执行，可以调用阻塞 API
 * @note 创建后定时器处于停止状态，需要调用 timer_start() 启动
 */
timer_id_t timer_create(const char *name, timer_callback_t callback,
                        void *arg, uint32_t period);

/**
 * @brief 删除定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 如果定时器正在执行回调，会等待回调完成
 */
kern_err_t timer_delete(timer_id_t timer_id);

/*============================================================================
 * 定时器控制
 *============================================================================*/

/**
 * @brief 启动定时器
 *
 * @param timer_id 定时器 ID
 * @param delay    首次触发的延迟（ticks），0 表示立即开始
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 此函数发送命令到队列，立即返回
 * @note 可以在中断中安全调用
 */
kern_err_t timer_start(timer_id_t timer_id, uint32_t delay);

/**
 * @brief 停止定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 定时器停止后可以从堆中移除，不再触发回调
 */
kern_err_t timer_stop(timer_id_t timer_id);

/**
 * @brief 重置定时器
 *
 * @param timer_id 定时器 ID
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 重新开始计时，相当于 stop + start
 */
kern_err_t timer_reset(timer_id_t timer_id);

/**
 * @brief 修改定时器周期
 *
 * @param timer_id   定时器 ID
 * @param new_period 新周期（ticks）
 *
 * @return KERN_OK 成功，其他失败
 *
 * @note 修改周期后定时器会重新开始计时
 */
kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period);

/*============================================================================
 * 定时器状态查询
 *============================================================================*/

/**
 * @brief 获取定时器状态
 *
 * @param timer_id 定时器 ID
 *
 * @return 定时器状态
 */
timer_state_t timer_get_state(timer_id_t timer_id);

/**
 * @brief 获取定时器剩余时间
 *
 * @param timer_id 定时器 ID
 *
 * @return 剩余 ticks，-1 表示失败或定时器未启动
 */
int32_t timer_get_remaining(timer_id_t timer_id);

/**
 * @brief 检查定时器是否正在运行
 *
 * @param timer_id 定时器 ID
 *
 * @return 1 正在运行，0 未运行或失败
 */
int timer_is_active(timer_id_t timer_id);

/*============================================================================
 * 初始化
 *============================================================================*/

/**
 * @brief 初始化定时器模块
 *
 * @note 由 kern_init() 内部调用
 */
void timer_init(void);

/**
 * @brief 启动定时器服务
 *
 * @note 由 kern_start() 内部调用
 */
void timer_service_start(void);

#endif /* TIMER_H */
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/timer/timer.h
git commit -m "feat(timer): add timer interface header"
```

---

## Task 4: Implement Min-Heap Operations

**Files:**
- Create: `src/kernel/timer/timer.c`

- [ ] **Step 1: Create timer.c with heap operations**

```c
/**
 * @file timer.c
 * @brief 软件定时器实现
 *
 * 使用最小堆管理活跃定时器，命令队列实现线程安全操作。
 */

#include "timer.h"
#include "scheduler.h"
#include "mqueue.h"
#include "task.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 配置兼容性
 *============================================================================*/

#ifndef TIMER_ENABLE
#define TIMER_ENABLE 0
#endif

#ifndef TIMER_MAX
#define TIMER_MAX 16
#endif

#ifndef TIMER_CMD_QUEUE_SIZE
#define TIMER_CMD_QUEUE_SIZE 8
#endif

#ifndef TIMER_TASK_PRIORITY
#define TIMER_TASK_PRIORITY 1
#endif

#ifndef TIMER_TASK_STACK_SIZE
#define TIMER_TASK_STACK_SIZE 512
#endif

#ifndef TIMER_NAME_LEN
#define TIMER_NAME_LEN 16
#endif

/* 兼容性别名 */
#define KERN_TIMER_MAX          TIMER_MAX
#define KERN_TIMER_CMD_QUEUE    TIMER_CMD_QUEUE_SIZE
#define KERN_TIMER_TASK_PRIO    TIMER_TASK_PRIORITY
#define KERN_TIMER_STACK_SIZE   TIMER_TASK_STACK_SIZE
#define KERN_TIMER_NAME_LEN     TIMER_NAME_LEN

/*============================================================================
 * 静态分配
 *============================================================================*/

#if TIMER_ENABLE

/* 定时器池 */
static timer_t timer_pool[KERN_TIMER_MAX];
static uint32_t timer_used_bitmap;

/* 最小堆 */
typedef struct {
    timer_t    *timers[KERN_TIMER_MAX];
    int         size;
} timer_heap_t;

static timer_heap_t timer_heap;

/* 命令队列 */
static queue_id_t cmd_queue;

/* 定时器服务任务 */
static task_id_t timer_task_id;

#endif /* TIMER_ENABLE */

/*============================================================================
 * 最小堆操作
 *============================================================================*/

#if TIMER_ENABLE

/**
 * @brief 获取堆顶定时器（expire 最小）
 */
static inline timer_t *heap_peek(timer_heap_t *heap) {
    return (heap->size > 0) ? heap->timers[0] : NULL;
}

/**
 * @brief 插入定时器到堆
 * @return 0 成功，-1 堆已满
 */
static int heap_insert(timer_heap_t *heap, timer_t *timer) {
    if (heap->size >= KERN_TIMER_MAX) {
        return -1;
    }

    /* 插入到末尾 */
    int i = heap->size++;
    heap->timers[i] = timer;
    timer->heap_index = i;

    /* 上浮调整 */
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (heap->timers[parent]->expire <= timer->expire) {
            break;
        }
        /* 交换 */
        heap->timers[i] = heap->timers[parent];
        heap->timers[i]->heap_index = i;
        heap->timers[parent] = timer;
        timer->heap_index = parent;
        i = parent;
    }

    return 0;
}

/**
 * @brief 弹出堆顶定时器
 */
static timer_t *heap_pop(timer_heap_t *heap) {
    if (heap->size == 0) {
        return NULL;
    }

    timer_t *min = heap->timers[0];
    min->heap_index = -1;

    /* 用最后一个元素替换根 */
    timer_t *last = heap->timers[--heap->size];
    if (heap->size > 0) {
        heap->timers[0] = last;
        last->heap_index = 0;

        /* 下沉调整 */
        int i = 0;
        while (1) {
            int smallest = i;
            int left = 2 * i + 1;
            int right = 2 * i + 2;

            if (left < heap->size &&
                heap->timers[left]->expire < heap->timers[smallest]->expire) {
                smallest = left;
            }
            if (right < heap->size &&
                heap->timers[right]->expire < heap->timers[smallest]->expire) {
                smallest = right;
            }

            if (smallest == i) {
                break;
            }

            heap->timers[i] = heap->timers[smallest];
            heap->timers[i]->heap_index = i;
            heap->timers[smallest] = last;
            last->heap_index = smallest;
            i = smallest;
        }
    }

    return min;
}

/**
 * @brief 从堆中移除指定定时器
 */
static void heap_remove(timer_heap_t *heap, timer_t *timer) {
    int i = timer->heap_index;
    if (i < 0 || i >= heap->size) {
        return;
    }

    timer->heap_index = -1;

    /* 用最后一个元素替换 */
    timer_t *last = heap->timers[--heap->size];
    if (heap->size > 0 && i < heap->size) {
        heap->timers[i] = last;
        last->heap_index = i;

        /* 可能需要上浮或下沉 */
        int parent = (i - 1) / 2;
        if (i > 0 && heap->timers[parent]->expire > last->expire) {
            /* 上浮 */
            while (i > 0) {
                parent = (i - 1) / 2;
                if (heap->timers[parent]->expire <= last->expire) {
                    break;
                }
                heap->timers[i] = heap->timers[parent];
                heap->timers[i]->heap_index = i;
                heap->timers[parent] = last;
                last->heap_index = parent;
                i = parent;
            }
        } else {
            /* 下沉 */
            while (1) {
                int smallest = i;
                int left = 2 * i + 1;
                int right = 2 * i + 2;

                if (left < heap->size &&
                    heap->timers[left]->expire < heap->timers[smallest]->expire) {
                    smallest = left;
                }
                if (right < heap->size &&
                    heap->timers[right]->expire < heap->timers[smallest]->expire) {
                    smallest = right;
                }

                if (smallest == i) {
                    break;
                }

                heap->timers[i] = heap->timers[smallest];
                heap->timers[i]->heap_index = i;
                heap->timers[smallest] = last;
                last->heap_index = smallest;
                i = smallest;
            }
        }
    }
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 定时器池管理
 *============================================================================*/

#if TIMER_ENABLE

static timer_id_t alloc_timer_id(void) {
    for (int i = 0; i < KERN_TIMER_MAX; i++) {
        if (!(timer_used_bitmap & (1U << i))) {
            timer_used_bitmap |= (1U << i);
            return (timer_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void free_timer_id(timer_id_t id) {
    if (id >= 0 && id < KERN_TIMER_MAX) {
        timer_used_bitmap &= ~(1U << id);
    }
}

static timer_t *timer_get(timer_id_t id) {
    if (id < 0 || id >= KERN_TIMER_MAX) {
        return NULL;
    }
    if (!timer_pool[id].in_use) {
        return NULL;
    }
    return &timer_pool[id];
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 命令发送
 *============================================================================*/

#if TIMER_ENABLE

static kern_err_t send_command(timer_cmd_type_t type, timer_id_t timer_id, uint32_t param) {
    timer_cmd_t cmd;
    cmd.type = type;
    cmd.timer_id = timer_id;
    cmd.param = param;

    /* 使用非阻塞发送，避免在中断中阻塞 */
    return mqueue_trysend(cmd_queue, &cmd);
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 命令处理（在服务任务中执行）
 *============================================================================*/

#if TIMER_ENABLE

static void process_cmd_start(timer_id_t timer_id, uint32_t delay) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return;
    }

    /* 如果已在堆中，先移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    /* 设置到期时间 */
    uint32_t now = sched_get_tick_count();
    timer->expire = now + (delay > 0 ? delay : timer->period);
    timer->state = TIMER_STATE_ACTIVE;

    /* 插入堆 */
    heap_insert(&timer_heap, timer);
}

static void process_cmd_stop(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return;
    }

    /* 从堆中移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    timer->state = TIMER_STATE_IDLE;
}

static void process_cmd_reset(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return;
    }

    /* 重新开始计时 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    uint32_t now = sched_get_tick_count();
    timer->expire = now + timer->period;
    timer->state = TIMER_STATE_ACTIVE;
    heap_insert(&timer_heap, timer);
}

static void process_cmd_change_period(timer_id_t timer_id, uint32_t new_period) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return;
    }

    timer->period = new_period;
    timer->one_shot = (new_period == 0) ? 1 : 0;

    /* 如果正在运行，重新插入 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
        uint32_t now = sched_get_tick_count();
        timer->expire = now + new_period;
        heap_insert(&timer_heap, timer);
    }
}

static void process_cmd_delete(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return;
    }

    /* 从堆中移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    /* 清零并释放 */
    memset(timer, 0, sizeof(timer_t));
    free_timer_id(timer_id);
}

static void process_command(timer_cmd_t *cmd) {
    switch (cmd->type) {
        case TIMER_CMD_START:
            process_cmd_start(cmd->timer_id, cmd->param);
            break;
        case TIMER_CMD_STOP:
            process_cmd_stop(cmd->timer_id);
            break;
        case TIMER_CMD_RESET:
            process_cmd_reset(cmd->timer_id);
            break;
        case TIMER_CMD_CHANGE_PERIOD:
            process_cmd_change_period(cmd->timer_id, cmd->param);
            break;
        case TIMER_CMD_DELETE:
            process_cmd_delete(cmd->timer_id);
            break;
    }
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 定时器到期处理
 *============================================================================*/

#if TIMER_ENABLE

static void process_expired_timers(void) {
    uint32_t now = sched_get_tick_count();

    while (timer_heap.size > 0) {
        timer_t *timer = timer_heap.timers[0];

        if (timer->expire > now) {
            break;  /* 最近的定时器还未到期 */
        }

        /* 从堆中移除 */
        heap_pop(&timer_heap);
        timer->state = TIMER_STATE_RUNNING;

        /* 执行回调 */
        if (timer->callback) {
            timer->callback(timer->arg);
        }

        /* 周期定时器重新插入 */
        if (!timer->one_shot && timer->state == TIMER_STATE_RUNNING) {
            timer->expire = now + timer->period;
            timer->state = TIMER_STATE_ACTIVE;
            heap_insert(&timer_heap, timer);
        } else if (timer->one_shot) {
            timer->state = TIMER_STATE_IDLE;
        }
    }
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 定时器服务任务
 *============================================================================*/

#if TIMER_ENABLE

static void timer_service_task(void *arg) {
    (void)arg;
    timer_cmd_t cmd;
    uint32_t timeout;

    while (1) {
        /* 计算等待时间 */
        if (timer_heap.size > 0) {
            uint32_t now = sched_get_tick_count();
            uint32_t expire = timer_heap.timers[0]->expire;
            if (expire > now) {
                timeout = expire - now;
            } else {
                timeout = 0;  /* 已到期 */
            }
        } else {
            timeout = 0xFFFFFFFF;  /* 无限等待 */
        }

        /* 等待命令或超时 */
        kern_err_t err = mqueue_recv(cmd_queue, &cmd, timeout);

        if (err == KERN_OK) {
            process_command(&cmd);
        } else if (err == KERN_ERR_TIMEOUT) {
            process_expired_timers();
        }
    }
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 公开接口实现
 *============================================================================*/

timer_id_t timer_create(const char *name, timer_callback_t callback,
                        void *arg, uint32_t period) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();

    timer_id_t id = alloc_timer_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    timer_t *timer = &timer_pool[id];
    memset(timer, 0, sizeof(timer_t));

    timer->id = id;
    timer->callback = callback;
    timer->arg = arg;
    timer->period = period;
    timer->one_shot = (period == 0) ? 1 : 0;
    timer->state = TIMER_STATE_IDLE;
    timer->heap_index = -1;
    timer->in_use = 1;

    if (name) {
        strncpy(timer->name, name, KERN_TIMER_NAME_LEN - 1);
        timer->name[KERN_TIMER_NAME_LEN - 1] = '\0';
    }

    hal_irq_restore(crit);
    return id;
#else
    (void)name;
    (void)callback;
    (void)arg;
    (void)period;
    return KERN_INVALID_ID;
#endif
}

kern_err_t timer_delete(timer_id_t timer_id) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return KERN_ERR_PARAM;
    }

    return send_command(TIMER_CMD_DELETE, timer_id, 0);
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_start(timer_id_t timer_id, uint32_t delay) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return KERN_ERR_PARAM;
    }

    return send_command(TIMER_CMD_START, timer_id, delay);
#else
    (void)timer_id;
    (void)delay;
    return KERN_ERR;
#endif
}

kern_err_t timer_stop(timer_id_t timer_id) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return KERN_ERR_PARAM;
    }

    return send_command(TIMER_CMD_STOP, timer_id, 0);
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_reset(timer_id_t timer_id) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return KERN_ERR_PARAM;
    }

    return send_command(TIMER_CMD_RESET, timer_id, 0);
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        return KERN_ERR_PARAM;
    }

    return send_command(TIMER_CMD_CHANGE_PERIOD, timer_id, new_period);
#else
    (void)timer_id;
    (void)new_period;
    return KERN_ERR;
#endif
}

timer_state_t timer_get_state(timer_id_t timer_id) {
#if TIMER_ENABLE
    timer_t *timer = timer_get(timer_id);
    return timer ? timer->state : TIMER_STATE_DELETED;
#else
    (void)timer_id;
    return TIMER_STATE_DELETED;
#endif
}

int32_t timer_get_remaining(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();

    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->heap_index < 0) {
        hal_irq_restore(crit);
        return -1;
    }

    uint32_t now = sched_get_tick_count();
    int32_t remaining = (timer->expire > now) ? (int32_t)(timer->expire - now) : 0;

    hal_irq_restore(crit);
    return remaining;
#else
    (void)timer_id;
    return -1;
#endif
}

int timer_is_active(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();

    timer_t *timer = timer_get(timer_id);
    int active = (timer && timer->heap_index >= 0) ? 1 : 0;

    hal_irq_restore(crit);
    return active;
#else
    (void)timer_id;
    return 0;
#endif
}

/*============================================================================
 * 初始化
 *============================================================================*/

void timer_init(void) {
#if TIMER_ENABLE
    memset(timer_pool, 0, sizeof(timer_pool));
    timer_used_bitmap = 0;

    timer_heap.size = 0;

    /* 创建命令队列 */
    cmd_queue = mqueue_create(sizeof(timer_cmd_t), KERN_TIMER_CMD_QUEUE);
#endif
}

void timer_service_start(void) {
#if TIMER_ENABLE
    /* 创建定时器服务任务 */
    timer_task_id = task_create("timer_svc", timer_service_task, NULL,
                                 KERN_TIMER_TASK_PRIO, KERN_TIMER_STACK_SIZE);
    if (timer_task_id >= 0) {
        task_start(timer_task_id);
    }
#endif
}
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/timer/timer.c
git commit -m "feat(timer): implement timer module with min-heap and command queue"
```

---

## Task 5: Integrate Timer into Kernel

**Files:**
- Modify: `src/kernel/kernel.c`
- Modify: `src/kernel/include/kernel.h`

- [ ] **Step 1: Add timer include to kernel.c**

Add at line 11 (after `#include "board_config.h"`):

```c
#include "timer.h"
```

- [ ] **Step 2: Add timer_init() to kern_init()**

In `kern_init()` function, add after `ipc_init();` (line 26):

```c
    timer_init();
```

- [ ] **Step 3: Add timer_service_start() to kern_start()**

In `kern_start()` function, add before `sched_start();` (line 41):

```c
    // 启动定时器服务
    timer_service_start();
```

- [ ] **Step 4: Check kernel.h for any needed updates**

Read `/home/five/my-rtos/src/kernel/include/kernel.h` and verify no changes needed.

- [ ] **Step 5: Commit**

```bash
git add src/kernel/kernel.c
git commit -m "feat(timer): integrate timer module into kernel init/start"
```

---

## Task 6: Update Makefile

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add timer source to Makefile**

Find the kernel source files section and add timer.c:

```makefile
# Kernel sources
KERNEL_SRCS = \
    src/kernel/kernel.c \
    src/kernel/core/scheduler.c \
    src/kernel/task/task.c \
    src/kernel/mem/mem.c \
    src/kernel/mem/mempool.c \
    src/kernel/lib/kstring.c \
    src/kernel/ipc/semaphore.c \
    src/kernel/ipc/mutex.c \
    src/kernel/ipc/mqueue.c \
    src/kernel/ipc/event.c \
    src/kernel/timer/timer.c \
    src/kernel/system_init.c
```

- [ ] **Step 2: Commit**

```bash
git add Makefile
git commit -m "feat(timer): add timer.c to build"
```

---

## Task 7: Build and Verify

**Files:**
- None (build verification)

- [ ] **Step 1: Build the project**

```bash
cd /home/five/my-rtos
make clean
make
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Check for warnings**

If there are warnings, fix them before proceeding.

- [ ] **Step 3: Note the binary size**

```
Flash: [size] bytes
SRAM: [size] bytes
```

---

## Task 8: Create Timer Test Module

**Files:**
- Create: `src/tests/test_timer.c`

- [ ] **Step 1: Create test_timer.c**

```c
/**
 * @file test_timer.c
 * @brief 软件定时器测试模块
 */

#include "test_framework.h"
#include "timer.h"
#include "task.h"
#include "scheduler.h"
#include "hal.h"

/*============================================================================
 * 测试数据
 *============================================================================*/

static volatile int test_flag = 0;
static volatile int test_count = 0;

/*============================================================================
 * 测试回调函数
 *============================================================================*/

static void callback_set_flag(void *arg) {
    int *flag = (int *)arg;
    *flag = 1;
}

static void callback_increment(void *arg) {
    int *count = (int *)arg;
    (*count)++;
}

/*============================================================================
 * 测试用例
 *============================================================================*/

/**
 * @brief 测试 1：单次定时器
 */
static void test_one_shot_timer(void) {
    test_flag = 0;

    timer_id_t tid = timer_create("test1", callback_set_flag, (void *)&test_flag, 0);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    kern_err_t err = timer_start(tid, 10);
    TEST_ASSERT(err == KERN_OK, "Timer start failed");

    /* 等待定时器触发 */
    task_delay(15);

    TEST_ASSERT(test_flag == 1, "One-shot timer did not fire");

    timer_delete(tid);
    TEST_PASS("One-shot timer");
}

/**
 * @brief 测试 2：周期定时器
 */
static void test_periodic_timer(void) {
    test_count = 0;

    timer_id_t tid = timer_create("test2", callback_increment, (void *)&test_count, 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    kern_err_t err = timer_start(tid, 0);
    TEST_ASSERT(err == KERN_OK, "Timer start failed");

    /* 等待多个周期 */
    task_delay(25);

    TEST_ASSERT(test_count >= 4, "Periodic timer did not fire enough times");

    timer_stop(tid);
    timer_delete(tid);
    TEST_PASS("Periodic timer");
}

/**
 * @brief 测试 3：定时器停止
 */
static void test_timer_stop(void) {
    test_count = 0;

    timer_id_t tid = timer_create("test3", callback_increment, (void *)&test_count, 5);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);
    task_delay(12);

    int count_before = test_count;
    timer_stop(tid);

    task_delay(20);

    TEST_ASSERT(test_count == count_before, "Timer still firing after stop");

    timer_delete(tid);
    TEST_PASS("Timer stop");
}

/**
 * @brief 测试 4：定时器重置
 */
static void test_timer_reset(void) {
    test_flag = 0;

    timer_id_t tid = timer_create("test4", callback_set_flag, (void *)&test_flag, 0);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 20);

    /* 在触发前重置 */
    task_delay(10);
    timer_reset(tid);

    /* 原本应该在 20 ticks 触发，但重置后要等更久 */
    task_delay(15);
    TEST_ASSERT(test_flag == 0, "Timer fired too early after reset");

    /* 等待重置后的触发 */
    task_delay(10);
    TEST_ASSERT(test_flag == 1, "Timer did not fire after reset");

    timer_delete(tid);
    TEST_PASS("Timer reset");
}

/**
 * @brief 测试 5：修改周期
 */
static void test_timer_change_period(void) {
    test_count = 0;

    timer_id_t tid = timer_create("test5", callback_increment, (void *)&test_count, 10);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_start(tid, 0);

    /* 等待几次触发 */
    task_delay(25);
    int count1 = test_count;

    /* 修改为更短的周期 */
    timer_change_period(tid, 3);
    task_delay(20);
    int count2 = test_count;

    TEST_ASSERT(count2 > count1 + 3, "Period change did not take effect");

    timer_stop(tid);
    timer_delete(tid);
    TEST_PASS("Change period");
}

/**
 * @brief 测试 6：多定时器并发
 */
static void test_multiple_timers(void) {
    #define MULTI_COUNT 4
    static int flags[MULTI_COUNT] = {0};

    timer_id_t timers[MULTI_COUNT];

    for (int i = 0; i < MULTI_COUNT; i++) {
        flags[i] = 0;
        timers[i] = timer_create("multi", callback_set_flag, &flags[i], 0);
        timer_start(timers[i], 5 + i * 5);
    }

    task_delay(30);

    int fired = 0;
    for (int i = 0; i < MULTI_COUNT; i++) {
        if (flags[i]) fired++;
        timer_delete(timers[i]);
    }

    TEST_ASSERT(fired >= 3, "Not enough timers fired");
    TEST_PASS("Multiple timers");
}

/**
 * @brief 测试 7：定时器状态查询
 */
static void test_timer_state(void) {
    timer_id_t tid = timer_create("state", callback_set_flag, (void *)&test_flag, 10);
    TEST_ASSERT(tid >= 0, "Timer create failed");

    timer_state_t state = timer_get_state(tid);
    TEST_ASSERT(state == TIMER_STATE_IDLE, "Timer should be idle after create");

    timer_start(tid, 0);
    state = timer_get_state(tid);
    /* 状态可能是 ACTIVE 或 RUNNING，取决于时机 */

    int active = timer_is_active(tid);
    TEST_ASSERT(active == 1, "Timer should be active");

    timer_stop(tid);
    active = timer_is_active(tid);
    TEST_ASSERT(active == 0, "Timer should not be active after stop");

    timer_delete(tid);
    TEST_PASS("Timer state");
}

/*============================================================================
 * 测试模块注册
 *============================================================================*/

TEST_MODULE_REGISTER(test_timer, "Timer Tests",
    test_one_shot_timer,
    test_periodic_timer,
    test_timer_stop,
    test_timer_reset,
    test_timer_change_period,
    test_multiple_timers,
    test_timer_state
);
```

- [ ] **Step 2: Update Kconfig to add timer test option**

Add to `Kconfig` in Test Configuration menu:

```kconfig
config TEST_MODULE_TIMER
    bool "Timer tests"
    depends on TEST_ENABLE && TIMER_ENABLE
    default y
    help
      启用定时器测试模块。
```

- [ ] **Step 3: Update Makefile to include test_timer.c**

Add to test sources in Makefile:

```makefile
TEST_SRCS = \
    src/tests/test_framework.c \
    src/tests/test_scheduler.c \
    src/tests/test_timer.c
```

- [ ] **Step 4: Regenerate config**

```bash
python scripts/menuconfig.py genconfig
```

- [ ] **Step 5: Commit**

```bash
git add src/tests/test_timer.c Kconfig Makefile
git commit -m "feat(timer): add timer test module"
```

---

## Task 9: Final Build and Test

**Files:**
- None (verification)

- [ ] **Step 1: Clean build**

```bash
cd /home/five/my-rtos
make clean
make
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Flash to board and run tests**

```bash
make flash
```

Expected: All timer tests pass.

- [ ] **Step 3: Final commit if needed**

If any fixes were made:

```bash
git add -A
git commit -m "fix(timer): fix build/test issues"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Add Kconfig options | Kconfig |
| 2 | Update kernel_types.h | kernel_types.h |
| 3 | Create timer.h | timer.h |
| 4 | Implement timer.c | timer.c |
| 5 | Integrate into kernel | kernel.c |
| 6 | Update Makefile | Makefile |
| 7 | Build verification | - |
| 8 | Create test module | test_timer.c |
| 9 | Final test | - |
