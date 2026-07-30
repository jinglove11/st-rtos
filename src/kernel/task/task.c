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
#include "spinlock.h"
#include "endpoint.h"
#include "channel.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
/* Phase F4: vfs.h 移除 (内核 VFS 已删) */
#include "mem.h"
#include "root_bootstrap.h"

/*
 * PendSV/SVC 汇编通过 tcb_offsets.inc (构建时由 scripts/gen_tcb_offsets.py
 * 从 kernel_types.h 自动生成) 间接拿到 tcb_t 字段偏移。这里仅放一个
 * "TCB 布局变化时让 C 编译先失败" 的兜底断言 — 真正的同步机制靠
 * gen_tcb_offsets.py 重新跑 offsetof()。详见 CMakeLists.txt 的 custom_command。
 */
typedef char tcb_state_offset_in_first_cacheline[(offsetof(tcb_t, state) < 64) ? 1 : -1];

/* Return to Thread mode using PSP, with a basic (non-FP) hardware frame. */
#define TASK_INITIAL_EXC_RETURN 0xFFFFFFFDUL

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
 * Generation-aware exit records for the legacy raw task_id join API.
 *
 * Task slots must remain immediately reusable (many kernel workers are
 * fire-and-forget), but an SMP creator can be delayed until after its child's
 * raw ID has already been recycled.  A single record indexed only by ID then
 * aliases the new task.  Keep a bounded history keyed by (id,generation), and
 * remember which generation each creator received from task_create().
 * User-facing task capabilities already carry this generation explicitly;
 * this table is the compatibility bridge for in-kernel task_join(id).
 */
#define TASK_EXIT_RECORD_MAX (KERNEL_MAX_TASKS * 4U)
typedef struct {
    task_id_t task_id;
    uint16_t _pad;
    uint32_t generation;
    void *value;
    kern_err_t result;
    uint32_t sequence;
    uint8_t valid;
} task_exit_record_t;

static task_exit_record_t task_exit_records[TASK_EXIT_RECORD_MAX];
static uint32_t task_join_expected[KERNEL_MAX_TASKS][KERNEL_MAX_TASKS];
static uint32_t task_exit_sequence;
static uint16_t task_exit_next;

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

static uint8_t idle_stacks[SMP_MAX_CPUS][IDLE_STACK_SIZE_ACTUAL]
    __attribute__((aligned(8)));

/* Per-CPU idle task TCBs. Sharing one idle TCB/stack across cores corrupts
 * the saved PSP when both cores become idle at the same time. */
static tcb_t idle_tasks[SMP_MAX_CPUS];

/*
 * 任务使用位图按 32 位原子访问单元保存。RP2350 是 32 位内核，直接并发
 * 读写 uint64_t 会撕裂；池内增删仍由 task_lock 串行化，查询路径则只需
 * 读取包含目标 ID 的单个 word。
 */
static uint32_t task_used_words[2] = {0, 0};

/* Spinlock protecting task_pool, task_used_bitmap, and exit-record tables.
 * In single-core mode this is uncontended (degrades to IRQ disable). */
static irq_spinlock_t task_lock;

/* A terminated TCB remains published while task-owned resources are cleaned
 * outside task_lock.  Deleters/reclaimers must not reuse the slot meanwhile. */
#define TASK_RECLAIM_CLEANING UINT32_MAX

typedef char task_bitmap_covers_config[(KERNEL_MAX_TASKS <= 64) ? 1 : -1];

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
        uint32_t word = (uint32_t)i >> 5;
        uint32_t bit = 1u << ((uint32_t)i & 31u);
        if ((task_used_words[word] & bit) == 0 &&
            !kobj_generation_is_retired(task_pool[i].hdr.generation)) {
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void mark_task_id_used(task_id_t id) {
    if (id >= 0 && id < KERNEL_MAX_TASKS) {
        uint32_t uid = (uint32_t)id;
        task_used_words[uid >> 5] |= 1u << (uid & 31u);
    }
}

// 释放任务 ID
static void free_task_id(task_id_t id) {
    if (id >= 0 && id < KERNEL_MAX_TASKS) {
        uint32_t uid = (uint32_t)id;
        task_used_words[uid >> 5] &= ~(1u << (uid & 31u));
    }
}

static int task_id_is_used(task_id_t id) {
    if (id < 0 || id >= KERNEL_MAX_TASKS) {
        return 0;
    }
    uint32_t uid = (uint32_t)id;
    return (task_used_words[uid >> 5] & (1u << (uid & 31u))) != 0;
}

/* All exit-record helpers are called with task_lock held. */
static int task_exit_record_find(task_id_t id, uint32_t generation) {
    for (uint32_t i = 0; i < TASK_EXIT_RECORD_MAX; i++) {
        if (task_exit_records[i].valid &&
            task_exit_records[i].task_id == id &&
            task_exit_records[i].generation == generation) {
            return (int)i;
        }
    }
    return -1;
}

static int task_exit_record_latest(task_id_t id) {
    int found = -1;
    uint32_t newest = 0U;
    for (uint32_t i = 0; i < TASK_EXIT_RECORD_MAX; i++) {
        if (task_exit_records[i].valid &&
            task_exit_records[i].task_id == id &&
            (found < 0 || task_exit_records[i].sequence > newest)) {
            found = (int)i;
            newest = task_exit_records[i].sequence;
        }
    }
    return found;
}

static void task_exit_record_store(tcb_t *tcb, void *value,
                                   kern_err_t result) {
    int slot = task_exit_record_find(tcb->id, tcb->hdr.generation);
    if (slot < 0) {
        slot = (int)task_exit_next;
        task_exit_next = (uint16_t)((task_exit_next + 1U) %
                                    TASK_EXIT_RECORD_MAX);
    }
    task_exit_record_t *record = &task_exit_records[slot];
    record->task_id = tcb->id;
    record->generation = tcb->hdr.generation;
    record->value = value;
    record->result = result;
    record->sequence = ++task_exit_sequence;
    record->valid = 1U;
}

static int task_exit_record_take(task_id_t id, uint32_t generation,
                                 int latest, void **value,
                                 kern_err_t *result) {
    int slot = latest ? task_exit_record_latest(id)
                      : task_exit_record_find(id, generation);
    if (slot < 0) {
        return 0;
    }
    task_exit_record_t *record = &task_exit_records[slot];
    if (value != NULL) {
        *value = record->value;
    }
    if (result != NULL) {
        *result = record->result;
    }
    memset(record, 0, sizeof(*record));
    return 1;
}

static void task_exit_record_remove(task_id_t id, uint32_t generation) {
    int slot = task_exit_record_find(id, generation);
    if (slot >= 0) {
        memset(&task_exit_records[slot], 0, sizeof(task_exit_records[slot]));
    }
}

static void task_cleanup_resources(tcb_t *tcb);

/* Caller holds task_lock.  Detach the list before publishing a reclaimable
 * target state, so a woken joiner can never race task_delete() against a TCB
 * that still owns joiners. */
static tcb_t *task_detach_joiners_locked(tcb_t *tcb) {
    tcb_t *j = tcb->joiners;
    void *exit_value = tcb->exit_value;
    tcb->joiners = NULL;

    for (tcb_t *it = j; it != NULL; it = it->join_next) {
        it->join_value = exit_value;
    }

    return j;
}

static void task_wake_joiner_list(tcb_t *j, kern_err_t result) {
    while (j) {
        tcb_t *next = j->join_next;
        j->join_next = NULL;
        sched_wakeup(j, result);
        j = next;
    }
}

static void task_record_exit(tcb_t *tcb, void *retval, kern_err_t result) {
    if (tcb == NULL || tcb->id < 0 || tcb->id >= KERNEL_MAX_TASKS) {
        return;
    }

    tcb->exit_value = retval;
    task_exit_record_store(tcb, retval, result);
}

static void task_finish_termination(tcb_t *tcb, kern_err_t result) {
    task_cleanup_resources(tcb);

    uint32_t crit = irq_spin_lock(&task_lock);
    if (task_id_is_used(tcb->id) &&
        tcb->reclaim_at == TASK_RECLAIM_CLEANING) {
        /* Complete the whole publish+wakeup transaction while task_lock and
         * local IRQ masking are still held.  In particular, a self-exiting
         * task must not become TERMINATED with wakeups left to execute: the
         * PendSV entry deliberately does not save a TERMINATED context, so a
         * tick in that window would strand every joiner until timeout.
         *
         * A remote woken joiner may run immediately, but task_delete() then
         * waits on task_lock and cannot observe a partially published exit. */
        tcb_t *joiners = task_detach_joiners_locked(tcb);
        if (joiners != NULL) {
            /* Every already-registered joiner has received exit_value above
             * and will receive result from task_wake_joiner_list().  No late
             * raw-id tombstone is needed, so normal reclaim may recycle the
             * slot after the wakeup transaction. */
            task_exit_record_remove(tcb->id, tcb->hdr.generation);
        }
        tcb->reclaim_at = sched_get_tick_count() + 1U;
        tcb->state = TASK_STATE_TERMINATED;
        task_wake_joiner_list(joiners, result);
    }
    irq_spin_unlock(&task_lock, crit);
}

static void task_cleanup_resources(tcb_t *tcb) {
    if (tcb == NULL || tcb->id < 0) {
        return;
    }

    /* Phase F4: vfs_close_task_fds 移除 (内核 VFS 已删)。
     * fd 清理由 fs_server 的 kern.fault 订阅 + fs_store_close_client_fds 承担。 */

#if MPU_ENABLE && CAP_ENABLE
    kshm_unmap_all_for_task(tcb);
    kmmio_unmap_all_for_task(tcb);
#endif

#if CAP_ENABLE
    root_bootstrap_cleanup_task(tcb);
    cnode_t *cspace = cap_space_of(tcb);
    cap_revoke_all((uint8_t)tcb->id);
    /* M2: 撤销其他任务持有的指向此 task 的 cap。
     * 防止 task id 复用后旧 cap 控制无关新 task (陈旧授权)。
     * M2-Step3c: 改用真指针 &tcb (header 在 offset 0)。 */
    (void)cap_revoke_object(tcb, CAP_OBJ_TASK);
    if (cspace != NULL) {
        /* A CNode is a first-class kernel object: revoke handles held by any
         * other task before retiring this task's namespace generation. */
        (void)cap_revoke_object(cspace, CAP_OBJ_CNODE);
        if (cap_space_destroy(tcb) != KERN_OK) {
            extern void kern_panic(const char *msg);
            kern_panic("task CSpace cleanup failed");
        }
    }
    /* generation bump 不在此做 — task_delete 的 memset 会清零;
     * 由 task_delete 在 memset 后写回。 */
#endif

}

static void task_unlink_from_join_target(tcb_t *tcb) {
    tcb_t *target = (tcb_t *)tcb->cont.object;

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

    switch (tcb->cont.op) {
    case BLOCK_REASON_NONE:
    case BLOCK_REASON_SLEEP:
    case BLOCK_REASON_TIMER:
    case BLOCK_REASON_IRQ:
        break;

    case BLOCK_REASON_JOIN:
        task_unlink_from_join_target(tcb);
        break;

    case BLOCK_REASON_SEM:
        if (tcb->cont.object) {
            sem_cleanup_task(tcb->cont.object, tcb);
        }
        break;

    case BLOCK_REASON_MUTEX:
        if (tcb->cont.object) {
            mutex_cleanup_task(tcb->cont.object, tcb);
        }
        break;

    case BLOCK_REASON_QUEUE:
        if (tcb->cont.object) {
            mqueue_cleanup_task(tcb->cont.object, tcb);
        }
        break;

    case BLOCK_REASON_EVENT:
        if (tcb->cont.object) {
            event_cleanup_task(tcb->cont.object, tcb);
        }
        break;

    case BLOCK_REASON_EP_SEND:
    case BLOCK_REASON_EP_RECV:
#if IPC_ENDPOINT
        endpoint_cleanup_task(tcb->cont.object, tcb);
#else
        return KERN_ERR_BUSY;
#endif
        break;

    case BLOCK_REASON_CH_SEND:
    case BLOCK_REASON_CH_RECV:
#if IPC_CHANNEL
        channel_cleanup_task(tcb->cont.object, tcb);
#else
        return KERN_ERR_BUSY;
#endif
        break;

    default:
        return KERN_ERR_STATE;
    }

    tcb->wait_next = NULL;
    tcb->wait_prev = NULL;
    tcb->cont.op = BLOCK_REASON_NONE;
    tcb->cont.object = NULL;
    tcb->cont.deadline = 0;
    return KERN_OK;
}

static void task_write_saved_svc_r0(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->sp == NULL) {
        return;
    }

    /* SVC saves R4-R11 below the hardware core frame.  For an extended FP
     * frame S0-S15/FPSCR follow the core frame; they do not precede R0. */
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->sp + 32U);
    *stacked_r0 = (uint32_t)result;
}

kern_err_t task_cancel_blocked_wait(tcb_t *tcb) {
    return task_unlink_blocked(tcb);
}

void task_complete_blocked_syscall(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->cont.active == 0) {
        return;
    }

    task_write_saved_svc_r0(tcb, result);
    tcb->cont.active = 0;
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void task_init(void) {
    // 初始化任务池自旋锁
    irq_spin_init_rank(&task_lock, LOCKDEP_RANK_TASK);
    // 清零任务池
    memset(task_pool, 0, sizeof(task_pool));
    memset(task_stacks, 0, sizeof(task_stacks));

    task_used_words[0] = 0;
    task_used_words[1] = 0;
    memset(task_exit_records, 0, sizeof(task_exit_records));
    memset(task_join_expected, 0, sizeof(task_join_expected));
    task_exit_sequence = 0U;
    task_exit_next = 0U;

    /* 初始化 per-CPU 空闲任务 */
    memset(idle_tasks, 0, sizeof(idle_tasks));
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        tcb_t *idle = &idle_tasks[cpu];
        idle->id = -1;  /* 特殊 ID */
        idle->priority = KERNEL_IDLE_PRIORITY;
        idle->base_priority = KERNEL_IDLE_PRIORITY;
        idle->state = TASK_STATE_READY;  /* 空闲任务始终就绪 */
        idle->stack_base = idle_stacks[cpu];
        idle->stack_size = IDLE_STACK_SIZE_ACTUAL;
        idle->time_slice = KERN_DEFAULT_TIME_SLICE;
        idle->time_slice_reload = KERN_DEFAULT_TIME_SLICE;
        idle->affinity_mask = (1UL << cpu);
        idle->cpu_owner = (uint8_t)cpu;
        idle->migration_state = TASK_MIGRATION_STABLE;
        idle->attrs = TASK_ATTR_PRIVILEGED;  /* 空闲任务运行在特权模式 */
        idle->exc_return = TASK_INITIAL_EXC_RETURN;
        strncpy(idle->name, "idle", KERN_TASK_NAME_LEN - 1);

        /* 初始化空闲任务栈 */
        idle->sp = hal_stack_init(
            idle_stacks[cpu] + IDLE_STACK_SIZE_ACTUAL,
            IDLE_STACK_SIZE_ACTUAL,
            idle_task_func,
            NULL,
            task_exit_handler
        );
    }
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

    uint32_t crit = irq_spin_lock(&task_lock);

    // 分配任务 ID
    task_id_t id = find_free_task_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_INVALID_ID;
    }

    tcb_t *tcb = &task_pool[id];

    // 初始化 TCB
    /* M2-Step3c: 跨 memset 保留 generation。task 池复用 id 时,上次
     * task_cleanup_resources 已 bump 了 hdr.generation,这里恢复。
     * 首次分配 generation=0 → kobj_header_init 设 1。 */
    uint32_t saved_gen = tcb->hdr.generation;
    memset(tcb, 0, sizeof(tcb_t));
    kobj_header_init(&tcb->hdr, CAP_OBJ_TASK);
    if (saved_gen != 0) {
        tcb->hdr.generation = saved_gen;
    }
    /* A reused creator slot must not inherit tickets issued to the task that
     * previously occupied it. */
    memset(task_join_expected[id], 0, sizeof(task_join_expected[id]));
    tcb->id = id;
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->state = TASK_STATE_CREATED;
    tcb->affinity_mask = KERN_CPU_AFFINITY_ALL;
    tcb->cpu_owner = KERN_CPU_NONE;
    tcb->migration_state = TASK_MIGRATION_STABLE;
    tcb->attrs = TASK_ATTR_PRIVILEGED;  /* 默认创建特权任务，兼容现有代码 */
    tcb->exc_return = TASK_INITIAL_EXC_RETURN;

    tcb_t *creator = sched_get_current();
    if (creator != NULL && creator->id >= 0 &&
        creator->id < KERNEL_MAX_TASKS && creator != tcb) {
        task_join_expected[creator->id][id] = tcb->hdr.generation;
    }

#if CAP_ENABLE
    /* Every ordinary task owns an independent CNode even while privileged.
     * Privileged caps are not installed yet, but a later task_create_user()
     * transition can populate the already-live namespace safely. */
    if (cap_space_init(tcb) != KERN_OK) {
        uint32_t failed_gen = tcb->hdr.generation;
        if (creator != NULL && creator->id >= 0 &&
            creator->id < KERNEL_MAX_TASKS) {
            task_join_expected[creator->id][id] = 0U;
        }
        memset(tcb, 0, sizeof(*tcb));
        tcb->hdr.obj_type = CAP_OBJ_TASK;
        tcb->hdr.generation = failed_gen;
        irq_spin_unlock(&task_lock, crit);
        return KERN_INVALID_ID;
    }
#endif

    // 设置名称
    if (name) {
        strncpy(tcb->name, name, KERN_TASK_NAME_LEN - 1);
        tcb->name[KERN_TASK_NAME_LEN - 1] = '\0';
    } else {
        strcpy(tcb->name, "task");
        char num[12] = {0};
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
    irq_spin_unlock(&task_lock, crit);

    return id;
}

kern_err_t task_set_initial_arg(task_id_t task_id, void *arg) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];
    if (tcb->state != TASK_STATE_CREATED || tcb->stack_base == NULL ||
        tcb->stack_size < 32U) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    /* R0 is the lowest word of the Cortex-M hardware exception frame. */
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)tcb->stack_base +
                                        tcb->stack_size - 32U);
    *stacked_r0 = (uint32_t)(uintptr_t)arg;
    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

kern_err_t task_start(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&task_lock);

    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];

    if (tcb->state != TASK_STATE_CREATED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    // 加入就绪队列
    sched_add_ready(tcb);

    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

kern_err_t task_exit_request(void *retval) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    if (!task_id_is_used(current->id) ||
        current->state == TASK_STATE_TERMINATED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    task_record_exit(current, retval, KERN_OK);
    /* Do not publish TERMINATED until cleanup and joiner wakeup are ready to
     * commit.  PendSV skips saving TERMINATED tasks; publishing it here made
     * a SysTick during cleanup permanently abandon this execution context. */
    current->reclaim_at = TASK_RECLAIM_CLEANING;
    irq_spin_unlock(&task_lock, crit);

    /* Cleanup may acquire endpoint/capability/resource locks.  Keep it outside
     * task_lock so lock ordering stays TASK -> OBJECT -> RESOURCE. */
    task_finish_termination(current, KERN_OK);

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
    uint32_t crit = irq_spin_lock(&task_lock);

    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    if (tcb->state == TASK_STATE_TERMINATED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    if (tcb->state == TASK_STATE_SUSPENDED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_OK;
    }

#if SMP
    /* READY/RUNNING state is owned by cpu_owner.  Ask that CPU to remove the
     * task and acknowledge after PendSV has stopped referencing its TCB. */
    if (tcb->cpu_owner < SMP_MAX_CPUS &&
        tcb->cpu_owner != hal_get_cpu_id() &&
        (_current_task[tcb->cpu_owner] == tcb ||
         tcb->state == TASK_STATE_READY ||
         tcb->state == TASK_STATE_RUNNING)) {
        kern_err_t err = sched_quiesce_task(tcb);
        if (err == KERN_OK && tcb->state == TASK_STATE_BLOCKED) {
            err = task_unlink_blocked(tcb);
            if (err == KERN_OK) {
                tcb->state = TASK_STATE_SUSPENDED;
            }
        }
        irq_spin_unlock(&task_lock, crit);
        return err;
    }
#endif

    // 根据当前状态处理
    switch (tcb->state) {
        case TASK_STATE_RUNNING:
            // 当前任务挂起自己：设置状态后触发调度
            // PendSV 会检查状态，SUSPENDED 不会加入就绪队列
            tcb->state = TASK_STATE_SUSPENDED;
            irq_spin_unlock(&task_lock, crit);
            sched_yield();  // 触发 PendSV
            break;

        case TASK_STATE_READY:
            // 从就绪队列移除
            sched_remove_ready(tcb);
            tcb->state = TASK_STATE_SUSPENDED;
            irq_spin_unlock(&task_lock, crit);
            break;

        case TASK_STATE_BLOCKED:
            if (task_unlink_blocked(tcb) != KERN_OK) {
                irq_spin_unlock(&task_lock, crit);
                return KERN_ERR_BUSY;
            }

            // 从阻塞状态转为挂起，清除阻塞信息
            tcb->state = TASK_STATE_SUSPENDED;
            irq_spin_unlock(&task_lock, crit);
            break;

        default:
            irq_spin_unlock(&task_lock, crit);
            return KERN_ERR_STATE;
    }

    return KERN_OK;
}

kern_err_t task_resume(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = irq_spin_lock(&task_lock);

    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    if (tcb->state != TASK_STATE_SUSPENDED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    // 加入就绪队列
    sched_add_ready(tcb);

    // 如果优先级高于当前任务，触发调度
    tcb_t *current = sched_get_current();
    if (current && tcb->priority < current->priority) {
        hal_trigger_pendsv();
    }

    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

kern_err_t task_delete(task_id_t task_id) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    tcb_t *tcb = &task_pool[task_id];
    uint32_t crit = irq_spin_lock(&task_lock);
    tcb_t *caller = sched_get_current();
    uint32_t expected_generation = 0U;
    if (caller != NULL && caller->id >= 0 &&
        caller->id < KERNEL_MAX_TASKS) {
        expected_generation = task_join_expected[caller->id][task_id];
    }

    /* A creator may delete an old generation after another core has already
     * reused the numeric ID.  Treat that as deleting/detaching the creator's
     * generation; never kill the unrelated live occupant. */
    if (expected_generation != 0U &&
        (!task_id_is_used(task_id) ||
         tcb->hdr.generation != expected_generation)) {
        task_exit_record_remove(task_id, expected_generation);
        task_join_expected[caller->id][task_id] = 0U;
        irq_spin_unlock(&task_lock, crit);
        return KERN_OK;
    }

    if (!task_id_is_used(task_id)) {
        if (task_exit_record_take(task_id, 0U, 1, NULL, NULL)) {
            irq_spin_unlock(&task_lock, crit);
            return KERN_OK;
        }
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    // 不能删除当前任务
    if (tcb == sched_get_current()) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_STATE;
    }

    /* A completed termination can be reclaimed immediately by explicit
     * delete.  A cleanup still in progress owns the TCB and must finish first. */
    if (tcb->state == TASK_STATE_TERMINATED) {
        if (tcb->reclaim_at == TASK_RECLAIM_CLEANING) {
            irq_spin_unlock(&task_lock, crit);
            return KERN_ERR_BUSY;
        }

        task_exit_record_remove(task_id, tcb->hdr.generation);
        if (expected_generation != 0U) {
            task_join_expected[caller->id][task_id] = 0U;
        }
        free_task_id(task_id);
        uint32_t next_gen = kobj_header_prepare_reuse(&tcb->hdr);
        memset(tcb, 0, sizeof(tcb_t));
        tcb->hdr.obj_type = CAP_OBJ_TASK;
        tcb->hdr.generation = next_gen;
        irq_spin_unlock(&task_lock, crit);
        return KERN_OK;
    }

    if (tcb->state == TASK_STATE_BLOCKED &&
        !(tcb->cpu_owner < SMP_MAX_CPUS &&
          tcb->cpu_owner != hal_get_cpu_id())) {
        kern_err_t err = task_unlink_blocked(tcb);
        if (err != KERN_OK) {
            irq_spin_unlock(&task_lock, crit);
            return err;
        }
    }

    /* Externally deleting a task that is READY/RUNNING on another CPU must
     * first quiesce that CPU.  Otherwise memset below can race PendSV or live
     * task execution. */
    if ((tcb->cpu_owner < SMP_MAX_CPUS &&
         tcb->cpu_owner != hal_get_cpu_id()) ||
        tcb->state == TASK_STATE_READY || tcb->state == TASK_STATE_RUNNING) {
        kern_err_t err = sched_quiesce_task(tcb);
        if (err != KERN_OK) {
            irq_spin_unlock(&task_lock, crit);
            return err;
        }

        /* It may have blocked just before the remote IPI preempted it.  The
         * quiesce acknowledgement guarantees its owner CPU no longer uses
         * the TCB, so unlinking the wait object is now safe. */
        if (tcb->state == TASK_STATE_BLOCKED) {
            err = task_unlink_blocked(tcb);
            if (err != KERN_OK) {
                irq_spin_unlock(&task_lock, crit);
                return err;
            }
            tcb->state = TASK_STATE_SUSPENDED;
        }
    } else {
        sched_remove_ready(tcb);
    }

    task_record_exit(tcb, NULL, KERN_ERR_NOEXIST);
    tcb->state = TASK_STATE_TERMINATED;
    tcb->reclaim_at = TASK_RECLAIM_CLEANING;
    irq_spin_unlock(&task_lock, crit);

    task_cleanup_resources(tcb);

    crit = irq_spin_lock(&task_lock);
    if (!task_id_is_used(task_id) ||
        tcb->state != TASK_STATE_TERMINATED ||
        tcb->reclaim_at != TASK_RECLAIM_CLEANING) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_BUSY;
    }

    /* Detach joiners before the slot becomes reusable.  Their join_value is
     * already copied; generation-aware exit records cover late joiners. */
    tcb_t *joiners = task_detach_joiners_locked(tcb);

    /* Explicit delete is also the detach operation for an unjoined zombie.
     * Registered joiners are woken below with NOEXIST, while a future raw-id
     * join must not consume a record belonging to the deleted generation. */
    task_exit_record_remove(task_id, tcb->hdr.generation);
    if (expected_generation != 0U) {
        task_join_expected[caller->id][task_id] = 0U;
    }

    // 先撤销可见性，再清零 TCB，避免扫描路径看到半清理槽位
    free_task_id(task_id);
    /* 跨 memset bump generation:task_create 复用此 slot 时 saved_gen
     * 非 0 → 用新 generation → 旧 task cap cross-check 失效 (M2 验收 A1)。 */
    uint32_t next_gen = kobj_header_prepare_reuse(&tcb->hdr);
    memset(tcb, 0, sizeof(tcb_t));
    tcb->hdr.obj_type   = CAP_OBJ_TASK;
    tcb->hdr.generation = next_gen;

    irq_spin_unlock(&task_lock, crit);
    task_wake_joiner_list(joiners, KERN_ERR_NOEXIST);
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

    if (!task_id_is_used(task_id)) {
        return NULL;
    }

    tcb_t *tcb = &task_pool[task_id];
    if (tcb->state == TASK_STATE_TERMINATED ||
        tcb->state > TASK_STATE_TERMINATED) {
        return NULL;
    }

    return tcb;
}

/* M2-Step3c: cap 路径 id ↔ 对象指针 转换 (tcb_t.hdr 在 offset 0)。 */
task_id_t task_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    tcb_t *tcb = (tcb_t *)obj;
    task_id_t id = (task_id_t)(tcb - task_pool);
    if (id < 0 || id >= KERNEL_MAX_TASKS) return KERN_INVALID_ID;
    return id;
}

void *task_obj_for_cap(task_id_t id) {
    if (id < 0 || id >= KERNEL_MAX_TASKS) return NULL;
    return (void *)&task_pool[id];
}

task_id_t task_get_next(task_id_t task_id) {
    uint64_t snapshot = task_get_used_bitmap();
    int start = (task_id < 0) ? 0 : task_id + 1;
    for (int i = start; i < KERNEL_MAX_TASKS; i++) {
        if (snapshot & (1ULL << i)) {
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

    uint32_t crit = irq_spin_lock(&task_lock);

    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];

    // 更新优先级
    tcb->priority = priority;
    tcb->base_priority = priority;

    // 如果在就绪队列, 由 owner CPU 原地重新插入
    if (tcb->state == TASK_STATE_READY) {
        sched_reinsert_by_priority(tcb);
    }

    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

uint8_t task_get_priority(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->priority : 0xFF;
}

kern_err_t task_set_affinity(task_id_t task_id, uint32_t affinity_mask) {
    if (task_id < 0 || task_id >= KERNEL_MAX_TASKS ||
        affinity_mask == 0U ||
        (affinity_mask & ~KERN_CPU_AFFINITY_ALL) != 0U) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    if (!task_id_is_used(task_id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];
    if (tcb->state == TASK_STATE_RUNNING) {
        uint32_t owner_bit = (tcb->cpu_owner < SMP_MAX_CPUS)
                           ? (1UL << tcb->cpu_owner) : 0U;
        if ((affinity_mask & owner_bit) == 0U) {
            irq_spin_unlock(&task_lock, crit);
            return KERN_ERR_BUSY;
        }
        tcb->affinity_mask = affinity_mask;
        irq_spin_unlock(&task_lock, crit);
        return KERN_OK;
    }

    if (tcb->state == TASK_STATE_READY) {
        kern_err_t err = sched_quiesce_task(tcb);
        if (err != KERN_OK) {
            irq_spin_unlock(&task_lock, crit);
            return err;
        }
        tcb->affinity_mask = affinity_mask;
        sched_add_ready(tcb);
    } else {
        tcb->affinity_mask = affinity_mask;
    }

    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

uint32_t task_get_affinity(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->affinity_mask : 0U;
}

#if RT_SCHED
kern_err_t task_set_sched_policy(task_id_t task_id, uint8_t policy) {
    tcb_t *tcb = task_get_tcb(task_id);
    if (tcb == NULL) {
        return KERN_ERR_PARAM;
    }
    if (policy > SCHED_RR) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    tcb->sched_policy = policy;
    /* SCHED_FIFO: disable time-slice rotation (run until blocked/preempted).
     * SCHED_RR / SCHED_NORMAL: restore default time slice. */
    if (policy == SCHED_FIFO) {
        tcb->time_slice_reload = 0;
        tcb->time_slice = 0;
    } else {
        tcb->time_slice_reload = KERN_DEFAULT_TIME_SLICE;
        tcb->time_slice = KERN_DEFAULT_TIME_SLICE;
    }
    irq_spin_unlock(&task_lock, crit);
    return KERN_OK;
}

uint8_t task_get_sched_policy(task_id_t task_id) {
    tcb_t *tcb = task_get_tcb(task_id);
    return tcb ? tcb->sched_policy : SCHED_NORMAL;
}
#else
kern_err_t task_set_sched_policy(task_id_t task_id, uint8_t policy) {
    (void)task_id; (void)policy;
    return KERN_ERR_NOSYS;
}
uint8_t task_get_sched_policy(task_id_t task_id) {
    (void)task_id;
    return SCHED_NORMAL;
}
#endif

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

    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    uint32_t expected_generation = 0U;
    if (current->id >= 0 && current->id < KERNEL_MAX_TASKS) {
        expected_generation = task_join_expected[current->id][task_id];
    }

    /* The slot may already contain a newer task.  Resolve the creator's
     * generation ticket before consulting the raw live-ID bitmap. */
    if (expected_generation != 0U &&
        (!task_id_is_used(task_id) ||
         task_pool[task_id].hdr.generation != expected_generation)) {
        kern_err_t result = KERN_OK;
        if (task_exit_record_take(task_id, expected_generation, 0,
                                  retval, &result)) {
            irq_spin_unlock(&task_lock, crit);
            return result;
        }
        task_join_expected[current->id][task_id] = 0U;
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    if (!task_id_is_used(task_id)) {
        kern_err_t result = KERN_OK;
        if (task_exit_record_take(task_id, 0U, 1, retval, &result)) {
            irq_spin_unlock(&task_lock, crit);
            return result;
        }
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    tcb_t *tcb = &task_pool[task_id];

    /* 不能 join 自己 */
    if (tcb == current) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_PARAM;
    }

    /* A fully cleaned terminated task can be consumed immediately.  Merely
     * observing TERMINATED is insufficient on SMP: task_exit publishes that
     * state before it drops task-owned caps/endpoints.  A late join in that
     * window used to return KERN_OK while reclaim_at was still CLEANING, so
     * the caller's immediate task_delete() deterministically saw BUSY.
     *
     * For TERMINATED+CLEANING fall through and register as a normal joiner.
     * task_finish_termination() publishes the reclaimable state and detaches
     * this list under the same task_lock before waking us. */
    if (tcb->state == TASK_STATE_TERMINATED &&
        tcb->reclaim_at != TASK_RECLAIM_CLEANING) {
        kern_err_t result = KERN_OK;
        if (!task_exit_record_take(task_id, tcb->hdr.generation, 0,
                                   retval, &result) && retval != NULL) {
            *retval = tcb->exit_value;
        }
        irq_spin_unlock(&task_lock, crit);
        return result;
    }

    /* Publish the waiter and its BLOCKED state under one cross-core lock.
     * Otherwise target exit can observe the list while sched_wakeup still sees
     * this task RUNNING, losing the only wakeup. */
    current->join_value = NULL;
    current->join_next = tcb->joiners;
    tcb->joiners = current;
    current->state = TASK_STATE_BLOCKED;
    current->cont.op = BLOCK_REASON_JOIN;
    current->cont.object = tcb;
    current->cont.result = KERN_OK;
    current->cont.deadline = sched_timeout_deadline(timeout);
    irq_spin_unlock(&task_lock, crit);

    sched_yield();
    kern_err_t err = current->cont.result;

    if (err == KERN_OK) {
        if (retval) {
            *retval = current->join_value;
        }
        current->join_value = NULL;
    }

    return err;
}

void task_cancel_join_wait(tcb_t *tcb) {
    if (tcb == NULL) {
        return;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    if (tcb->state == TASK_STATE_BLOCKED &&
        tcb->cont.op == BLOCK_REASON_JOIN) {
        task_unlink_from_join_target(tcb);
        tcb->cont.object = NULL;
    }
    irq_spin_unlock(&task_lock, crit);
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
    uint32_t cpu = hal_get_cpu_id();
    if (cpu >= SMP_MAX_CPUS) {
        cpu = 0;
    }
    return &idle_tasks[cpu];
}

tcb_t *task_get_idle_cpu(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) {
        return NULL;
    }
    return &idle_tasks[cpu];
}

uint64_t task_get_used_bitmap(void) {
    uint32_t crit = irq_spin_lock(&task_lock);
    uint64_t snapshot = (uint64_t)task_used_words[0] |
                        ((uint64_t)task_used_words[1] << 32);
    irq_spin_unlock(&task_lock, crit);
    return snapshot;
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

    /* Delay one tick so an already-running joiner can still observe the live
     * TERMINATED TCB.  Later joins use the separate generation-aware record. */
    uint32_t crit = irq_spin_lock(&task_lock);
    if (tcb->reclaim_at == 0) {
        tcb->reclaim_at = sched_get_tick_count() + 1;
    }
    irq_spin_unlock(&task_lock, crit);
}

void task_reclaim_expired(void) {
    uint32_t now = sched_get_tick_count();
    uint32_t crit = irq_spin_lock(&task_lock);
    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if (!task_id_is_used(id)) continue;

        tcb_t *tcb = &task_pool[id];
        if (tcb->state != TASK_STATE_TERMINATED) continue;
        if (tcb->reclaim_at == 0) continue;
        if (tcb->reclaim_at == TASK_RECLAIM_CLEANING) continue;
        if (now < tcb->reclaim_at) continue;
        if (tcb->joiners) continue;

        free_task_id(id);
        uint32_t next_gen = kobj_header_prepare_reuse(&tcb->hdr);
        memset(tcb, 0, sizeof(tcb_t));
        tcb->hdr.obj_type = CAP_OBJ_TASK;
        tcb->hdr.generation = next_gen;
    }
    irq_spin_unlock(&task_lock, crit);
}

kern_err_t task_terminate_with_result(tcb_t *tcb, kern_err_t result) {
    if (tcb == NULL || tcb->id < 0) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&task_lock);
    if (!task_id_is_used(tcb->id)) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_ERR_NOEXIST;
    }

    if (tcb->state == TASK_STATE_TERMINATED) {
        irq_spin_unlock(&task_lock, crit);
        return KERN_OK;
    }

    if (tcb->state == TASK_STATE_BLOCKED) {
        (void)task_unlink_blocked(tcb);
    }

    task_record_exit(tcb, NULL, result);
    /* The fault/self-termination path is still executing on this TCB.  Keep
     * it schedulable until task_finish_termination() can atomically publish
     * TERMINATED and wake joiners; otherwise PendSV can discard the cleanup
     * continuation exactly like the normal task_exit() path. */
    if (tcb != sched_get_current()) {
        tcb->state = TASK_STATE_TERMINATED;
    }
    tcb->reclaim_at = TASK_RECLAIM_CLEANING;
    irq_spin_unlock(&task_lock, crit);

    task_finish_termination(tcb, result);
    return KERN_OK;
}

void task_terminate(tcb_t *tcb) {
    (void)task_terminate_with_result(tcb, KERN_OK);
}

/*============================================================================
 * 栈溢出检测
 *============================================================================*/

void task_check_stack_overflow(void) {
    uint64_t snapshot = task_get_used_bitmap();
    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if (!(snapshot & (1ULL << id))) continue;

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
