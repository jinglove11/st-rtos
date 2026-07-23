/**
 * @file timer.c
 * @brief 软件定时器实现
 *
 * 使用最小堆管理活跃定时器，命令队列实现线程安全操作。
 */

#include "timer.h"
#include "endpoint.h"
#include "scheduler.h"
#include "mqueue.h"
#include "task.h"
#include "kernel_config.h"
#include "trace.h"
#include "stats.h"
#include "hal.h"
#include "spinlock.h"
#include "capability.h"
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

#ifndef TRACE_TIMER_CREATE
#define TRACE_TIMER_CREATE      1
#define TRACE_TIMER_START       2
#define TRACE_TIMER_STOP        3
#define TRACE_TIMER_FIRE        4
#define TRACE_TIMER_DELETE      5
#define TRACE_TIMER_QUEUE_FULL  6
#define TRACE_TIMER_RESET       7
#define TRACE_TIMER_CHANGE      8
#endif

/*============================================================================
 * 静态分配
 *============================================================================*/

#if TIMER_ENABLE

/* 定时器池 */
static timer_t timer_pool[KERN_TIMER_MAX];
static uint32_t timer_used_bitmap;
static irq_spinlock_t timer_lock; /* M1: SMP safe */

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
 * Trace / Stats
 *============================================================================*/

#if TIMER_ENABLE

static uint8_t timer_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

#if TRACE_ENABLE
static uint8_t timer_trace_result(kern_err_t err) {
    switch (err) {
        case KERN_OK:
            return TRACE_RESULT_OK;
        case KERN_ERR_TIMEOUT:
            return TRACE_RESULT_TIMEOUT;
        case KERN_ERR_BUSY:
        case KERN_ERR_RESOURCE:
        case KERN_ERR_OVERFLOW:
            return TRACE_RESULT_FULL;
        case KERN_ERR_NOEXIST:
            return TRACE_RESULT_NOEXIST;
        default:
            return TRACE_RESULT_ERR;
    }
}
#endif

#if KERN_TASK_STATS
static uint8_t timer_stats_counter(kern_err_t err) {
    switch (err) {
        case KERN_OK:
            return STATS_COUNTER_OK;
        case KERN_ERR_TIMEOUT:
            return STATS_COUNTER_TIMEOUT;
        case KERN_ERR_BUSY:
        case KERN_ERR_RESOURCE:
        case KERN_ERR_OVERFLOW:
            return STATS_COUNTER_QUEUE_FULL;
        case KERN_ERR_NOEXIST:
            return STATS_COUNTER_NOEXIST;
        default:
            return STATS_COUNTER_ERROR;
    }
}
#endif

static void timer_record_event(timer_id_t timer_id, uint8_t action, kern_err_t err) {
#if TRACE_ENABLE
    uint8_t object_id = (timer_id >= 0) ? (uint8_t)timer_id : 0xFFU;
    trace_timer(timer_current_task_id(), object_id, action, timer_trace_result(err));
#else
    (void)timer_id;
    (void)action;
#endif

#if KERN_TASK_STATS
    (void)stats_record_event(STATS_SUBSYS_TIMER, timer_stats_counter(err));
#endif
    (void)err;
}

static uint8_t timer_cmd_trace_action(timer_cmd_type_t type) {
    switch (type) {
        case TIMER_CMD_START:
            return TRACE_TIMER_START;
        case TIMER_CMD_STOP:
            return TRACE_TIMER_STOP;
        case TIMER_CMD_RESET:
            return TRACE_TIMER_RESET;
        case TIMER_CMD_CHANGE_PERIOD:
            return TRACE_TIMER_CHANGE;
        case TIMER_CMD_DELETE:
            return TRACE_TIMER_DELETE;
        default:
            return TRACE_TIMER_QUEUE_FULL;
    }
}

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
        if (!(timer_used_bitmap & (1U << i)) &&
            !kobj_generation_is_retired(timer_pool[i].hdr.generation)) {
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

    /* 非阻塞发送并重试，给服务任务排出队列的机会 */
    kern_err_t err;
    for (int retry = 0; retry < 3; retry++) {
        err = mqueue_trysend(cmd_queue, &cmd);
        if (err == KERN_OK) {
            timer_record_event(timer_id, timer_cmd_trace_action(type), KERN_OK);
            return KERN_OK;
        }
    }
    timer_record_event(timer_id, TRACE_TIMER_QUEUE_FULL, err);
    return err;
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 命令处理（在服务任务中执行）
 *============================================================================*/

#if TIMER_ENABLE

static void process_cmd_start(timer_id_t timer_id, uint32_t delay) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        timer_record_event(timer_id, TRACE_TIMER_START, KERN_ERR_NOEXIST);
        return;
    }
    if (timer->delete_pending) {
        timer_record_event(timer_id, TRACE_TIMER_START, KERN_ERR_NOEXIST);
        return;
    }

    /* 如果已在堆中，先移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    /* 设置到期时间 */
    uint32_t now = sched_get_tick_count();
    uint32_t actual_delay = (delay > 0) ? delay : timer->period;
    timer->expire = now + actual_delay;

    /* 对于单次定时器，保存 delay 值以便 reset 使用 */
    if (timer->one_shot && delay > 0) {
        timer->period = delay;
    }

    /* 插入堆，成功后再设置状态 */
    if (heap_insert(&timer_heap, timer) == 0) {
        timer->state = TIMER_STATE_ACTIVE;
        timer->stop_pending = 0;
    } else {
        timer->state = TIMER_STATE_IDLE;
        timer_record_event(timer_id, TRACE_TIMER_START, KERN_ERR_RESOURCE);
    }
}

static void process_cmd_stop(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        timer_record_event(timer_id, TRACE_TIMER_STOP, KERN_ERR_NOEXIST);
        return;
    }
    if (timer->delete_pending) {
        timer_record_event(timer_id, TRACE_TIMER_STOP, KERN_ERR_NOEXIST);
        return;
    }

    /* 从堆中移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    timer->state = TIMER_STATE_IDLE;
    timer->stop_pending = 0;
}

static void process_cmd_reset(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        timer_record_event(timer_id, TRACE_TIMER_RESET, KERN_ERR_NOEXIST);
        return;
    }
    if (timer->delete_pending) {
        timer_record_event(timer_id, TRACE_TIMER_RESET, KERN_ERR_NOEXIST);
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
        timer->stop_pending = 0;
    } else {
        timer->state = TIMER_STATE_IDLE;
        timer_record_event(timer_id, TRACE_TIMER_RESET, KERN_ERR_RESOURCE);
    }
}

static void process_cmd_change_period(timer_id_t timer_id, uint32_t new_period) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        timer_record_event(timer_id, TRACE_TIMER_CHANGE, KERN_ERR_NOEXIST);
        return;
    }
    if (timer->delete_pending) {
        timer_record_event(timer_id, TRACE_TIMER_CHANGE, KERN_ERR_NOEXIST);
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
            timer->stop_pending = 0;
        } else {
            timer->state = TIMER_STATE_IDLE;
            timer_record_event(timer_id, TRACE_TIMER_CHANGE, KERN_ERR_RESOURCE);
        }
    }
}

static void process_cmd_delete(timer_id_t timer_id) {
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        timer_record_event(timer_id, TRACE_TIMER_DELETE, KERN_ERR_NOEXIST);
        return;
    }

    timer->delete_pending = 1;

    /* 从堆中移除 */
    if (timer->heap_index >= 0) {
        heap_remove(&timer_heap, timer);
    }

    /* 清零并释放 */
#if CAP_ENABLE
    /* M2-Step1+3a: 撤销所有任务持有的指向此 timer 的 cap。Step3a 改真指针。 */
    (void)cap_revoke_object(timer, CAP_OBJ_TIMER);
#endif
    /* M2-Step3a: bump generation 跨 memset 保留 */
    uint32_t next_gen = kobj_header_prepare_reuse(&timer->hdr);
    memset(timer, 0, sizeof(timer_t));
    timer->hdr.obj_type   = CAP_OBJ_TIMER;
    timer->hdr.generation = next_gen;
    timer->state = TIMER_STATE_DELETED;
    free_timer_id(timer_id);
}

/* M2-Step3a: cap 路径 id ↔ 对象指针 转换。 */
timer_id_t timer_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    timer_t *timer = (timer_t *)obj;
    timer_id_t id = (timer_id_t)(timer - timer_pool);
    if (id < 0 || id >= KERN_TIMER_MAX) return KERN_INVALID_ID;
    return id;
}

void *timer_obj_for_cap(timer_id_t id) {
    if (id < 0 || id >= KERN_TIMER_MAX) return NULL;
    return (void *)&timer_pool[id];
}

static void process_command(timer_cmd_t *cmd) {
    uint32_t crit = irq_spin_lock(&timer_lock);
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
    irq_spin_unlock(&timer_lock, crit);
}

#endif /* TIMER_ENABLE */

/*============================================================================
 * 定时器到期处理
 *============================================================================*/

#if TIMER_ENABLE

static void process_expired_timers(void) {
    uint32_t now = sched_get_tick_count();

    for (;;) {
        uint32_t crit = irq_spin_lock(&timer_lock);
        if (timer_heap.size == 0) {
            irq_spin_unlock(&timer_lock, crit);
            break;
        }
        timer_t *timer = timer_heap.timers[0];

        if ((int32_t)(timer->expire - now) > 0) {
            irq_spin_unlock(&timer_lock, crit);
            break;  /* 最近的定时器还未到期 */
        }

        /* 从堆中移除 */
        heap_pop(&timer_heap);
        timer->state = TIMER_STATE_RUNNING;

        uint8_t notify_bound = timer->notify_bound;
        ep_id_t notify_ep = timer->notify_ep;
        uint32_t notify_badge = timer->notify_badge;
        timer_id_t timer_id = timer->id;
        timer_callback_t callback = timer->callback;
        void *callback_arg = timer->arg;
        irq_spin_unlock(&timer_lock, crit);

        if (notify_bound) {
            uint8_t notify_msg[KERN_EP_MSG_SIZE];
            memset(notify_msg, 0, sizeof(notify_msg));
            ((uint32_t *)notify_msg)[0] = notify_badge;
            ((uint32_t *)notify_msg)[1] = (uint32_t)timer_id;
            (void)endpoint_notify(notify_ep, notify_msg);
        }

        /* 回调执行 (内核测试用,user 任务被 sys_timer_create 拒绝)。
         * Phase H2:timer_svc 保留内核特权 (需响应 SysTick + 回调),
         * 和 bh_svc/irq_N 同为内核 TCB 一部分。 */
        if (callback) {
            callback(callback_arg);
        }
        timer_record_event(timer_id, TRACE_TIMER_FIRE, KERN_OK);

        /* 周期定时器重新插入（除非回调中请求了停止） */
        crit = irq_spin_lock(&timer_lock);
        if (!timer->one_shot && !timer->stop_pending && !timer->delete_pending &&
            timer->state == TIMER_STATE_RUNNING) {
            timer->expire = now + timer->period;
            if (heap_insert(&timer_heap, timer) == 0) {
                timer->state = TIMER_STATE_ACTIVE;
            } else {
                timer->state = TIMER_STATE_IDLE;
            }
        } else {
            timer->state = TIMER_STATE_IDLE;
            timer->stop_pending = 0;
        }
        irq_spin_unlock(&timer_lock, crit);
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
        uint32_t crit = irq_spin_lock(&timer_lock);
        if (timer_heap.size > 0) {
            uint32_t now = sched_get_tick_count();
            int32_t diff = (int32_t)(timer_heap.timers[0]->expire - now);
            if (diff > 0) {
                timeout = (uint32_t)diff;
            } else {
                timeout = 0;  /* 已到期 */
            }
        } else {
            /*
             * 没有活动定时器时只需等待控制命令。旧的 100-tick 轮询会
             * 让 timer_svc 永久执行“超时唤醒 -> 重新入队”，既浪费调度
             * 开销，也扩大 Cortex-M 异常返回和 SMP wait-queue 的竞态窗口。
             * mqueue 发送命令本身会唤醒接收者，因此这里应真正无限等待。
             */
            timeout = KERN_WAIT_FOREVER;
        }
        irq_spin_unlock(&timer_lock, crit);

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
    uint32_t crit = irq_spin_lock(&timer_lock);

    timer_id_t id = alloc_timer_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(KERN_INVALID_ID, TRACE_TIMER_CREATE, KERN_ERR_RESOURCE);
        return KERN_INVALID_ID;
    }

    timer_t *timer = &timer_pool[id];
    /* M2-Step3a: 跨 memset 保留 generation (process_cmd_delete 已 bump)。
     * 首次分配 generation=0 → 初始化为 1; 复用时保留 bumped 值。 */
    uint32_t saved_gen = timer->hdr.generation;
    memset(timer, 0, sizeof(timer_t));
    kobj_header_init(&timer->hdr, CAP_OBJ_TIMER);
    if (saved_gen != 0) {
        timer->hdr.generation = saved_gen;
    }

    timer->id = id;
    timer->callback = callback;
    timer->arg = arg;
    timer->period = period;
    timer->one_shot = (period == 0) ? 1 : 0;
    timer->state = TIMER_STATE_IDLE;
    timer->heap_index = -1;
    timer->notify_ep = KERN_INVALID_ID;
    timer->in_use = 1;

    if (name) {
        strncpy(timer->name, name, KERN_TIMER_NAME_LEN - 1);
        timer->name[KERN_TIMER_NAME_LEN - 1] = '\0';
    }

    irq_spin_unlock(&timer_lock, crit);
    timer_record_event(id, TRACE_TIMER_CREATE, KERN_OK);
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
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_DELETE, KERN_ERR_PARAM);
        return KERN_ERR_PARAM;
    }
    if (timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_DELETE, KERN_ERR_NOEXIST);
        return KERN_ERR_NOEXIST;
    }
    timer->delete_pending = 1;
    irq_spin_unlock(&timer_lock, crit);

    kern_err_t err = send_command(TIMER_CMD_DELETE, timer_id, 0);

    return err;
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_start(timer_id_t timer_id, uint32_t delay) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_START, KERN_ERR_PARAM);
        return KERN_ERR_PARAM;
    }

    irq_spin_unlock(&timer_lock, crit);
    return send_command(TIMER_CMD_START, timer_id, delay);
#else
    (void)timer_id;
    (void)delay;
    return KERN_ERR;
#endif
}

kern_err_t timer_stop(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_STOP, KERN_ERR_PARAM);
        return KERN_ERR_PARAM;
    }

    timer->stop_pending = 1;
    irq_spin_unlock(&timer_lock, crit);
    return send_command(TIMER_CMD_STOP, timer_id, 0);
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_reset(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_RESET, KERN_ERR_PARAM);
        return KERN_ERR_PARAM;
    }

    irq_spin_unlock(&timer_lock, crit);
    return send_command(TIMER_CMD_RESET, timer_id, 0);
#else
    (void)timer_id;
    return KERN_ERR;
#endif
}

kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        timer_record_event(timer_id, TRACE_TIMER_CHANGE, KERN_ERR_PARAM);
        return KERN_ERR_PARAM;
    }

    irq_spin_unlock(&timer_lock, crit);
    return send_command(TIMER_CMD_CHANGE_PERIOD, timer_id, new_period);
#else
    (void)timer_id;
    (void)new_period;
    return KERN_ERR;
#endif
}

kern_err_t timer_bind_endpoint(timer_id_t timer_id, ep_id_t ep_id,
                               uint32_t badge) {
#if TIMER_ENABLE && IPC_ENDPOINT
    if (!endpoint_exists(ep_id)) {
        return KERN_ERR_PARAM;
    }
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->delete_pending) {
        irq_spin_unlock(&timer_lock, crit);
        return KERN_ERR_PARAM;
    }
    timer->notify_ep = ep_id;
    timer->notify_badge = badge;
    timer->notify_bound = 1;
    irq_spin_unlock(&timer_lock, crit);
    return KERN_OK;
#else
    (void)timer_id;
    (void)ep_id;
    (void)badge;
    return KERN_ERR;
#endif
}

timer_state_t timer_get_state(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);
    timer_t *timer = timer_get(timer_id);
    timer_state_t state = timer ? timer->state : TIMER_STATE_DELETED;
    irq_spin_unlock(&timer_lock, crit);
    return state;
#else
    (void)timer_id;
    return TIMER_STATE_DELETED;
#endif
}

int32_t timer_get_remaining(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);

    timer_t *timer = timer_get(timer_id);
    if (timer == NULL || timer->heap_index < 0) {
        irq_spin_unlock(&timer_lock, crit);
        return -1;
    }

    uint32_t now = sched_get_tick_count();
    int32_t diff = (int32_t)(timer->expire - now);
    int32_t remaining = (diff > 0) ? diff : 0;

    irq_spin_unlock(&timer_lock, crit);
    return remaining;
#else
    (void)timer_id;
    return -1;
#endif
}

int timer_is_active(timer_id_t timer_id) {
#if TIMER_ENABLE
    uint32_t crit = irq_spin_lock(&timer_lock);

    timer_t *timer = timer_get(timer_id);
    int active = (timer && timer->heap_index >= 0) ? 1 : 0;

    irq_spin_unlock(&timer_lock, crit);
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
    irq_spin_init_rank(&timer_lock, LOCKDEP_RANK_OBJECT);
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
