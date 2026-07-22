/**
 * @file spinlock.h
 * @brief 自旋锁实现
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "kernel_config.h"

/* Debug lock ordering.  Ranks must be acquired in strictly increasing order;
 * equal-rank nesting is intentionally rejected because M1 forbids nesting
 * peer object locks.  Rank 0 leaves legacy/unclassified locks unchecked. */
typedef enum {
    LOCKDEP_RANK_NONE       = 0,
    LOCKDEP_RANK_REGISTRY   = 10,
    LOCKDEP_RANK_TASK       = 20,
    LOCKDEP_RANK_OBJECT     = 30,
    LOCKDEP_RANK_RESOURCE   = 40,
    LOCKDEP_RANK_REMOTE     = 50,
} lockdep_rank_t;

#if KERN_DEBUG_ENABLE
void lockdep_check(uint8_t rank, const void *lock);
void lockdep_acquire(uint8_t rank, const void *lock);
void lockdep_release(uint8_t rank, const void *lock);
#else
static inline void lockdep_check(uint8_t rank, const void *lock) {
    (void)rank;
    (void)lock;
}
static inline void lockdep_acquire(uint8_t rank, const void *lock) {
    (void)rank;
    (void)lock;
}
static inline void lockdep_release(uint8_t rank, const void *lock) {
    (void)rank;
    (void)lock;
}
#endif

#if CAP_ENABLE
/* Drain capability cleanup/revoke callbacks at an outermost lock-release
 * safe point.  capability.c supplies the implementation; keeping the call in
 * irq_spin_unlock guarantees callbacks never inherit a ranked subsystem lock. */
void cap_deferred_poll(void);
#endif

typedef volatile uint32_t spinlock_t;

#define SPIN_LOCK_INIT      0
#define SPIN_LOCK_LOCKED    1

/**
 * @brief 初始化自旋锁
 */
static inline void spin_init(spinlock_t *lock) {
    *lock = SPIN_LOCK_INIT;
}

/**
 * @brief 获取自旋锁
 */
static inline void spin_lock(spinlock_t *lock) {
    // 使用 LDREX/STREX 实现原子操作
    uint32_t tmp;
    __asm volatile(
        "1: ldrex   %0, [%1]      \n"  // 独占读取
        "   cmp     %0, #0        \n"  // 检查是否已锁定
        "   bne     1b            \n"  // 如果已锁定，继续等待
        "   strex   %0, %2, [%1]  \n"  // 尝试获取锁
        "   cmp     %0, #0        \n"  // 检查是否成功
        "   bne     1b            \n"  // 如果失败，重试
        "   dmb                   \n"  // 数据内存屏障
        : "=&r"(tmp)
        : "r"(lock), "r"(SPIN_LOCK_LOCKED)
        : "memory", "cc"
    );
}

/**
 * @brief 释放自旋锁
 */
static inline void spin_unlock(spinlock_t *lock) {
    __asm volatile(
        "   dmb                   \n"  // 数据内存屏障
        "   str     %1, [%0]      \n"  // 释放锁
        :
        : "r"(lock), "r"(SPIN_LOCK_INIT)
        : "memory"
    );
}

/**
 * @brief 尝试获取自旋锁（非阻塞）
 * @return 0 成功获取，-1 锁已被占用
 */
static inline int spin_trylock(spinlock_t *lock) {
    uint32_t tmp;
    __asm volatile(
        "   ldrex   %0, [%1]      \n"  // 独占读取
        "   cmp     %0, #0        \n"  // 检查是否已锁定
        "   bne     2f            \n"  // 如果已锁定，跳转
        "   strex   %0, %2, [%1]  \n"  // 尝试获取锁
        "   cmp     %0, #0        \n"  // 检查是否成功
        "   bne     2f            \n"  // 如果失败，跳转
        "   dmb                   \n"  // 数据内存屏障
        "   mov     %0, #0        \n"  // 返回成功
        "   b       3f            \n"
        "2: mov     %0, #-1       \n"  // 返回失败
        "3:                       \n"
        : "=&r"(tmp)
        : "r"(lock), "r"(SPIN_LOCK_LOCKED)
        : "memory", "cc"
    );
    return (int)tmp;
}

/**
 * @brief 带中断保护的自旋锁
 */
typedef struct {
    spinlock_t lock;
    uint32_t irq_state;
    uint8_t rank;
    uint8_t _pad[3];
} irq_spinlock_t;

static inline void irq_spin_init(irq_spinlock_t *s) {
    spin_init(&s->lock);
    s->irq_state = 0;
    s->rank = LOCKDEP_RANK_NONE;
}

static inline void irq_spin_init_rank(irq_spinlock_t *s, uint8_t rank) {
    irq_spin_init(s);
    s->rank = rank;
}

static inline uint32_t irq_spin_lock(irq_spinlock_t *s) {
    /* Check order before potentially waiting.  While contended, restore the
     * caller's PRIMASK between try-lock attempts so a scheduler IPI can break
     * cross-core dependencies (e.g. a remote quiesce).  Interrupts remain
     * masked for the entire interval after the lock is actually acquired. */
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    lockdep_check(s->rank, s);

    for (;;) {
        __asm volatile("cpsid i");
        if (spin_trylock(&s->lock) == 0) {
            break;
        }
        __asm volatile("msr primask, %0" :: "r"(primask));
        __asm volatile("yield");
    }

    lockdep_acquire(s->rank, s);
    return primask;
}

static inline void irq_spin_unlock(irq_spinlock_t *s, uint32_t irq_state) {
    lockdep_release(s->rank, s);

    // 释放自旋锁
    spin_unlock(&s->lock);

    // 恢复中断状态
    __asm volatile("msr primask, %0" :: "r"(irq_state));

#if CAP_ENABLE
    /* A saved PRIMASK of zero means this was the outermost irq_spinlock.
     * Poll only after restoring it, so deferred callbacks start with no
     * ranked lock held.  Nested unlocks leave the work for their outer owner. */
    if (irq_state == 0U) {
        cap_deferred_poll();
    }
#endif
}

#endif // SPINLOCK_H
