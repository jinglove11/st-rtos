/**
 * @file syscall.h
 * @brief 系统调用接口 — 编号 + 注册宏 + 分发入口
 *
 * 所有内核侧 handler 统一签名为 6 参数。
 * SYSDEF 宏记录期望 argc (0-6)，内核据此忽略多余参数。
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include "kernel_types.h"

#define KERN_SYSCALL_BLOCKED    (-128)

#if SYSCALL_ENABLE

/*============================================================================
 * Syscall 编号
 *============================================================================*/

/* 任务管理 */
#define SYSCALL_TASK_YIELD       0
#define SYSCALL_TASK_DELAY       1
#define SYSCALL_TASK_EXIT        2
#define SYSCALL_TASK_CREATE      3
#define SYSCALL_TASK_START       4
#define SYSCALL_TASK_SUSPEND     5
#define SYSCALL_TASK_RESUME      6
#define SYSCALL_TASK_DELETE      7
#define SYSCALL_TASK_SELF        8

/* IPC — 信号量 */
#define SYSCALL_SEM_CREATE       9
#define SYSCALL_SEM_WAIT         10
#define SYSCALL_SEM_POST         11
#define SYSCALL_SEM_DELETE       12

/* IPC — 互斥锁 */
#define SYSCALL_MUTEX_CREATE     13
#define SYSCALL_MUTEX_LOCK       14
#define SYSCALL_MUTEX_UNLOCK     15

/* IPC — 消息队列 */
#define SYSCALL_MQUEUE_CREATE    16
#define SYSCALL_MQUEUE_SEND      17
#define SYSCALL_MQUEUE_RECV      18

/* IPC — 事件 */
#define SYSCALL_EVENT_CREATE     19
#define SYSCALL_EVENT_WAIT       20
#define SYSCALL_EVENT_SET        21

/* 定时器 */
#define SYSCALL_TIMER_CREATE     22
#define SYSCALL_TIMER_START      23

/* 中断 */
#define SYSCALL_IRQ_REGISTER     24
#define SYSCALL_BH_CREATE        25
#define SYSCALL_BH_SCHEDULE      26

/* 内存 */
#define SYSCALL_MEM_ALLOC        27
#define SYSCALL_MEM_FREE         28

/* 能力管理 */
#define SYSCALL_CAP_DERIVE       29
#define SYSCALL_CAP_TRANSFER     30  /* legacy raw-target ABI; kernel only */
#define SYSCALL_CAP_REVOKE       31

/* VFS — reserved(内核 VFS 已删,P0-6):编号永不复用,槽位恒返回 NOSYS */
#define SYSCALL_OPEN             32  /* reserved (former VFS) */
#define SYSCALL_CLOSE            33  /* reserved (former VFS) */
#define SYSCALL_READ             34  /* reserved (former VFS) */
#define SYSCALL_WRITE            35  /* reserved (former VFS) */
#define SYSCALL_IOCTL            36  /* reserved (former VFS) */
#define SYSCALL_LSEEK            37  /* reserved (former VFS) */

/* IPC — 补全 */
#define SYSCALL_MUTEX_DELETE     38
#define SYSCALL_MQUEUE_DELETE    39
#define SYSCALL_EVENT_DELETE     40
#define SYSCALL_EVENT_CLEAR      41
#define SYSCALL_EVENT_GET        42

/* IPC — Endpoint */
#define SYSCALL_EP_CREATE        43
#define SYSCALL_EP_DELETE        44
#define SYSCALL_EP_SEND          45
#define SYSCALL_EP_RECV          46
#define SYSCALL_EP_REPLY         47

/* IPC — Channel */
#define SYSCALL_CH_CREATE        48
#define SYSCALL_CH_DELETE        49
#define SYSCALL_CH_CONNECT       50
#define SYSCALL_CH_SEND          51
#define SYSCALL_CH_RECV          52
#define SYSCALL_CH_GET_SHM       53

/* IPC — Capability-bearing payloads */
#define SYSCALL_EP_SEND_CAPS     54
#define SYSCALL_EP_RECV_CAPS     55
#define SYSCALL_CH_SEND_CAPS     56
#define SYSCALL_CH_RECV_CAPS     57
#define SYSCALL_EP_TAKE_REPLY    58
#define SYSCALL_TIMER_BIND       59
#define SYSCALL_MEM_SIZE         60
#define SYSCALL_SHM_MAP          61
#define SYSCALL_SHM_UNMAP        62
#define SYSCALL_SHM_CREATE       63
#define SYSCALL_CAP_TYPE         64
#define SYSCALL_CAP_RIGHTS       65
#define SYSCALL_IRQ_BIND         66
/* VFS — reserved(内核 VFS 已删,P0-6):编号永不复用,槽位恒返回 NOSYS */
#define SYSCALL_READDIR          67  /* reserved (former VFS) */
#define SYSCALL_UNLINK           68  /* reserved (former VFS) */
#define SYSCALL_MKDIR            69  /* reserved (former VFS) */
#define SYSCALL_STAT             70  /* reserved (former VFS) */
#define SYSCALL_FAULT_SUBSCRIBE  71
#define SYSCALL_TASK_RESTART     72
#define SYSCALL_GET_TICK         73
#define SYSCALL_FLASH_OP         74
#define SYSCALL_MMIO_MAP         75
#define SYSCALL_MMIO_UNMAP       76
#define SYSCALL_MMIO_REQUEST     77
#define SYSCALL_TASK_SET_POLICY  78
#define SYSCALL_MEM_MAP          79            /* compatibility: map Frame into task MPU */
#define SYSCALL_CAP_TRANSFER_TO  80            /* transfer cap to task (via task cap) */
#define SYSCALL_EP_SENDER        81            /* get last IPC sender task id */
#define SYSCALL_CAP_SELF_SLOT    82            /* M2-Step2c: find nth cap of obj_type in self CSpace */
#define SYSCALL_CAP_MINT         83            /* M2-#7: derive cap with rights subset + badge */
#define SYSCALL_CAP_BADGE        84            /* M2-#7: read badge of a cap */
#define SYSCALL_FACTORY_CREATE   85            /* M2: capability-authorized object creation */
#define SYSCALL_ABI_VERSION      86            /* M3-Step1: query kernel ABI version */
#define SYSCALL_NTFN_CREATE      87            /* P1-1: notification create (full cap) */
#define SYSCALL_NTFN_SIGNAL      88            /* P1-1: signal via badged cap */
#define SYSCALL_NTFN_WAIT        89            /* P1-1: block until word!=0, consume it */
#define SYSCALL_NTFN_POLL        90            /* P1-1: consume word without blocking */
#define SYSCALL_NTFN_DELETE      91            /* P1-1: delete object (CAP_MANAGE) */

/*============================================================================
 * Syscall 分发
 *============================================================================*/

/**
 * @brief 内核侧 — 统一 6-arg 签名
 */
typedef int (*syscall_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6);

/** 表项: handler + 期望参数个数 (0-6) */
typedef struct {
    syscall_fn_t handler;
    uint8_t      argc;
} syscall_entry_t;

/**
 * @brief 注册宏
 * @param num   syscall 编号
 * @param fn    处理函数 (6-arg 签名)
 * @param _argc 用户参数个数 (0=sys_call0, ..., 6=sys_call6)
 */
#define SYSDEF(num, fn, _argc) \
    [num] = { .handler = (syscall_fn_t)(fn), .argc = (_argc) }

/**
 * @brief 内核 syscall 分发入口 (由 context.S SVC #0x80 调用)
 *
 * @param syscall_num  syscall 编号 (来自用户 R0)
 * @param a1-a6        用户参数 (a1-a3=R1-R3, a4-a6=用户栈)
 */
int kern_syscall_handler(uint32_t syscall_num,
                                 uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6);

#endif /* SYSCALL_ENABLE */

#endif /* SYSCALL_H */
