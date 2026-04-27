/**
 * @file task.c
 * @brief 任务管理实现
 */

#include "task.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "hal.h"
#include <string.h>

// 简单的数字转字符串
static void int_to_str(int n, char *buf) {
    int i = 0;
    if (n == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[12];
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

/*============================================================================
 * 静态分配的任务控制块和栈
 *============================================================================*/

// 任务控制块池
static tcb_t task_pool[KERN_MAX_TASKS];

// 任务栈池
static uint8_t task_stacks[KERN_MAX_TASKS][KERN_DEFAULT_STACK_SIZE]
    __attribute__((aligned(KERN_STACK_ALIGN)));

// 空闲任务栈
static uint8_t idle_stack[KERN_IDLE_STACK_SIZE]
    __attribute__((aligned(KERN_STACK_ALIGN)));

// 空闲任务 TCB
static tcb_t idle_task;

// 任务使用位图
static uint32_t task_used_bitmap = 0;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 前向声明
static void task_check_stack_overflow(void);

// 任务退出处理
static void task_exit_handler(void) {
    task_exit(NULL);
}

// 空闲任务函数
static void idle_task_func(void *arg) {
    (void)arg;
    uint32_t check_counter = 0;

    while (1) {
#if KERN_IDLE_SLEEP
        hal_enter_lowpower();
#endif

        // 定期检查栈溢出
        if (++check_counter >= 1000) {
            check_counter = 0;
            task_check_stack_overflow();
        }

#if KERN_WATCHDOG_ENABLE
        hal_watchdog_feed();
#endif
    }
}

// 分配任务 ID
static task_id_t alloc_task_id(void) {
    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (!(task_used_bitmap & (1U << i))) {
            task_used_bitmap |= (1U << i);
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放任务 ID
static void free_task_id(task_id_t id) {
    if (id >= 0 && id < KERN_MAX_TASKS) {
        task_used_bitmap &= ~(1U << id);
    }
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void task_init(void) {
    // 清零任务池
    memset(task_pool, 0, sizeof(task_pool));
    memset(task_stacks, 0, sizeof(task_stacks));

    task_used_bitmap = 0;

    // 初始化空闲任务
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.id = -1;  // 特殊 ID
    idle_task.priority = KERN_IDLE_PRIORITY;
    idle_task.base_priority = KERN_IDLE_PRIORITY;
    idle_task.state = TASK_STATE_CREATED;
    idle_task.stack_base = idle_stack;
    idle_task.stack_size = KERN_IDLE_STACK_SIZE;
    idle_task.time_slice = KERN_DEFAULT_TIME_SLICE;
    idle_task.time_slice_reload = KERN_DEFAULT_TIME_SLICE;
    strncpy(idle_task.name, "idle", KERN_TASK_NAME_LEN - 1);

    // 初始化空闲任务栈
    idle_task.sp = hal_stack_init(
        idle_stack + KERN_IDLE_STACK_SIZE,
        KERN_IDLE_STACK_SIZE,
        idle_task_func,
        NULL,
        task_exit_handler
    );
}

task_id_t task_create(const char   *name,
                      task_func_t  entry,
                      void        *arg,
                      uint8_t      priority,
                      uint32_t     stack_size)
{
    // 检查优先级范围
    if (priority >= KERN_MAX_PRIORITY) {
        return KERN_INVALID_ID;
    }

    // 分配任务 ID
    task_id_t id = alloc_task_id();
    if (id == KERN_INVALID_ID) {
        return KERN_INVALID_ID;
    }

    tcb_t *tcb = &task_pool[id];

    // 初始化 TCB
    memset(tcb, 0, sizeof(tcb_t));
    tcb->id = id;
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->state = TASK_STATE_CREATED;

    // 设置名称
    if (name) {
        strncpy(tcb->name, name, KERN_TASK_NAME_LEN - 1);
        tcb->name[KERN_TASK_NAME_LEN - 1] = '\0';
    } else {
        strcpy(tcb->name, "task");
        char num[12];
        int_to_str(id, num);
        strncat(tcb->name, num, KERN_TASK_NAME_LEN - 5);
    }

    // 设置栈
    if (stack_size == 0) {
        stack_size = KERN_DEFAULT_STACK_SIZE;
    }
    tcb->stack_base = task_stacks[id];
    tcb->stack_size = stack_size;

    // 设置时间片
    tcb->time_slice = KERN_DEFAULT_TIME_SLICE;
    tcb->time_slice_reload = KERN_DEFAULT_TIME_SLICE;

    // 初始化栈
    tcb->sp = hal_stack_init(
        (uint8_t *)tcb->stack_base + stack_size,
        stack_size,
        entry,
        arg,
        task_exit_handler
    );

    return id;
}

kern_err_t task_start(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];

    if (tcb->state != TASK_STATE_CREATED) {
        return KERN_ERR_STATE;
    }

    // 加入就绪队列
    sched_add_ready(tcb);

    return KERN_OK;
}

void task_exit(void *retval) {
    (void)retval;

    tcb_t *current = sched_get_current();
    if (current == NULL) {
        // 空闲任务退出, 不应该发生
        while (1);
    }

    // 设置为终止状态
    current->state = TASK_STATE_TERMINATED;

    // 释放任务 ID
    if (current->id >= 0 && current->id < KERN_MAX_TASKS) {
        free_task_id(current->id);
    }

    // TODO: 唤醒等待此任务的线程

    // 触发调度
    sched_yield();

    // 不应该到达这里
    while (1);
}

kern_err_t task_suspend(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = hal_enter_critical();

    if (tcb->state == TASK_STATE_TERMINATED) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    if (tcb->state == TASK_STATE_SUSPENDED) {
        hal_exit_critical(crit);
        return KERN_OK;
    }

    // 根据当前状态处理
    switch (tcb->state) {
        case TASK_STATE_RUNNING:
            // 当前任务挂起自己：设置状态后触发调度
            // PendSV 会检查状态，SUSPENDED 不会加入就绪队列
            tcb->state = TASK_STATE_SUSPENDED;
            hal_exit_critical(crit);
            sched_yield();  // 触发 PendSV
            break;

        case TASK_STATE_READY:
            // 从就绪队列移除
            sched_remove_ready(tcb);
            tcb->state = TASK_STATE_SUSPENDED;
            hal_exit_critical(crit);
            break;

        case TASK_STATE_BLOCKED:
            // 从阻塞状态转为挂起，清除阻塞信息
            tcb->state = TASK_STATE_SUSPENDED;
            tcb->wake_tick = 0;
            tcb->block_reason = BLOCK_REASON_NONE;
            tcb->block_obj = NULL;
            hal_exit_critical(crit);
            break;

        default:
            hal_exit_critical(crit);
            return KERN_ERR_STATE;
    }

    return KERN_OK;
}

kern_err_t task_resume(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = hal_enter_critical();

    if (tcb->state != TASK_STATE_SUSPENDED) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    // 加入就绪队列
    sched_add_ready(tcb);

    // 如果优先级高于当前任务，触发调度
    tcb_t *current = sched_get_current();
    if (current && tcb->priority < current->priority) {
        hal_trigger_pendsv();
    }

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t task_delete(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];

    // 不能删除当前任务
    if (tcb == sched_get_current()) {
        return KERN_ERR_STATE;
    }

    // 从就绪队列移除
    sched_remove_ready(tcb);

    // 清零 TCB
    memset(tcb, 0, sizeof(tcb_t));
    free_task_id(task_id);

    return KERN_OK;
}

task_id_t task_self(void) {
    tcb_t *current = sched_get_current();
    return current ? current->id : KERN_INVALID_ID;
}

kern_err_t task_yield(void) {
    sched_yield();
    return KERN_OK;
}

tcb_t *task_get_tcb(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return NULL;
    }

    tcb_t *tcb = &task_pool[task_id];
    return (tcb->state != TASK_STATE_TERMINATED && 
            (task_used_bitmap & (1U << task_id))) ? tcb : NULL;
}

task_id_t task_get_next(task_id_t task_id) {
    int start = (task_id < 0) ? 0 : task_id + 1;
    for (int i = start; i < KERN_MAX_TASKS; i++) {
        if (task_used_bitmap & (1U << i)) {
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

kern_err_t task_set_priority(task_id_t task_id, uint8_t priority) {
    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    if (priority >= KERN_MAX_PRIORITY) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];

    // 更新优先级
    tcb->priority = priority;
    tcb->base_priority = priority;

    // 如果在就绪队列, 需要重新插入
    if (tcb->state == TASK_STATE_READY) {
        sched_remove_ready(tcb);
        sched_add_ready(tcb);
    }

    return KERN_OK;
}

uint8_t task_get_priority(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->priority : 0xFF;
}

kern_err_t task_delay(uint32_t ticks) {
    if (ticks == 0) {
        return KERN_OK;
    }

    return sched_block(BLOCK_REASON_SLEEP, NULL, ticks);
}

kern_err_t task_delay_ms(uint32_t ms) {
    uint32_t ticks = (ms * 1000UL) / KERN_TICK_US;
    return task_delay(ticks);
}

kern_err_t task_delay_until(uint32_t tick) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    uint32_t now = hal_systick_get();
    if (tick <= now) {
        return KERN_OK;  // 已经过期
    }

    return task_delay(tick - now);
}

kern_err_t task_join(task_id_t task_id, void **retval, uint32_t timeout) {
    (void)retval;
    (void)timeout;

    if (task_id < 0 || task_id >= KERN_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];

    if (tcb->state == TASK_STATE_TERMINATED) {
        return KERN_OK;  // 任务已结束
    }

    // TODO: 实现等待任务结束
    return sched_block(BLOCK_REASON_JOIN, tcb, timeout);
}

const char *task_get_name(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->name : NULL;
}

task_state_t task_get_state(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->state : TASK_STATE_TERMINATED;
}

tcb_t *task_get_idle(void) {
    return &idle_task;
}

uint32_t task_get_used_bitmap(void) {
    return task_used_bitmap;
}

/*============================================================================
 * 任务回收 (由 PendSV 调用)
 *============================================================================*/

void task_reclaim(tcb_t *tcb) {
    if (tcb == NULL || tcb->id < 0) return;  // 空闲任务不回收

    task_id_t id = tcb->id;

    // 释放任务 ID
    free_task_id(id);

    // 清零 TCB (可选，调试时可以保留)
    // memset(tcb, 0, sizeof(tcb_t));

    // 栈空间标记空闲 (静态池，实际无需操作)
}

/*============================================================================
 * 栈溢出检测
 *============================================================================*/

void task_check_stack_overflow(void) {
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        if (!(task_used_bitmap & (1U << id))) continue;

        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_TERMINATED) continue;

        uint8_t *stack_base = (uint8_t *)tcb->stack_base;

        // 检查栈底魔数是否被破坏
        int overflow = 0;
        for (int i = 0; i < 16; i++) {
            if (stack_base[i] != STACK_MAGIC_BYTE) {
                overflow = 1;
                break;
            }
        }

        if (overflow) {
            hal_debug_puts("\r\n[STACK OVERFLOW] Task: ");
            hal_debug_puts(tcb->name);
            hal_debug_puts("\r\n");
        }
    }
}
