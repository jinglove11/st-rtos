/**
 * @file spinlock.h
 * @brief 自旋锁实现
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

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
} irq_spinlock_t;

static inline void irq_spin_init(irq_spinlock_t *s) {
    spin_init(&s->lock);
    s->irq_state = 0;
}

static inline uint32_t irq_spin_lock(irq_spinlock_t *s) {
    // 保存中断状态并关闭中断
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i");

    // 获取自旋锁
    spin_lock(&s->lock);

    return primask;
}

static inline void irq_spin_unlock(irq_spinlock_t *s, uint32_t irq_state) {
    // 释放自旋锁
    spin_unlock(&s->lock);

    // 恢复中断状态
    __asm volatile("msr primask, %0" :: "r"(irq_state));
}

#endif // SPINLOCK_H
