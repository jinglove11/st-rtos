/**
 * @file kernel_types.h
 * @brief 内核类型定义
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel_config.h"
#include "kobject.h"   /* M2-Step3a: kobject_header_t */

/* Kconfig hides SMP_MAX_CPUS when SMP is disabled, while the scheduler keeps
 * one-element per-CPU arrays in the uniprocessor build. */
#ifndef SMP_MAX_CPUS
#define SMP_MAX_CPUS 1
#endif

/*============================================================================
 * 错误码
 *============================================================================*/

typedef enum {
    KERN_OK             =  0,    // 成功
    KERN_ERR            = -1,    // 一般错误
    KERN_ERR_PARAM      = -2,    // 参数错误
    KERN_ERR_TIMEOUT    = -3,    // 超时
    KERN_ERR_RESOURCE   = -4,    // 资源不足
    KERN_ERR_STATE      = -5,    // 状态错误
    KERN_ERR_ISR        = -6,    // ISR 上下文错误
    KERN_ERR_CAP        = -7,    // 能力不足
    KERN_ERR_BUSY       = -8,    // 忙
    KERN_ERR_NOEXIST    = -9,    // 对象不存在
    KERN_ERR_OVERFLOW   = -10,   // 溢出
    KERN_ERR_DEADLOCK   = -11,   // 死锁
    KERN_ERR_PERM       = -12,   // 权限不足
    KERN_ERR_NOTDIR     = -13,   // 不是目录
    KERN_ERR_ISDIR      = -14,   // 是目录 (不可作为文件操作)
    KERN_ERR_FAULT      = -15,   // 任务因 fault 终止
    KERN_ERR_NOSYS      = -16,   // 功能未实现/未启用 (e.g. FAULT_ENDPOINT off)
} kern_err_t;

/*============================================================================
 * 句柄类型
 *============================================================================*/

typedef int16_t task_id_t;       // 任务 ID
typedef int16_t sem_id_t;        // 信号量 ID
typedef int16_t mutex_id_t;      // 互斥锁 ID
typedef int16_t queue_id_t;      // 消息队列 ID
typedef int16_t event_id_t;      // 事件标志组 ID
typedef int16_t timer_id_t;      // 定时器 ID
/* Capability handles use bit 31 as the invalid/sign bit and keep every valid
 * handle positive.  The capability implementation currently assigns bits
 * [6:0] to the global slot and bits [30:7] to a non-wrapping generation. */
typedef int32_t         cap_id_t;        // 能力句柄 (slot + generation)
typedef int16_t ep_id_t;         // Endpoint ID
typedef int16_t ch_id_t;         // Channel ID

struct cnode;

#define KERN_INVALID_ID      (-1)
#define KERN_WAIT_FOREVER    UINT32_MAX

/*============================================================================
 * 任务状态
 *============================================================================*/

typedef enum {
    TASK_STATE_CREATED     = 0,  // 已创建，未启动
    TASK_STATE_READY       = 1,  // 就绪
    TASK_STATE_RUNNING     = 2,  // 运行中
    TASK_STATE_BLOCKED     = 3,  // 阻塞
    TASK_STATE_SUSPENDED   = 4,  // 挂起
    TASK_STATE_TERMINATED  = 5,  // 已终止
} task_state_t;

/* M1 SMP ownership model.  KERN_CPU_NONE is used while a task has not yet
 * been assigned to a run queue.  Migration state changes are deliberately
 * explicit so a TCB can never be runnable on two CPUs at the same time. */
#define KERN_CPU_NONE            UINT8_MAX
#define KERN_CPU_AFFINITY_ALL    ((1UL << SMP_MAX_CPUS) - 1UL)

typedef enum {
    TASK_MIGRATION_STABLE       = 0,
    TASK_MIGRATION_MIGRATING    = 1,
    TASK_MIGRATION_READY_REMOTE = 2,
} task_migration_state_t;

/* 任务属性 */
#define TASK_ATTR_PRIVILEGED   0x00   // 内核任务 (特权模式)
#define TASK_ATTR_USER         0x01   // 用户任务 (非特权模式)

/*============================================================================
 * 调度策略 (RT_SCHED)
 *============================================================================*/

typedef enum {
    SCHED_NORMAL   = 0,   // 普通时间片轮转 (默认)
    SCHED_FIFO     = 1,   // 实时 FIFO: 同优先级不轮转, 一直跑到阻塞/被更高抢占
    SCHED_RR       = 2,   // 实时轮转: 同优先级轮转 (RT 带时间片)
} sched_policy_t;

/* 优先级 band: 0..KERN_RT_PRIORITY_MAX = RT, 以上 = normal。
 * RT 任务的优先级永远高于 normal (因为调度器选最高优先级, RT band
 * 天然抢占 normal band)。这是 L4 RT/normal 分层的最小落地。 */
#define KERN_RT_PRIORITY_MAX   15

/*============================================================================
 * 任务阻塞原因
 *============================================================================*/

typedef enum {
    BLOCK_REASON_NONE      = 0,  // 未阻塞
    BLOCK_REASON_SEM       = 1,  // 等待信号量
    BLOCK_REASON_MUTEX     = 2,  // 等待互斥锁
    BLOCK_REASON_QUEUE     = 3,  // 等待消息队列
    BLOCK_REASON_EVENT     = 4,  // 等待事件
    BLOCK_REASON_TIMER     = 5,  // 等待定时器
    BLOCK_REASON_SLEEP     = 6,  // 延时
    BLOCK_REASON_JOIN      = 7,  // 等待任务结束
    BLOCK_REASON_IRQ       = 8,  // 等待线程化 IRQ 信号
    BLOCK_REASON_EP_SEND   = 9,  // 等待 endpoint 回复
    BLOCK_REASON_EP_RECV   = 10, // 等待 endpoint 请求
    BLOCK_REASON_CH_SEND   = 11, // 等待 channel 对端接收
    BLOCK_REASON_CH_RECV   = 12, // 等待 channel 对端发送
} block_reason_t;

/*============================================================================
 * 任务控制块 (TCB)
 *============================================================================*/

#if MPU_ENABLE && CAP_ENABLE
#define TASK_SHM_MAP_MAX 5
typedef struct {
    uint8_t  in_use;
    uint8_t  region;
    uint8_t  rights;
    uint8_t  _pad;
    cap_id_t cap;
    void    *addr;
    size_t   size;
} shm_mapping_t;
#endif

/*============================================================================
 * M3-Task3: 统一 continuation 状态机
 *
 * 替代原先散在 TCB 的 5 个阻塞字段:
 *   block_reason → cont.op
 *   block_obj    → cont.object
 *   block_result → cont.result
 *   wake_tick    → cont.deadline
 *   syscall_blocked → cont.active
 *
 * 加 3 个统一 helper: syscall_block_current / syscall_complete / syscall_cancel
 * SVC handler asm 不引用这些字段 (只用 sp/exc_return/attrs/sp_limit)。
 *============================================================================*/

typedef struct {
    /* --- 状态机字段 (替代散在 TCB 的 5 个标量) --- */
    uint8_t     active;         /* 1 = 阻塞中 (替代 syscall_blocked) */
    uint8_t     op;             /* block_reason_t (替代 block_reason) */
    uint16_t    flags;          /* 预留 */
    void       *object;         /* 等待的内核对象指针 (替代 block_obj) */
    uint32_t    deadline;       /* 超时 tick, 0=永久 (替代 wake_tick) */
    kern_err_t  result;         /* 唤醒结果 (替代 block_result) */

    /* --- payload (替代全局 side table) --- */
    void       *msg_buf;        /* user 消息缓冲区指针 (recv: out buf) */
    void       *reply_buf;      /* endpoint send: reply-out buffer */
    uint32_t    request_gen;    /* endpoint send: request generation */
    union {
        struct { cap_id_t *out_caps; uint8_t *out_cap_count; } ep_recv;
        struct { cap_id_t *out_caps; uint8_t *out_cap_count; } ch;
        struct { uint32_t wait_flags; uint32_t wait_opt; uint32_t *received; } event;
    } u;
} syscall_cont_t;

/* M3-Task3: 兼容宏已删除,所有引用改为 cont.* 直接访问。 */

typedef struct tcb {
    /* M2-Step3c: kobject_header_t 在 offset 0。
     * sp 移到 hdr 之后 (offset 12),asm 用 [rX, #OFF_SP] 访问
     * (tcb_offsets.inc 自动同步)。这是 cap_get_entry cross-check
     * "hdr 在 object 指针的 offset 0" 不变量的唯一例外处理方式。 */
    kobject_header_t hdr;

    // --- 上下文保存区 (汇编访问,偏移由 tcb_offsets.inc 自动生成) ---
    void       *sp;                   // 栈指针
    uint32_t    exc_return;           // 异常返回类型 (basic/extended FP frame)

    // --- 基本信息 ---
    char        name[KERN_TASK_NAME_LEN];  // 任务名称
    task_id_t   id;                   // 任务 ID
    uint8_t     priority;             // 当前优先级 (0 最高)
    uint8_t     base_priority;        // 基硎优先级 (用于优先级继承恢复)
    task_state_t state;               // 任务状态

    // --- 栈信息 ---
    void       *stack_base;           // 栈基址
    uint32_t    stack_size;           // 栈大小 (字节)
    uint32_t    sp_limit;             // PSP 下界 (装载到 PSPLIM, M33 栈溢出保护)
    uint32_t    fp_high[16];          // FPU S16-S31 (硬件仅自动保存 S0-S15)

    // --- 调度信息 ---
    uint32_t    time_slice;           // 剩余时间片
    uint32_t    time_slice_reload;    // 时间片重载值
    uint32_t    total_ticks;          // 总运行时间 (统计用)
    /* M3-Task3: wake_tick → cont.deadline (兼容宏) */
    uint32_t    affinity_mask;        // 允许运行的 CPU 位图
    uint8_t     cpu_owner;            // 所属运行队列/CPU，KERN_CPU_NONE=未分配
    uint8_t     migration_state;      // task_migration_state_t
    uint8_t     _smp_pad[2];          // 4 字节对齐

    // --- 阻塞信息 ---
    /* M3-Task3: block_reason/block_obj/block_result → cont.* (兼容宏) */

    /* --- join 支持 --- */
    void       *exit_value;           // task_exit 存储的返回值
    void       *join_value;           // 作为 joiner 时的返回值快照
    struct tcb *joiners;              // 等待此任务结束的链表头
    struct tcb *join_next;            // joiner 链表的 next 指针
    uint32_t    reclaim_at;           // 延迟回收时间戳 (tick), 0=不需要回收

    // --- 链表节点 (用于各种队列) ---
    struct tcb *next;                 // 下一个 (就绪队列)
    struct tcb *prev;                 // 前一个 (就绪队列)

    // --- 等待队列链表节点 ---
    struct tcb *wait_next;            // 下一个 (等待队列)
    struct tcb *wait_prev;            // 前一个 (等待队列)

    // --- M3-Task3: 统一 continuation payload ---
    syscall_cont_t cont;              // 阻塞 IPC 的 continuation 状态

    // --- MPU 内存保护 (Phase 1) ---
    uint8_t     attrs;                // TASK_ATTR_PRIVILEGED / TASK_ATTR_USER
    /* M3-Task3: syscall_blocked → cont.active (兼容宏) */
    uint8_t     sched_policy;         // SCHED_NORMAL / SCHED_FIFO / SCHED_RR
    uint8_t     _pad1[2];             // 4 字节对齐
#if MPU_ENABLE
    uint32_t    mpu_regions[8][2];   // MPU region [RBAR, RASR/RLAR] x 8
#if CAP_ENABLE
    shm_mapping_t shm_maps[TASK_SHM_MAP_MAX];
#endif
#endif

    // --- 能力 ---
#if CAP_ENABLE
    /* M2 CSpace: 叶 CNode 存储与 TCB 分离，TCB 只持有指针。
     * 这使 task slot 复用/清零 TCB 时，CNode 自己的 slot generation
     * 仍能持续递增，为后续 local CPtr ABI 提供不回绕基础。 */
    struct cnode *cspace;
#endif

    // --- 统计信息 ---
#if KERN_TASK_STATS
    uint32_t    ctx_switch_count;     // 上下文切换次数
    uint32_t    cpu_usage;            // CPU 使用率 (万分比)
#endif

    // --- 文件描述符表 ---
    /* Phase #22: fd_table 删除 (内核 VFS 移除,fd 由 fs_server 管理) */

} tcb_t;

/*============================================================================
 * 等待队列
 *============================================================================*/

typedef struct {
    tcb_t *head;                       // 队列头
    tcb_t *tail;                       // 队列尾
    uint16_t count;                    // 等待数量
} wait_queue_t;

/*============================================================================
 * 信号量
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;               // M2-Step3a: 对象 header (generation 等)
    uint32_t    count;                  // 当前计数
    uint32_t    max_count;              // 最大计数
    wait_queue_t wait_queue;            // 等待队列
    uint8_t     in_use;                 // 使用标志
} sem_t;

/*============================================================================
 * 互斥锁
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;               // M2-Step3a: 对象 header
    task_id_t   owner;                  // 持有者
    uint8_t     lock_count;             // 锁计数 (递归锁)
    uint8_t     owner_original_prio;    // 持有者原始优先级
    wait_queue_t wait_queue;            // 等待队列
    uint8_t     in_use;                 // 使用标志
} mutex_t;

/*============================================================================
 * 消息队列
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;               // M2-Step3a: 对象 header
    void       *buffer;                 // 消息缓冲区
    uint16_t    msg_size;               // 单条消息大小
    uint16_t    capacity;               // 容量
    uint16_t    count;                  // 当前消息数
    uint16_t    head;                   // 头指针 (写入)
    uint16_t    tail;                   // 尾指针 (读取)
    wait_queue_t send_queue;            // 发送等待队列
    wait_queue_t recv_queue;            // 接收等待队列
    uint8_t     in_use;                 // 使用标志
} mqueue_t;

/*============================================================================
 * 事件标志组
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;               // M2-Step3a: 对象 header
    uint32_t    flags;                  // 当前标志
    wait_queue_t wait_queue;            // 等待队列
    uint8_t     in_use;                 // 使用标志
} event_t;

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
    // --- M2-Step3a: 对象 header ---
    kobject_header_t hdr;                        // 对象 header (generation 等)

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

    // --- 通知信息 ---
    ep_id_t         notify_ep;                  // 到期通知 endpoint
    uint32_t        notify_badge;               // 到期通知 badge

    // --- 堆索引 ---
    int16_t         heap_index;                 // 在最小堆中的索引，-1 表示不在堆中

    // --- 标志 ---
    uint8_t         one_shot;                   // 单次触发标志
    uint8_t         in_use;                     // 使用标志
    uint8_t         stop_pending;               // 回调中请求了 stop
    uint8_t         delete_pending;             // 删除已请求
    uint8_t         notify_bound;               // 是否绑定 endpoint 通知
} timer_t;

/**
 * @brief 定时器命令消息
 */
typedef struct {
    timer_cmd_type_t    type;       // 命令类型
    timer_id_t          timer_id;   // 目标定时器
    uint32_t            param;      // 参数（周期、延迟等）
} timer_cmd_t;

/*============================================================================
 * 能力令牌
 *============================================================================*/

#if KERN_ENABLE_CAPABILITY
typedef struct {
    cap_id_t    token;                 // 32-bit 能力句柄
    uint8_t     rights;                // 权限位图
    task_id_t   owner;                 // 拥有者
    void       *object;                // 关联对象
} capability_t;
#endif

/*============================================================================
 * 函数类型
 *============================================================================*/

typedef void (*task_func_t)(void *arg);    // 任务函数
typedef void (*isr_func_t)(void);          // 中断服务函数

/*============================================================================
 * 中断线程描述符
 *============================================================================*/

#if IRQ_THREADED_ENABLE
typedef struct {
    task_id_t   task_id;               // 关联任务 ID
    int16_t     irq_num;               // IRQ 号
    task_func_t handler;               // 用户线程模式处理函数
    void       *arg;                   // 用户参数
    uint8_t     priority;              // 线程优先级
    uint8_t     in_use;                // 使用标志
    uint8_t     pending;               // ISR 已触发的待处理标志
    uint8_t     running;               // 线程 handler 正在执行
    uint8_t     stopping;              // release 已请求停止
} irq_thread_t;
#endif

/*============================================================================
 * 底半部 (Bottom Half)
 *============================================================================*/

typedef void (*bh_handler_t)(void *arg);

typedef struct {
    bh_handler_t handler;              // 处理函数
    void        *arg;                  // 参数
    ep_id_t      notify_ep;            // 通知 endpoint
    uint32_t     notify_badge;         // 通知 badge
    uint8_t      pending;              // 待处理标志
    uint8_t      in_use;               // 使用标志
    uint8_t      running;              // handler 正在执行
    uint8_t      delete_pending;       // 运行中删除，返回后释放
    uint8_t      notify_bound;         // 是否绑定 endpoint 通知
} bh_t;

/*============================================================================
 * 通用宏
 *============================================================================*/

#define ARRAY_SIZE(arr)        (sizeof(arr) / sizeof((arr)[0]))
#ifndef MIN
#define MIN(a, b)              ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)              ((a) > (b) ? (a) : (b))
#endif

#define ALIGN_UP(x, align)     (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align)   ((x) & ~((align) - 1))

#ifndef BIT
/* M2-#4: 1ULL 支持 n >= 32 (CSpace 64 slot bitmap 等)。
 * 32 位 ARM 上 unsigned long=32 位,BIT(32+) 是 UB。改用 1ULL。 */
#define BIT(n)                 (1ULL << (n))
#endif
#define SET_BIT(x, n)          ((x) |= BIT(n))
#define CLR_BIT(x, n)          ((x) &= ~BIT(n))
#define TGL_BIT(x, n)          ((x) ^= BIT(n))
#define GET_BIT(x, n)          (((x) >> (n)) & 1U)

#endif // KERNEL_TYPES_H
