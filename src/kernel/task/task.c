/**
 * @file task.c
 * @brief 任务管理实现
 */

#include "task.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "hal.h"
#include "mpu.h"
#include <string.h>
#include "capability.h"
#include "syscall.h"
#include "wait_queue.h"
#include "endpoint.h"
#include "channel.h"
#include "vfs.h"
#include "mem.h"
#include "root_bootstrap.h"

/*
 * PendSV/SVC 汇编通过 tcb_offsets.inc (构建时由 scripts/gen_tcb_offsets.py
 * 从 kernel_types.h 自动生成) 间接拿到 tcb_t 字段偏移。这里仅放一个
 * "TCB 布局变化时让 C 编译先失败" 的兜底断言 — 真正的同步机制靠
 * gen_tcb_offsets.py 重新跑 offsetof()。详见 CMakeLists.txt 的 custom_command。
 */
typedef char tcb_state_offset_in_first_cacheline[(offsetof(tcb_t, state) < 64) ? 1 : -1];

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

/* 任务控制块池 */
tcb_t task_pool[KERNEL_MAX_TASKS];

/*
 * 已终止任务的 exit_value 保留表
 *
 * task_reclaim 会清零 TCB，但 task_join 可能还没读取 exit_value。
 * 在 TCB 被清零之前，将 exit_value 保存到这里。
 * task_join 读取后清除对应条目。
 */
static void *exit_retain[KERNEL_MAX_TASKS];
static kern_err_t exit_retain_result[KERNEL_MAX_TASKS];
static uint32_t exit_retain_bitmap = 0;

/* 任务栈池 */
static uint8_t task_stacks[KERNEL_MAX_TASKS][KERNEL_TASK_STACK_SIZE]
    __attribute__((aligned(KERNEL_TASK_STACK_SIZE)));

/*
 * 空闲任务栈。
 *
 * KERNEL_IDLE_STACK_SIZE 的历史默认值是 256 字节，足够纯 WFI idle，
 * 但调试输出或 idle hook 很容易把它压穿并破坏相邻静态栈区。
 * 这里不改配置文件，只在实现中给 idle 留出最低 512 字节余量。
 */
#define IDLE_STACK_SIZE_ACTUAL \
    ((KERNEL_IDLE_STACK_SIZE < 512) ? 512 : KERNEL_IDLE_STACK_SIZE)

static uint8_t idle_stack[IDLE_STACK_SIZE_ACTUAL]
    __attribute__((aligned(8)));

/* 空闲任务 TCB */
static tcb_t idle_task;

/* 任务使用位图 */
uint32_t task_used_bitmap = 0;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 任务退出处理
static void task_exit_handler(void) {
    task_exit(NULL);
}

#if SYSCALL_ENABLE
static void user_task_exit_handler(void) {
    register int r0 __asm("r0") = SYSCALL_TASK_EXIT;

    __asm volatile("svc #1" : "+r"(r0) :: "memory");

    while (1) {
        __asm volatile("wfi");
    }
}
#endif

// 空闲任务函数
/**
 * @brief 空闲任务函数，系统在没有其他任务执行时运行此任务
 * @param arg 未使用的任务参数，通过(void)arg避免编译器警告
 */
static void idle_task_func(void *arg) {
    (void)arg;  // 显式标记未使用的参数，避免编译器警告

    while (1) {  // 无限循环，确保任务持续运行
#if KERN_IDLE_SLEEP
        hal_enter_lowpower();  // 如果启用了空闲睡眠功能，则进入低功耗模式
#endif

#if KERN_WATCHDOG_ENABLE
        hal_watchdog_feed();  // 如果启用了看门狗功能，则喂狗
#endif
    }
}

// 查找空闲任务 ID。调用者负责在提交 bitmap 前持有临界区。
static task_id_t find_free_task_id(void) {
    for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
        if (!(task_used_bitmap & (1U << i))) {
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void mark_task_id_used(task_id_t id) {
    if (id >= 0 && id < KERNEL_MAX_TASKS) {
        task_used_bitmap |= (1U << id);
    }
}

// 释放任务 ID
static void free_task_id(task_id_t id) {
    if (id >= 0 && id < KERNEL_MAX_TASKS) {
        task_used_bitmap &= ~(1U << id);
    }
}

static int task_id_is_used(task_id_t id) {
    return (id >= 0 && id < KERNEL_MAX_TASKS &&
            (task_used_bitmap & (1U << id)) != 0);
}

static void task_wake_joiners(tcb_t *tcb, kern_err_t result) {
    tcb_t *j = tcb->joiners;

    while (j) {
        tcb_t *next = j->join_next;
        j->join_next = NULL;
        sched_wakeup(j, result);
        j = next;
    }
    tcb->joiners = NULL;
}

static void task_record_exit(tcb_t *tcb, void *retval, kern_err_t result) {
    if (tcb == NULL || tcb->id < 0 || tcb->id >= KERNEL_MAX_TASKS) {
        return;
    }

    tcb->exit_value = retval;
    exit_retain[tcb->id] = retval;
    exit_retain_result[tcb->id] = result;
    exit_retain_bitmap |= (1U << tcb->id);
}

static void task_cleanup_resources(tcb_t *tcb, kern_err_t join_result) {
    if (tcb == NULL || tcb->id < 0) {
        return;
    }

#if VFS_ENABLE
    vfs_close_task_fds(tcb);
#endif

#if MPU_ENABLE && CAP_ENABLE
    kshm_unmap_all_for_task(tcb);
#endif

#if CAP_ENABLE
    root_bootstrap_cleanup_task(tcb);
    cap_revoke_all((uint8_t)tcb->id);
#endif

    task_wake_joiners(tcb, join_result);
}

static void task_unlink_from_join_target(tcb_t *tcb) {
    tcb_t *target = (tcb_t *)tcb->block_obj;

    if (target == NULL) {
        return;
    }

    tcb_t **link = &target->joiners;
    while (*link) {
        if (*link == tcb) {
            *link = tcb->join_next;
            tcb->join_next = NULL;
            return;
        }
        link = &(*link)->join_next;
    }
}

static kern_err_t task_unlink_blocked(tcb_t *tcb) {
    if (tcb == NULL || tcb->state != TASK_STATE_BLOCKED) {
        return KERN_OK;
    }

    switch (tcb->block_reason) {
    case BLOCK_REASON_NONE:
    case BLOCK_REASON_SLEEP:
    case BLOCK_REASON_TIMER:
    case BLOCK_REASON_IRQ:
        break;

    case BLOCK_REASON_JOIN:
        task_unlink_from_join_target(tcb);
        break;

    case BLOCK_REASON_SEM:
        if (tcb->block_obj) {
            sem_t *sem = (sem_t *)tcb->block_obj;
            wait_queue_remove_safe(&sem->wait_queue, tcb);
        }
        break;

    case BLOCK_REASON_MUTEX:
        if (tcb->block_obj) {
            mutex_t *mutex = (mutex_t *)tcb->block_obj;
            wait_queue_remove_safe(&mutex->wait_queue, tcb);
        }
        break;

    case BLOCK_REASON_QUEUE:
        if (tcb->block_obj) {
            mqueue_t *mq = (mqueue_t *)tcb->block_obj;
            wait_queue_remove_safe(&mq->send_queue, tcb);
            wait_queue_remove_safe(&mq->recv_queue, tcb);
        }
        break;

    case BLOCK_REASON_EVENT:
        if (tcb->block_obj) {
            event_t *evt = (event_t *)tcb->block_obj;
            wait_queue_remove_safe(&evt->wait_queue, tcb);
        }
        break;

    case BLOCK_REASON_EP_SEND:
    case BLOCK_REASON_EP_RECV:
#if IPC_ENDPOINT
        endpoint_cleanup_task(tcb->block_obj, tcb);
#else
        return KERN_ERR_BUSY;
#endif
        break;

    case BLOCK_REASON_CH_SEND:
    case BLOCK_REASON_CH_RECV:
#if IPC_CHANNEL
        channel_cleanup_task(tcb->block_obj, tcb);
#else
        return KERN_ERR_BUSY;
#endif
        break;

    default:
        return KERN_ERR_STATE;
    }

    tcb->wait_next = NULL;
    tcb->wait_prev = NULL;
    tcb->block_reason = BLOCK_REASON_NONE;
    tcb->block_obj = NULL;
    tcb->wake_tick = 0;
    return KERN_OK;
}

static void task_write_saved_svc_r0(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->sp == NULL) {
        return;
    }

    /*
     * SVC saves R4-R11 below the hardware frame before blocking:
     *   sp + 0..31  saved R4-R11
     *   sp + 32     stacked R0
     */
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
    *stacked_r0 = (uint32_t)result;
}

kern_err_t task_cancel_blocked_wait(tcb_t *tcb) {
    return task_unlink_blocked(tcb);
}

void task_complete_blocked_syscall(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->syscall_blocked == 0) {
        return;
    }

    task_write_saved_svc_r0(tcb, result);
    tcb->syscall_blocked = 0;
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void task_init(void) {
    // 清零任务池
    memset(task_pool, 0, sizeof(task_pool));
    memset(task_stacks, 0, sizeof(task_stacks));

    task_used_bitmap = 0;
    memset(exit_retain, 0, sizeof(exit_retain));
    memset(exit_retain_result, 0, sizeof(exit_retain_result));
    exit_retain_bitmap = 0;

    /* 初始化空闲任务 */
    memset(&idle_task, 0, sizeof(idle_task));
    idle_task.id = -1;  /* 特殊 ID */
    idle_task.priority = KERNEL_IDLE_PRIORITY;
    idle_task.base_priority = KERNEL_IDLE_PRIORITY;
    idle_task.state = TASK_STATE_READY;  /* 空闲任务始终就绪 */
    idle_task.stack_base = idle_stack;
    idle_task.stack_size = IDLE_STACK_SIZE_ACTUAL;
    idle_task.time_slice = KERN_DEFAULT_TIME_SLICE;
    idle_task.time_slice_reload = KERN_DEFAULT_TIME_SLICE;
    idle_task.attrs = TASK_ATTR_PRIVILEGED;  /* 空闲任务运行在特权模式 */
    strncpy(idle_task.name, "idle", KERN_TASK_NAME_LEN - 1);

    /* 初始化空闲任务栈 */
    idle_task.sp = hal_stack_init(
        idle_stack + IDLE_STACK_SIZE_ACTUAL,
        IDLE_STACK_SIZE_ACTUAL,
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
    /* 检查优先级范围 */
    if (priority >= KERNEL_MAX_PRIORITIES) {
        return KERN_INVALID_ID;
    }

    uint32_t crit = hal_enter_critical();

    // 分配任务 ID
    task_id_t id = find_free_task_id();
    if (id == KERN_INVALID_ID) {
        hal_exit_critical(crit);
        return KERN_INVALID_ID;
    }

    /* 清除保留表条目 (ID 可能被复用) */
    exit_retain[id] = NULL;
    exit_retain_result[id] = KERN_OK;
    exit_retain_bitmap &= ~(1U << id);

    tcb_t *tcb = &task_pool[id];

    // 初始化 TCB
    memset(tcb, 0, sizeof(tcb_t));
    tcb->id = id;
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->state = TASK_STATE_CREATED;
    tcb->attrs = TASK_ATTR_PRIVILEGED;  /* 默认创建特权任务，兼容现有代码 */

    // 设置名称
    if (name) {
        strncpy(tcb->name, name, KERN_TASK_NAME_LEN - 1);
        tcb->name[KERN_TASK_NAME_LEN - 1] = '\0';
    } else {
        strcpy(tcb->name, "task");
        char num[12];
        int_to_str(id, num);
        uint32_t pos = 4U;
        uint32_t i = 0U;
        while (num[i] != '\0' && pos + 1U < KERN_TASK_NAME_LEN) {
            tcb->name[pos++] = num[i++];
        }
        tcb->name[pos] = '\0';
    }

    /* 设置栈 — 512 最小 (初始帧 36B + 异常帧 32B + 上下文保存 32B + 余量) */
    if (stack_size < 512 || stack_size > KERNEL_TASK_STACK_SIZE) {
        stack_size = KERNEL_TASK_STACK_SIZE;
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

    /*
     * TCB 和初始栈帧都有效后再发布 bitmap。
     * SysTick/task scan 只通过 bitmap 发现任务，不能看到半初始化 TCB。
     */
    mark_task_id_used(id);
    hal_exit_critical(crit);

    return id;
}

kern_err_t task_start(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    if (!task_id_is_used(task_id)) {
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];

    if (tcb->state != TASK_STATE_CREATED) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    // 加入就绪队列
    sched_add_ready(tcb);

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t task_exit_request(void *retval) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    task_record_exit(current, retval, KERN_OK);

    /* 设置为终止状态 */
    current->state = TASK_STATE_TERMINATED;

    task_cleanup_resources(current, KERN_OK);

    /* free_task_id 由 task_reclaim 负责，避免 double-free */

    /* 触发调度 */
    sched_yield();

    return KERN_OK;
}

void task_exit(void *retval) {
    (void)task_exit_request(retval);

    while (1);
}

kern_err_t task_suspend(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = hal_enter_critical();

    if (!task_id_is_used(task_id)) {
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

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
            if (task_unlink_blocked(tcb) != KERN_OK) {
                hal_exit_critical(crit);
                return KERN_ERR_BUSY;
            }

            // 从阻塞状态转为挂起，清除阻塞信息
            tcb->state = TASK_STATE_SUSPENDED;
            hal_exit_critical(crit);
            break;

        default:
            hal_exit_critical(crit);
            return KERN_ERR_STATE;
    }

    return KERN_OK;
}

kern_err_t task_resume(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = hal_enter_critical();

    if (!task_id_is_used(task_id)) {
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

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
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = hal_enter_critical();

    if (!task_id_is_used(task_id)) {
        if (exit_retain_bitmap & (1U << task_id)) {
            exit_retain_result[task_id] = KERN_OK;
            exit_retain_bitmap &= ~(1U << task_id);
            exit_retain[task_id] = NULL;
            hal_exit_critical(crit);
            return KERN_OK;
        }
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

    // 不能删除当前任务
    if (tcb == sched_get_current()) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    if (tcb->state == TASK_STATE_BLOCKED) {
        kern_err_t err = task_unlink_blocked(tcb);
        if (err != KERN_OK) {
            hal_exit_critical(crit);
            return err;
        }
    }

    // 从就绪队列移除
    sched_remove_ready(tcb);

    if (tcb->state != TASK_STATE_TERMINATED) {
        task_cleanup_resources(tcb, KERN_ERR_NOEXIST);
    } else if (task_id >= 0 && task_id < KERNEL_MAX_TASKS) {
        exit_retain_bitmap &= ~(1U << task_id);
        exit_retain[task_id] = NULL;
        exit_retain_result[task_id] = KERN_OK;
    }

    // 先撤销可见性，再清零 TCB，避免扫描路径看到半清理槽位
    free_task_id(task_id);
    memset(tcb, 0, sizeof(tcb_t));

    hal_exit_critical(crit);
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
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return NULL;
    }

    if ((task_used_bitmap & (1U << task_id)) == 0) {
        return NULL;
    }

    tcb_t *tcb = &task_pool[task_id];
    if (tcb->state == TASK_STATE_TERMINATED ||
        tcb->state > TASK_STATE_TERMINATED) {
        return NULL;
    }

    return tcb;
}

task_id_t task_get_next(task_id_t task_id) {
    int start = (task_id < 0) ? 0 : task_id + 1;
    for (int i = start; i < KERNEL_MAX_TASKS; i++) {
        if (task_used_bitmap & (1U << i)) {
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

kern_err_t task_set_priority(task_id_t task_id, uint8_t priority) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    if (priority >= KERNEL_MAX_PRIORITIES) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    if (!task_id_is_used(task_id)) {
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
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

    hal_exit_critical(crit);
    return KERN_OK;
}

uint8_t task_get_priority(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->priority : 0xFF;
}

kern_err_t task_delay(uint32_t ticks) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    if (ticks == 0) {
        return KERN_OK;
    }

    kern_err_t err = sched_block(BLOCK_REASON_SLEEP, NULL, ticks);
    return (err == KERN_ERR_TIMEOUT) ? KERN_OK : err;
}

kern_err_t task_delay_ms(uint32_t ms) {
    /* KERNEL_TICK_RATE 是 Hz，假设 1000Hz = 1ms per tick */
    uint32_t ticks = (ms * KERNEL_TICK_RATE) / 1000;
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
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS)
        return KERN_ERR_PARAM;

    if (!task_id_is_used(task_id)) {
        if (exit_retain_bitmap & (1U << task_id)) {
            if (retval) *retval = exit_retain[task_id];
            kern_err_t result = exit_retain_result[task_id];
            exit_retain_bitmap &= ~(1U << task_id);
            exit_retain[task_id] = NULL;
            exit_retain_result[task_id] = KERN_OK;
            return result;
        }
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];
    tcb_t *current = sched_get_current();

    /* 不能 join 自己 */
    if (tcb == current)
        return KERN_ERR_PARAM;

    /* 已终止 — 直接取 retval */
    if (tcb->state == TASK_STATE_TERMINATED) {
        if (retval) *retval = tcb->exit_value;
        kern_err_t result = (exit_retain_bitmap & (1U << task_id))
                            ? exit_retain_result[task_id] : KERN_OK;
        /* 清除保留表条目 */
        exit_retain_bitmap &= ~(1U << task_id);
        exit_retain[task_id] = NULL;
        exit_retain_result[task_id] = KERN_OK;
        return result;
    }

    /*
     * used_bitmap 后备检测：task_reclaim 可能在 task_join 之前
     * 运行（PendSV + tick handler），TCB 被清零。
     * 如果 bit 已清除，说明任务已被回收（只有 TERMINATED 才会回收）。
     * 从保留表中读取 exit_value。
     */
    if (!task_id_is_used(task_id)) {
        kern_err_t result = KERN_OK;
        if (retval) {
            *retval = (exit_retain_bitmap & (1U << task_id))
                      ? exit_retain[task_id] : NULL;
        }
        if (exit_retain_bitmap & (1U << task_id)) {
            result = exit_retain_result[task_id];
        }
        exit_retain_bitmap &= ~(1U << task_id);
        exit_retain[task_id] = NULL;
        exit_retain_result[task_id] = KERN_OK;
        return result;
    }

    /* 挂入 joiner 链表 */
    current->join_next = tcb->joiners;
    tcb->joiners = current;

    /* 阻塞等待 */
    kern_err_t err = sched_block(BLOCK_REASON_JOIN, tcb, timeout);

    if (err == KERN_OK) {
        kern_err_t result = KERN_OK;
        if (exit_retain_bitmap & (1U << task_id)) {
            result = exit_retain_result[task_id];
            if (retval) {
                *retval = exit_retain[task_id];
            }
            exit_retain_bitmap &= ~(1U << task_id);
            exit_retain[task_id] = NULL;
            exit_retain_result[task_id] = KERN_OK;
            return result;
        }

        if (retval) {
            *retval = tcb->exit_value;
        }
    }

    return err;
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
 * 用户任务创建
 *============================================================================*/

#if MPU_ENABLE
#include "board_config.h"
#endif

task_id_t task_create_user(const char   *name,
                            task_func_t  entry,
                            void        *arg,
                            uint8_t      priority,
                            uint32_t     stack_size)
{
    task_id_t id = task_create(name, entry, arg, priority, stack_size);
    if (id < 0) return id;

    tcb_t *tcb = &task_pool[id];

    /* 标记为用户任务 */
    tcb->attrs = TASK_ATTR_USER;

#if SYSCALL_ENABLE
    /*
     * task_create() 默认把 LR 设为内核 task_exit_handler。
     * 用户任务返回时必须经 SVC 退出，不能在非特权线程里直接跑内核退出路径。
     */
    tcb->sp = hal_stack_init((uint8_t *)tcb->stack_base + tcb->stack_size,
                             tcb->stack_size,
                             entry,
                             arg,
                             user_task_exit_handler);
#endif

#if MPU_ENABLE
    /*
     * PSPLIM 装载用户栈下界。PendSV/SVC context-in 时(msr psplim, r2)生效,
     * 用户任务 SP 跌破 stack_base 即触发 MemManage fault — 比 MPU 守卫区
     * 更精确(0x1F 误差 vs 32B 老的子区域 hack)且不耗区。
     */
    tcb->sp_limit = (uint32_t)(uintptr_t)tcb->stack_base;

    /* MPU Region 0: Flash 代码段 (用户 RO+Execute) */
    mpu_region_encode(0, BOARD_FLASH_BASE, BOARD_FLASH_SIZE,
                      RASR_ENABLE | AP_PRW_URO | ATTR_NORMAL_WBWA,
                      &tcb->mpu_regions[0][0],
                      &tcb->mpu_regions[0][1]);

    /*
     * Region 1 intentionally remains disabled.
     *
     * Older bring-up mapped the full SRAM as user RW. That made syscall tests
     * easy, but it also let user tasks write TCBs, ready queues, and kernel
     * globals. P0 tightens the boundary to Flash RO + current task stack RW.
     * Future shared/user data buffers should be added as explicit regions.
     */
    tcb->mpu_regions[1][0] = 0;
    tcb->mpu_regions[1][1] = 0;

    /* MPU Region 2: 用户栈 (RW + XN) — PSPLIM 兜底下界,无需缩 32 字节做守卫 */
    uint32_t stack_base = (uint32_t)(uintptr_t)tcb->stack_base;
    mpu_region_encode(2, stack_base, tcb->stack_size,
                      AP_FULL | ATTR_NORMAL_WBWA | XN_ENABLE,
                      &tcb->mpu_regions[2][0],
                      &tcb->mpu_regions[2][1]);

    /* MPU Region 3-7: 禁用,留给 kshm 等运行时映射 */
    for (int i = 3; i < MPU_REGION_COUNT; i++) {
        tcb->mpu_regions[i][0] = 0;
        tcb->mpu_regions[i][1] = 0;
    }
#endif

    return id;
}


/*============================================================================
 * 任务回收 (由 PendSV 调用)
 *============================================================================*/

void task_reclaim(tcb_t *tcb) {
    if (tcb == NULL || tcb->id < 0) return;
    if (tcb->state != TASK_STATE_TERMINATED) return;

    /* 还有 joiner 在等 — 延迟回收 */
    if (tcb->joiners) return;

    /*
     * 设置 reclaim_at 延迟回收，给 task_join 一拍的时间读取
     * exit_value/state。避免 PendSV 立即清零 TCB 导致 joiner
     * 看不到 TERMINATED 状态。
     */
    if (tcb->reclaim_at == 0) {
        tcb->reclaim_at = sched_get_tick_count() + 1;
    }
}

void task_reclaim_expired(void) {
    uint32_t now = sched_get_tick_count();
    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if (!task_id_is_used(id)) continue;

        tcb_t *tcb = &task_pool[id];
        if (tcb->state != TASK_STATE_TERMINATED) continue;
        if (tcb->reclaim_at == 0) continue;
        if (now < tcb->reclaim_at) continue;
        if (tcb->joiners) continue;

        free_task_id(id);
        memset(tcb, 0, sizeof(tcb_t));
    }
}

kern_err_t task_terminate_with_result(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->id < 0) {
        return KERN_ERR_PARAM;
    }

    if (tcb->state == TASK_STATE_TERMINATED) {
        return KERN_OK;
    }

    if (tcb->state == TASK_STATE_BLOCKED) {
        (void)task_unlink_blocked(tcb);
    }

    task_record_exit(tcb, NULL, result);
    tcb->state = TASK_STATE_TERMINATED;
    task_cleanup_resources(tcb, result);
    return KERN_OK;
}

void task_terminate(tcb_t *tcb) {
    (void)task_terminate_with_result(tcb, KERN_OK);
}

/*============================================================================
 * 栈溢出检测
 *============================================================================*/

void task_check_stack_overflow(void) {
    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if (!(task_used_bitmap & (1U << id))) continue;

        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_TERMINATED) continue;
        if (tcb->state > TASK_STATE_TERMINATED) continue;
        if (tcb->stack_base == NULL || tcb->stack_size < 16) {
            hal_debug_puts("\r\n[STACK CHECK] Invalid TCB stack metadata, id=");
            char id_buf[12];
            int_to_str(id, id_buf);
            hal_debug_puts(id_buf);
            hal_debug_puts("\r\n");
            continue;
        }

        uint8_t *stack_base = (uint8_t *)tcb->stack_base;

        /* 检查栈底魔数是否被破坏 */
        int overflow = 0;
        for (int i = 0; i < 16; i++) {
            if (stack_base[i] != STACK_MAGIC_BYTE) {
                overflow = 1;
                break;
            }
        }

        if (overflow) {
            hal_debug_puts("\r\n[STACK OVERFLOW] Task: ");
            if (tcb->name[0] != '\0') {
                hal_debug_puts(tcb->name);
            } else {
                hal_debug_puts("<unnamed> id=");
                char id_buf[12];
                int_to_str(id, id_buf);
                hal_debug_puts(id_buf);
            }
            hal_debug_puts("\r\n");
        }
    }
}
