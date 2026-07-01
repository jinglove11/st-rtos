/**
 * @file user_api.h
 * @brief 用户态通用 syscall 封装 — 所有 syscall 走 svc #1
 *
 * sys_call0(num)                         → R0=num
 * sys_call1(num, a1)                     → R0=num, R1=a1
 * sys_call2(num, a1, a2)                 → R0=num, R1=a1, R2=a2
 * sys_call3(num, a1, a2, a3)             → R0=num, R1=a1, R2=a2, R3=a3
 * sys_call4(num, a1, a2, a3, a4)         → + a4 on stack
 * sys_call5(num, a1, a2, a3, a4, a5)     → + a4, a5 on stack
 * sys_call6(num, a1, a2, a3, a4, a5, a6) → + a4, a5, a6 on stack
 *
 * 统一栈布局 (svc 硬件帧 + padding = 固定偏移):
 *   PSP+64: a4, PSP+68: a5, PSP+72: a6
 *
 * Wrappers push 16 bytes before SVC so PSP remains 8-byte aligned and the
 * Cortex-M exception entry does not insert an alignment padding word.
 * sys_call0-3 的 a4-a6 固定压 0，内核根据 argc 忽略。
 */

#ifndef USER_API_H
#define USER_API_H

#include "syscall.h"
#include "ipc_transfer.h"
#include "inode.h"

#if SYSCALL_ENABLE

/* 用 r4 压零值，r4 是 callee-saved，svc handler 会保存/恢复 */
#define PAD3  "mov.w r4, #0\n\t" \
              "push {r4}\n\t"   \
              "push {r4}\n\t"   \
              "push {r4}\n\t"   \
              "push {r4}\n\t"

#define UNPAD3 "add sp, sp, #16\n\t"

static inline int sys_call0(int num) {
    register int r0 __asm("r0") = num;
    __asm volatile(
        PAD3
        "svc #1\n\t"
        UNPAD3
        : "+r"(r0) :: "r4", "cc", "memory");
    return r0;
}

static inline int sys_call1(int num, int a1) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    __asm volatile(
        PAD3
        "svc #1\n\t"
        UNPAD3
        : "+r"(r0) : "r"(r1) : "r4", "cc", "memory");
    return r0;
}

static inline int sys_call2(int num, int a1, int a2) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    register int r2 __asm("r2") = a2;
    __asm volatile(
        PAD3
        "svc #1\n\t"
        UNPAD3
        : "+r"(r0) : "r"(r1), "r"(r2) : "r4", "cc", "memory");
    return r0;
}

static inline int sys_call3(int num, int a1, int a2, int a3) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    register int r2 __asm("r2") = a2;
    register int r3 __asm("r3") = a3;
    __asm volatile(
        PAD3
        "svc #1\n\t"
        UNPAD3
        : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "r4", "cc", "memory");
    return r0;
}

static inline int sys_call4(int num, int a1, int a2, int a3, int a4) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    register int r2 __asm("r2") = a2;
    register int r3 __asm("r3") = a3;
    register int r4 __asm("r4") = a4;
    __asm volatile(
        "mov.w r12, #0\n\t"
        "push {r12}\n\t"      /* alignment pad */
        "push {r12}\n\t"      /* a6 = 0 */
        "push {r12}\n\t"      /* a5 = 0 */
        "push {%4}\n\t"       /* a4 = r4 */
        "svc #1\n\t"
        "add sp, sp, #16\n\t"
        : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4)
        : "r12", "cc", "memory");
    return r0;
}

static inline int sys_call5(int num, int a1, int a2, int a3, int a4, int a5) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    register int r2 __asm("r2") = a2;
    register int r3 __asm("r3") = a3;
    register int r4 __asm("r4") = a4;
    register int r5 __asm("r5") = a5;
    __asm volatile(
        "mov.w r12, #0\n\t"
        "push {r12}\n\t"      /* alignment pad */
        "push {r12}\n\t"      /* a6 = 0 */
        "push {%5}\n\t"       /* a5 = r5 */
        "push {%4}\n\t"       /* a4 = r4 */
        "svc #1\n\t"
        "add sp, sp, #16\n\t"
        : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
        : "r12", "cc", "memory");
    return r0;
}

static inline int sys_call6(int num, int a1, int a2, int a3, int a4, int a5, int a6) {
    register int r0 __asm("r0") = num;
    register int r1 __asm("r1") = a1;
    register int r2 __asm("r2") = a2;
    register int r3 __asm("r3") = a3;
    register int r4 __asm("r4") = a4;
    register int r5 __asm("r5") = a5;
    register int r6 __asm("r6") = a6;
    __asm volatile(
        "mov.w r12, #0\n\t"
        "push {r12}\n\t"      /* alignment pad */
        "push {%6}\n\t"       /* a6 = r6 */
        "push {%5}\n\t"       /* a5 = r5 */
        "push {%4}\n\t"       /* a4 = r4 */
        "svc #1\n\t"
        "add sp, sp, #16\n\t"
        : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6)
        : "r12", "cc", "memory");
    return r0;
}

#undef PAD3
#undef UNPAD3

/*============================================================================
 * 任务管理 — 用户态内联封装
 *============================================================================*/

static inline int sys_task_yield(void) {
    return sys_call0(SYSCALL_TASK_YIELD);
}

static inline int sys_task_delay(int ticks) {
    return sys_call1(SYSCALL_TASK_DELAY, ticks);
}

static inline int sys_task_exit(void *retval) {
    return sys_call1(SYSCALL_TASK_EXIT, (int)(uintptr_t)retval);
}

static inline int sys_task_create(const char *name, void (*entry)(void *),
                                  void *arg, int priority, int stack_size) {
    return sys_call5(SYSCALL_TASK_CREATE,
                     (int)(uintptr_t)name, (int)(uintptr_t)entry,
                     (int)(uintptr_t)arg, priority, stack_size);
}

static inline int sys_task_start(int task_id) {
    return sys_call1(SYSCALL_TASK_START, task_id);
}

static inline int sys_task_suspend(int task_id) {
    return sys_call1(SYSCALL_TASK_SUSPEND, task_id);
}

static inline int sys_task_resume(int task_id) {
    return sys_call1(SYSCALL_TASK_RESUME, task_id);
}

static inline int sys_task_delete(int task_id) {
    return sys_call1(SYSCALL_TASK_DELETE, task_id);
}

static inline int sys_task_self(void) {
    return sys_call0(SYSCALL_TASK_SELF);
}

/*============================================================================
 * 信号量 — 用户态内联封装
 *============================================================================*/

static inline int sys_sem_create(int count, int max_count) {
    return sys_call2(SYSCALL_SEM_CREATE, count, max_count);
}

static inline int sys_sem_delete(int sem_id) {
    return sys_call1(SYSCALL_SEM_DELETE, sem_id);
}

static inline int sys_sem_wait(int sem_id, int timeout) {
    return sys_call2(SYSCALL_SEM_WAIT, sem_id, timeout);
}

static inline int sys_sem_post(int sem_id) {
    return sys_call1(SYSCALL_SEM_POST, sem_id);
}

/*============================================================================
 * 互斥锁 — 用户态内联封装
 *============================================================================*/

static inline int sys_mutex_create(void) {
    return sys_call0(SYSCALL_MUTEX_CREATE);
}

static inline int sys_mutex_delete(int mutex_id) {
    return sys_call1(SYSCALL_MUTEX_DELETE, mutex_id);
}

static inline int sys_mutex_lock(int mutex_id, int timeout) {
    return sys_call2(SYSCALL_MUTEX_LOCK, mutex_id, timeout);
}

static inline int sys_mutex_unlock(int mutex_id) {
    return sys_call1(SYSCALL_MUTEX_UNLOCK, mutex_id);
}

/*============================================================================
 * 消息队列 — 用户态内联封装
 *============================================================================*/

static inline int sys_mqueue_create(int msg_size, int capacity) {
    return sys_call2(SYSCALL_MQUEUE_CREATE, msg_size, capacity);
}

static inline int sys_mqueue_delete(int queue_id) {
    return sys_call1(SYSCALL_MQUEUE_DELETE, queue_id);
}

static inline int sys_mqueue_send(int queue_id, const void *msg, int timeout) {
    return sys_call3(SYSCALL_MQUEUE_SEND, queue_id, (int)(uintptr_t)msg, timeout);
}

static inline int sys_mqueue_recv(int queue_id, void *msg, int timeout) {
    return sys_call3(SYSCALL_MQUEUE_RECV, queue_id, (int)(uintptr_t)msg, timeout);
}

/*============================================================================
 * 事件标志组 — 用户态内联封装
 *============================================================================*/

static inline int sys_event_create(int init_flags) {
    return sys_call1(SYSCALL_EVENT_CREATE, init_flags);
}

static inline int sys_event_delete(int event_id) {
    return sys_call1(SYSCALL_EVENT_DELETE, event_id);
}

static inline int sys_event_set(int event_id, int flags) {
    return sys_call2(SYSCALL_EVENT_SET, event_id, flags);
}

static inline int sys_event_clear(int event_id, int flags) {
    return sys_call2(SYSCALL_EVENT_CLEAR, event_id, flags);
}

static inline int sys_event_get(int event_id) {
    return sys_call1(SYSCALL_EVENT_GET, event_id);
}

static inline int sys_event_wait(int event_id, int flags, int timeout) {
    return sys_call3(SYSCALL_EVENT_WAIT, event_id, flags, timeout);
}

/*============================================================================
 * 定时器 — 用户态内联封装
 *============================================================================*/

static inline int sys_timer_create(const char *name, void (*cb)(void *),
                                   void *arg, int one_shot) {
    return sys_call4(SYSCALL_TIMER_CREATE,
                     (int)(uintptr_t)name, (int)(uintptr_t)cb,
                     (int)(uintptr_t)arg, one_shot);
}

static inline int sys_timer_start(int timer_id, int period) {
    return sys_call2(SYSCALL_TIMER_START, timer_id, period);
}

static inline int sys_timer_bind(int timer_id, int ep_id, int badge) {
    return sys_call3(SYSCALL_TIMER_BIND, timer_id, ep_id, badge);
}

static inline int sys_irq_bind(int irq_cap, int ep_cap, int badge) {
    return sys_call3(SYSCALL_IRQ_BIND, irq_cap, ep_cap, badge);
}

/*============================================================================
 * Endpoint (C/S) — 用户态内联封装
 *============================================================================*/

static inline int sys_ep_create(const char *name, int msg_size, int max_pending) {
    return sys_call3(SYSCALL_EP_CREATE,
                     (int)(uintptr_t)name, msg_size, max_pending);
}

static inline int sys_ep_delete(int ep_id) {
    return sys_call1(SYSCALL_EP_DELETE, ep_id);
}

static inline int sys_ep_send(int ep_id, void *msg, int timeout) {
    return sys_call3(SYSCALL_EP_SEND, ep_id, (int)(uintptr_t)msg, timeout);
}

static inline int sys_ep_send_caps(int ep_id, void *msg,
                                   const ipc_cap_xfer_t *caps,
                                   int cap_count, int timeout) {
    return sys_call5(SYSCALL_EP_SEND_CAPS, ep_id, (int)(uintptr_t)msg,
                     (int)(uintptr_t)caps, cap_count, timeout);
}

static inline int sys_ep_recv(int ep_id, void *msg, int timeout) {
    return sys_call3(SYSCALL_EP_RECV, ep_id, (int)(uintptr_t)msg, timeout);
}

static inline int sys_ep_recv_caps(int ep_id, void *msg,
                                   cap_id_t *out_caps,
                                   uint8_t *out_cap_count,
                                   int timeout) {
    return sys_call5(SYSCALL_EP_RECV_CAPS, ep_id, (int)(uintptr_t)msg,
                     (int)(uintptr_t)out_caps,
                     (int)(uintptr_t)out_cap_count, timeout);
}

static inline int sys_ep_reply(int ep_id, const void *msg) {
    return sys_call2(SYSCALL_EP_REPLY, ep_id, (int)(uintptr_t)msg);
}

static inline int sys_ep_take_reply(int ep_id) {
    return sys_call1(SYSCALL_EP_TAKE_REPLY, ep_id);
}

static inline int sys_cap_revoke(int cap) {
    return sys_call1(SYSCALL_CAP_REVOKE, cap);
}

static inline int sys_cap_type(int cap) {
    return sys_call1(SYSCALL_CAP_TYPE, cap);
}

static inline int sys_cap_rights(int cap) {
    return sys_call1(SYSCALL_CAP_RIGHTS, cap);
}

/*============================================================================
 * Channel (P2P) — 用户态内联封装
 *============================================================================*/

static inline int sys_ch_create(int msg_size, int shm_size) {
    return sys_call2(SYSCALL_CH_CREATE, msg_size, shm_size);
}

static inline int sys_ch_delete(int ch_id) {
    return sys_call1(SYSCALL_CH_DELETE, ch_id);
}

static inline int sys_ch_connect(int ch_id, int peer_a, int peer_b) {
    return sys_call3(SYSCALL_CH_CONNECT, ch_id, peer_a, peer_b);
}

static inline int sys_ch_send(int ch_id, const void *msg, int timeout) {
    return sys_call3(SYSCALL_CH_SEND, ch_id, (int)(uintptr_t)msg, timeout);
}

static inline int sys_ch_send_caps(int ch_id, const void *msg,
                                   const ipc_cap_xfer_t *caps,
                                   int cap_count, int timeout) {
    return sys_call5(SYSCALL_CH_SEND_CAPS, ch_id, (int)(uintptr_t)msg,
                     (int)(uintptr_t)caps, cap_count, timeout);
}

static inline int sys_ch_recv(int ch_id, void *msg, int timeout) {
    return sys_call3(SYSCALL_CH_RECV, ch_id, (int)(uintptr_t)msg, timeout);
}

static inline int sys_ch_recv_caps(int ch_id, void *msg,
                                   cap_id_t *out_caps,
                                   uint8_t *out_cap_count,
                                   int timeout) {
    return sys_call5(SYSCALL_CH_RECV_CAPS, ch_id, (int)(uintptr_t)msg,
                     (int)(uintptr_t)out_caps,
                     (int)(uintptr_t)out_cap_count, timeout);
}

static inline void *sys_ch_get_shm(int ch_id) {
    return (void *)(uintptr_t)sys_call1(SYSCALL_CH_GET_SHM, ch_id);
}

/*============================================================================
 * 内存管理 — 用户态内联封装
 *============================================================================*/

static inline int sys_mem_alloc(int size) {
    return sys_call1(SYSCALL_MEM_ALLOC, size);
}

static inline int sys_mem_free(int mem_cap) {
    return sys_call1(SYSCALL_MEM_FREE, mem_cap);
}

static inline int sys_mem_size(int mem_cap) {
    return sys_call1(SYSCALL_MEM_SIZE, mem_cap);
}

static inline int sys_shm_create(int size, int rights) {
    return sys_call2(SYSCALL_SHM_CREATE, size, rights);
}

static inline void *sys_shm_map(int shm_cap, int rights) {
    return (void *)(uintptr_t)sys_call2(SYSCALL_SHM_MAP, shm_cap, rights);
}

static inline int sys_shm_unmap(int shm_cap) {
    return sys_call1(SYSCALL_SHM_UNMAP, shm_cap);
}

/*============================================================================
 * VFS 文件操作 — 用户态内联封装
 *============================================================================*/
#if VFS_ENABLE

static inline int open(const char *path, int flags) {
    return sys_call2(SYSCALL_OPEN, (int)(uintptr_t)path, flags);
}

static inline int close(int fd) {
    return sys_call1(SYSCALL_CLOSE, fd);
}

static inline int read(int fd, void *buf, int size) {
    return sys_call3(SYSCALL_READ, fd, (int)(uintptr_t)buf, size);
}

static inline int write(int fd, const void *buf, int size) {
    return sys_call3(SYSCALL_WRITE, fd, (int)(uintptr_t)buf, size);
}

static inline int ioctl(int fd, int cmd, void *arg) {
    return sys_call3(SYSCALL_IOCTL, fd, cmd, (int)(uintptr_t)arg);
}

static inline int lseek(int fd, int offset, int whence) {
    return sys_call3(SYSCALL_LSEEK, fd, offset, whence);
}

static inline int readdir(int fd, dirent_t *entry) {
    return sys_call2(SYSCALL_READDIR, fd, (int)(uintptr_t)entry);
}

static inline int unlink(const char *path) {
    return sys_call1(SYSCALL_UNLINK, (int)(uintptr_t)path);
}

static inline int mkdir(const char *path) {
    return sys_call1(SYSCALL_MKDIR, (int)(uintptr_t)path);
}

static inline int stat(const char *path, vfs_stat_t *st) {
    return sys_call2(SYSCALL_STAT, (int)(uintptr_t)path,
                     (int)(uintptr_t)st);
}

#endif /* VFS_ENABLE */

/*============================================================================
 * Phase 2 — fault endpoint subscription
 *============================================================================*/

/**
 * sys_fault_subscribe — return the kernel fault endpoint id.
 *
 * On success, returns ep_id (>= 0) that the caller can pass to
 * sys_ep_recv() to block for fault_event_t notifications. On failure,
 * returns a negative kern_err_t.
 *
 * Layout of the received message is fault_event_t (see fault_endpoint.h).
 * The supervisor task is responsible for rate-limiting restarts and
 * capability subset enforcement (Phase 2 S3, S4).
 */
static inline int sys_fault_subscribe(void) {
    return sys_call0(SYSCALL_FAULT_SUBSCRIBE);
}

/*============================================================================
 * Phase 2 §2.4 — task restart with capability subset
 *============================================================================*/

/**
 * sys_task_restart — recreate a faulted task with a reduced capability set.
 *
 * The kernel creates a new user task from (entry, arg, priority, stack_size),
 * derives a child capability from the caller's TASK cap with CAP_GRANT
 * stripped (rights masked by cap_rights_mask), installs it into the new
 * task's cspace, and starts the task.
 *
 * The caller must hold a TASK cap carrying CAP_GRANT (its self-cap, granted
 * at spawn time). If it does not, the task is still created/started but
 * receives no caps (degraded, detectable via the returned management cap).
 *
 * @param name            task name (copied from user)
 * @param entry           task entry function
 * @param arg             argument passed to entry
 * @param priority        task priority (0..KERNEL_MAX_PRIORITIES-1)
 * @param stack_size      stack size in bytes (clamped to [512, KERNEL_TASK_STACK_SIZE])
 * @param cap_rights_mask desired rights for the new task's cap; CAP_GRANT bit
 *                        is forced off regardless.
 * @return                management cap id (>=0) on success, or negative kern_err_t.
 */
static inline int sys_task_restart(const char *name, void (*entry)(void *),
                                   void *arg, int priority, int stack_size,
                                   int cap_rights_mask) {
    return sys_call6(SYSCALL_TASK_RESTART,
                     (int)(uintptr_t)name, (int)(uintptr_t)entry,
                     (int)(uintptr_t)arg, priority, stack_size,
                     cap_rights_mask);
}

#endif /* SYSCALL_ENABLE */

#endif /* USER_API_H */
