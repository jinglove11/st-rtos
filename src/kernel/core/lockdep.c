/**
 * @file lockdep.c
 * @brief Small per-CPU lock-order validator for debug SMP builds.
 */

#include "spinlock.h"

#if KERN_DEBUG_ENABLE

#include "hal.h"
#include "kernel_types.h"

#ifndef SMP_MAX_CPUS
#define SMP_MAX_CPUS 1
#endif

#define LOCKDEP_MAX_DEPTH 16U

typedef struct {
    const void *locks[LOCKDEP_MAX_DEPTH];
    uint8_t ranks[LOCKDEP_MAX_DEPTH];
    uint8_t depth;
} lockdep_cpu_t;

static lockdep_cpu_t lockdep_cpu[SMP_MAX_CPUS];

static lockdep_cpu_t *lockdep_state(uint32_t cpu) {
    return &lockdep_cpu[(cpu < SMP_MAX_CPUS) ? cpu : 0U];
}

static void lockdep_put_hex(uintptr_t value) {
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = (int)(sizeof(value) * 8U) - 4; shift >= 0; shift -= 4) {
        hal_debug_putc(hex[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static const void *lockdep_return_address(void *address) {
    return __builtin_extract_return_addr(address);
}

static void lockdep_fail(const char *reason, uint32_t cpu,
                         const lockdep_cpu_t *state, uint8_t new_rank,
                         const void *new_lock, const void *return_address) {
    extern void kern_panic(const char *msg);
    uint8_t held_rank = LOCKDEP_RANK_NONE;
    const void *held_lock = NULL;
    uintptr_t ra = (uintptr_t)lockdep_return_address((void *)return_address);

    if (state->depth > 0U && state->depth <= LOCKDEP_MAX_DEPTH) {
        held_rank = state->ranks[state->depth - 1U];
        held_lock = state->locks[state->depth - 1U];
    }

    /* Cortex-M return addresses carry the Thumb bit.  The halfword before the
     * normalized return address lies in the call instruction and is directly
     * useful with arm-none-eabi-addr2line. */
    uintptr_t callsite = ra & ~(uintptr_t)1U;
    if (callsite >= 2U) {
        callsite -= 2U;
    }

    hal_debug_puts("\r\n[LOCKDEP] ");
    hal_debug_puts(reason);
    hal_debug_puts("\r\n  cpu=0x");
    lockdep_put_hex(cpu);
    hal_debug_puts(" depth=0x");
    lockdep_put_hex(state->depth);
    hal_debug_puts(" held_rank=0x");
    lockdep_put_hex(held_rank);
    hal_debug_puts(" held_lock=0x");
    lockdep_put_hex((uintptr_t)held_lock);
    hal_debug_puts("\r\n  new_rank=0x");
    lockdep_put_hex(new_rank);
    hal_debug_puts(" new_lock=0x");
    lockdep_put_hex((uintptr_t)new_lock);
    hal_debug_puts(" return_address=0x");
    lockdep_put_hex(ra);
    hal_debug_puts("\r\n  addr2line_address=0x");
    lockdep_put_hex(callsite);
    hal_debug_puts("\r\n");
    kern_panic("lock order violation");
}

static void lockdep_check_state(uint8_t rank, const void *lock, uint32_t cpu,
                                lockdep_cpu_t *state,
                                const void *return_address) {
    if (state->depth >= LOCKDEP_MAX_DEPTH) {
        lockdep_fail("nesting depth overflow", cpu, state, rank, lock,
                     return_address);
        return;
    }
    if (state->depth > 0U && rank <= state->ranks[state->depth - 1U]) {
        lockdep_fail("rank inversion or peer-lock nesting", cpu, state, rank,
                     lock, return_address);
    }
}

__attribute__((noinline))
void lockdep_check(uint8_t rank, const void *lock) {
    if (rank == LOCKDEP_RANK_NONE) {
        return;
    }

    uint32_t cpu = hal_get_cpu_id();
    lockdep_check_state(rank, lock, cpu, lockdep_state(cpu),
                        __builtin_return_address(0));
}

__attribute__((noinline))
void lockdep_acquire(uint8_t rank, const void *lock) {
    if (rank == LOCKDEP_RANK_NONE) {
        return;
    }

    uint32_t cpu = hal_get_cpu_id();
    lockdep_cpu_t *state = lockdep_state(cpu);
    lockdep_check_state(rank, lock, cpu, state, __builtin_return_address(0));

    state->locks[state->depth] = lock;
    state->ranks[state->depth] = rank;
    state->depth++;
}

__attribute__((noinline))
void lockdep_release(uint8_t rank, const void *lock) {
    if (rank == LOCKDEP_RANK_NONE) {
        return;
    }

    uint32_t cpu = hal_get_cpu_id();
    lockdep_cpu_t *state = lockdep_state(cpu);
    if (state->depth == 0U ||
        state->locks[state->depth - 1U] != lock ||
        state->ranks[state->depth - 1U] != rank) {
        lockdep_fail("unlock is not LIFO", cpu, state, rank, lock,
                     __builtin_return_address(0));
        return;
    }
    state->depth--;
}

#endif
