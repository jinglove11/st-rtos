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

    /* 插入堆，成功后再设置状态 */
    if (heap_insert(&timer_heap, timer) == 0) {
        timer->state = TIMER_STATE_ACTIVE;
    } else {
        timer->state = TIMER_STATE_IDLE;
    }
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

    /* 插入堆，成功后再设置状态 */
    if (heap_insert(&timer_heap, timer) == 0) {
        timer->state = TIMER_STATE_ACTIVE;
    } else {
        timer->state = TIMER_STATE_IDLE;
    }
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

        /* 插入堆，成功后再设置状态 */
        if (heap_insert(&timer_heap, timer) == 0) {
            timer->state = TIMER_STATE_ACTIVE;
        } else {
            timer->state = TIMER_STATE_IDLE;
        }
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
            /* 插入堆，成功后再设置状态 */
            if (heap_insert(&timer_heap, timer) == 0) {
                timer->state = TIMER_STATE_ACTIVE;
            } else {
                timer->state = TIMER_STATE_IDLE;
            }
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
    uint32_t crit = hal_irq_save();
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    kern_err_t err = send_command(TIMER_CMD_DELETE, timer_id, 0);
    hal_irq_restore(crit);
    return err;
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_start(timer_id_t timer_id, uint32_t delay) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    kern_err_t err = send_command(TIMER_CMD_START, timer_id, delay);
    hal_irq_restore(crit);
    return err;
#else
    (void)timer_id;
    (void)delay;
    return KERN_ERR;
#endif
}

kern_err_t timer_stop(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    kern_err_t err = send_command(TIMER_CMD_STOP, timer_id, 0);
    hal_irq_restore(crit);
    return err;
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_reset(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    kern_err_t err = send_command(TIMER_CMD_RESET, timer_id, 0);
    hal_irq_restore(crit);
    return err;
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period) {
#if TIMER_ENABLE
    uint32_t crit = hal_irq_save();
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    kern_err_t err = send_command(TIMER_CMD_CHANGE_PERIOD, timer_id, new_period);
    hal_irq_restore(crit);
    return err;
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