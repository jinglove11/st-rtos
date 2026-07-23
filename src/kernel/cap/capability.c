/**
 * @file capability.c
 * @brief Capability system — slot/generation handles + revoke tree
 */

#include "capability.h"
#include "kobject.h"
#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include "spinlock.h"
#include <string.h>

#if CAP_ENABLE

#define CAP_MAX_COUNT_VAL  CAP_MAX_COUNT
#define CAP_INVALID        ((cap_id_t)-1)
#define CAP_SLOT_BITS      7
#define CAP_SLOT_MASK      ((1U << CAP_SLOT_BITS) - 1U)
/* bit 30 distinguishes a user-visible local CPtr from an internal backing
 * handle. bit 31 stays clear, so all valid handles are positive and -1 is
 * always invalid. Internal handles use bits 0..29 only. */
#define CAP_GENERATION_MAX UINT32_C(0x007FFFFF)
#define CAP_NO_SLOT        ((int16_t)-1)
#define CAP_CNODE_SLOT_NONE UINT8_MAX

/* Local CPtr: marker[30] | generation[29:13] | root-CNode[12:7] | slot[6:0].
 * Root-CNode is the persistent task/CNode pool index, not a global cap slot. */
#define CAP_LOCAL_MARK             UINT32_C(0x40000000)
#define CAP_LOCAL_CNODE_BITS       6U
#define CAP_LOCAL_CNODE_MASK       ((1U << CAP_LOCAL_CNODE_BITS) - 1U)
#define CAP_LOCAL_CNODE_SHIFT      CAP_SLOT_BITS
#define CAP_LOCAL_GENERATION_SHIFT (CAP_SLOT_BITS + CAP_LOCAL_CNODE_BITS)
#define CAP_LOCAL_GENERATION_MAX   UINT32_C(0x0001FFFF)
#define CAP_CNODE_GENERATION_RETIRED \
    (CAP_LOCAL_GENERATION_MAX + UINT32_C(1))

/* Phase #3: cap_pool 自旋锁 (SMP 安全)。
 * 单核下 irq_spinlock 退化为关中断 (与之前隐式行为一致)。
 * SMP 下两核同时操作 cap_pool 互斥。
 * 用法: uint32_t crit = CAP_LOCK(); ... CAP_UNLOCK(crit); */
static irq_spinlock_t cap_pool_lock;
static cap_entry_t cap_pool[CAP_MAX_COUNT_VAL];
/* CNode storage is deliberately outside tcb_t.  TCB reclamation may memset a
 * task slot, while CNode slot generations must survive that reuse. */
static cnode_t task_cspaces[KERNEL_MAX_TASKS];

/* M2-#9: cleanup/revoke hook table (前向声明,供 deferred 队列引用)。
 * 真正定义在下方。 */
static cap_cleanup_fn_t cap_cleanup_table[CAP_OBJ_TYPE_MAX];
static cap_revoke_hook_fn_t cap_revoke_hook_table[CAP_OBJ_TYPE_MAX];

/*============================================================================
 * M2-#9: cleanup/revoke hook 延迟执行队列
 *
 * 问题: cap_clear_slot 在持 CAP_LOCK 时调 cleanup/revoke hook。hook 可能
 * 反向获取子系统锁 (如 mem.c 的 mem_lock),与已持有的子系统锁形成嵌套
 * 死锁。当前 hook (kmem_cap_cleanup / kshm_unmap) 不获取子系统锁,但
 * 未来 hook 可能会。
 *
 * 修复: cap_clear_slot 把需要调的 hook 记到 pending 队列 (不直接调)。
 * 最外层 irq_spin_unlock 在所有子系统锁释放后 flush 队列。仅仅离开
 * cap_pool_lock 还不够：调用者可能仍持有 ep_lock 等对象锁，此时执行
 * IRQ cleanup 会形成 OBJECT -> REGISTRY 的锁序反转。
 * pending 队列由 cap_pool_lock 保护。不能使用 per-CPU 队列：
 * CAP_UNLOCK 恢复中断后任务可在 flush 前迁核，会把旧 CPU 的
 * hook 永久留在队列中。单一 FIFO 队列再加一个非阻塞 drainer
 * 锁，保证 SMP 下 hook 不丢失、不并发，且 revoke hook 总在
 * 同对象的 cleanup 前执行。
 *============================================================================*/
#define CAP_DEFERRED_MAX (CAP_MAX_COUNT_VAL * 2U)
typedef struct {
    void     *object;
    uint8_t   obj_type;
    uint8_t   kind;        /* 0=cleanup, 1=revoke_hook */
    cap_id_t  cap;         /* revoke_hook 用 */
    cap_cleanup_fn_t cleanup_fn;
    cap_revoke_hook_fn_t revoke_fn;
} cap_deferred_t;

static cap_deferred_t cap_deferred_queue[CAP_DEFERRED_MAX];
static uint16_t cap_deferred_head;
static uint16_t cap_deferred_count;
static spinlock_t cap_deferred_flush_lock;
static uint32_t cap_deferred_pending;
static uint32_t cap_deferred_owner_cpu;

#define CAP_DEFERRED_OWNER_NONE UINT32_MAX

static void cap_defer_hook(void *object, uint8_t obj_type, uint8_t kind, cap_id_t cap) {
    cap_cleanup_fn_t cleanup_fn = NULL;
    cap_revoke_hook_fn_t revoke_fn = NULL;

    /* cap_defer_hook is only called while cap_pool_lock is held.  Snapshot the
     * registered callback now, so an event cannot accidentally use a callback
     * registered later for an unrelated object at the same stack address. */
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return;
    }
    if (kind == 0U) {
        cleanup_fn = cap_cleanup_table[obj_type];
        if (cleanup_fn == NULL) {
            return;
        }
    } else {
        revoke_fn = cap_revoke_hook_table[obj_type];
        if (revoke_fn == NULL) {
            return;
        }
    }

    if (cap_deferred_count < CAP_DEFERRED_MAX) {
        uint16_t i = (uint16_t)((cap_deferred_head + cap_deferred_count) %
                                CAP_DEFERRED_MAX);
        cap_deferred_queue[i].object     = object;
        cap_deferred_queue[i].obj_type   = obj_type;
        cap_deferred_queue[i].kind       = kind;
        cap_deferred_queue[i].cap        = cap;
        cap_deferred_queue[i].cleanup_fn = cleanup_fn;
        cap_deferred_queue[i].revoke_fn  = revoke_fn;
        cap_deferred_count++;
        __atomic_store_n(&cap_deferred_pending, 1U, __ATOMIC_RELEASE);
    }
#if KERN_DEBUG_ENABLE
    else {
        extern void kern_panic(const char *msg);
        kern_panic("cap deferred queue overflow");
    }
#endif
}

/* Execute pending callbacks only at a safe point with no ranked lock held.
 * Hardware IRQ/PendSV/SysTick contexts leave the work pending: cleanup hooks
 * may allocate, unmap or acquire ordinary kernel locks.  Thread mode and SVC
 * are valid synchronous kernel-call contexts. */
void cap_deferred_poll(void) {
    if (__atomic_load_n(&cap_deferred_pending, __ATOMIC_ACQUIRE) == 0U) {
        return;
    }

    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr != 0U && ipsr != 11U) { /* 11 = SVCall */
        return;
    }

    uint32_t cpu = hal_get_cpu_id();
    if (cpu >= SMP_MAX_CPUS) {
        cpu = 0U;
    }

    /* A nested cap operation from one of our own callbacks must return to the
     * outer drainer.  A different CPU, however, waits for/acquires the drainer
     * so synchronous cap_delete/revoke calls do not return before their
     * cleanup hook has actually freed the object (observable for SHM memory
     * accounting).  Polling is only entered with no ranked lock held, making
     * this cross-core serialization safe. */
    for (;;) {
        if (spin_trylock(&cap_deferred_flush_lock) == 0) {
            __atomic_store_n(&cap_deferred_owner_cpu, cpu, __ATOMIC_RELEASE);
            break;
        }
        if (__atomic_load_n(&cap_deferred_owner_cpu, __ATOMIC_ACQUIRE) == cpu) {
            return;
        }
        __asm volatile("yield");
    }

    for (;;) {
        cap_deferred_t d;
        uint32_t crit = irq_spin_lock(&cap_pool_lock);
        if (cap_deferred_count == 0U) {
            /* Clear pending while cap_pool_lock excludes producers, then drop
             * the drainer before CAP unlock.  The unlock's recursive poll sees
             * pending=0; a later producer necessarily publishes pending=1. */
            __atomic_store_n(&cap_deferred_pending, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&cap_deferred_owner_cpu,
                             CAP_DEFERRED_OWNER_NONE, __ATOMIC_RELEASE);
            spin_unlock(&cap_deferred_flush_lock);
            irq_spin_unlock(&cap_pool_lock, crit);
            return;
        }
        d = cap_deferred_queue[cap_deferred_head];
        cap_deferred_head = (uint16_t)((cap_deferred_head + 1U) %
                                       CAP_DEFERRED_MAX);
        cap_deferred_count--;
        irq_spin_unlock(&cap_pool_lock, crit);

        if (d.kind == 1U) {
            d.revoke_fn(d.cap, d.object, d.obj_type);
        } else {
            d.cleanup_fn(d.object, d.obj_type);
        }
    }
}

#define CAP_LOCK()   irq_spin_lock(&cap_pool_lock)
/* irq_spin_unlock polls the global deferred FIFO only at the outermost safe
 * point.  If CAP_LOCK is nested under a subsystem lock, its saved PRIMASK is
 * one and the outer lock's eventual unlock performs the drain. */
#define CAP_UNLOCK(crit) irq_spin_unlock(&cap_pool_lock, crit)

typedef char cap_slot_bits_fit[(CAP_MAX_COUNT_VAL <= (1U << CAP_SLOT_BITS)) ? 1 : -1];
typedef char cap_cnode_index_bits_fit[
    (KERNEL_MAX_TASKS <= (1U << CAP_LOCAL_CNODE_BITS)) ? 1 : -1];
typedef char cap_shm_type_registered[(CAP_OBJ_SHM < CAP_OBJ_TYPE_MAX) ? 1 : -1];

/* cap_cleanup_table / cap_revoke_hook_table 已在上方 M2-#9 deferred 队列前声明 */

#define CAP_TASK_CSPACE_SLOTS ((int)KERN_TASK_CAP_SLOTS)

typedef char cap_task_cspace_bitmap_fits[
    (KERN_TASK_CAP_SLOTS > 0 && KERN_TASK_CAP_SLOTS <= 64) ? 1 : -1];

static cap_id_t cap_encode(uint16_t slot, uint32_t generation) {
    if (slot >= CAP_MAX_COUNT_VAL || generation == 0 ||
        generation > CAP_GENERATION_MAX) {
        return CAP_INVALID;
    }
    return (cap_id_t)((generation << CAP_SLOT_BITS) | (uint32_t)slot);
}

static int cap_decode(cap_id_t cap, uint32_t *slot, uint32_t *generation) {
    uint32_t raw;

    if (cap == CAP_INVALID || cap < 0 ||
        (((uint32_t)cap & CAP_LOCAL_MARK) != 0)) {
        return 0;
    }

    raw = (uint32_t)cap;
    *slot = raw & CAP_SLOT_MASK;
    *generation = raw >> CAP_SLOT_BITS;
    if (*slot >= CAP_MAX_COUNT_VAL || *generation == 0 ||
        *generation > CAP_GENERATION_MAX) {
        return 0;
    }
    return 1;
}

int cap_is_local_cptr(cap_id_t cap) {
    return cap >= 0 && (((uint32_t)cap & CAP_LOCAL_MARK) != 0);
}

static cap_id_t cap_local_encode(uint8_t cnode_index, uint8_t slot,
                                 uint32_t generation) {
    if (cnode_index >= KERNEL_MAX_TASKS || slot >= KERN_TASK_CAP_SLOTS ||
        generation == 0 || generation > CAP_LOCAL_GENERATION_MAX) {
        return CAP_INVALID;
    }
    return (cap_id_t)(CAP_LOCAL_MARK |
                      (generation << CAP_LOCAL_GENERATION_SHIFT) |
                      ((uint32_t)cnode_index << CAP_LOCAL_CNODE_SHIFT) |
                      (uint32_t)slot);
}

static int cap_local_decode(cap_id_t cap, uint8_t *cnode_index,
                            uint8_t *slot, uint32_t *generation) {
    if (!cap_is_local_cptr(cap) || cnode_index == NULL || slot == NULL ||
        generation == NULL) {
        return 0;
    }

    uint32_t raw = (uint32_t)cap;
    *slot = (uint8_t)(raw & CAP_SLOT_MASK);
    *cnode_index = (uint8_t)((raw >> CAP_LOCAL_CNODE_SHIFT) &
                             CAP_LOCAL_CNODE_MASK);
    *generation = (raw >> CAP_LOCAL_GENERATION_SHIFT) &
                  CAP_LOCAL_GENERATION_MAX;
    return *slot < KERN_TASK_CAP_SLOTS &&
           *cnode_index < KERNEL_MAX_TASKS && *generation != 0;
}

static cap_entry_t *cap_get_entry(cap_id_t cap) {
    uint32_t slot;
    uint32_t generation;

    if (!cap_decode(cap, &slot, &generation)) {
        return NULL;
    }

    cap_entry_t *entry = &cap_pool[slot];
    if (!entry->in_use || entry->retired ||
        entry->generation != generation) {
        return NULL;
    }

    /* M2-Step3a: 对象 generation cross-check。
     * obj_generation=0 (栈/堆临时对象,如 test_capability.c 的 &test_obj)
     * 跳过校验,保持向后兼容。
     * obj_generation>0 (真池对象 sem/mutex/.../timer 等,header 在 offset 0)
     * 要求 entry->object 头部的 kobject_header_t.generation 与创建时一致。
     * 对象 slot 复用 + bump generation 后,旧 cap 在此失效。 */
    if (entry->obj_generation != 0 && entry->object != NULL) {
        const kobject_header_t *kh = (const kobject_header_t *)entry->object;
        if (kobj_generation_is_retired(kh->generation) ||
            kh->generation != entry->obj_generation) {
            return NULL;
        }
    }

    return entry;
}

static int cap_slot_of(const cap_entry_t *entry) {
    if (entry == NULL) {
        return -1;
    }
    return (int)(entry - cap_pool);
}

static cap_id_t cap_backing_of_entry(const cap_entry_t *entry) {
    int slot = cap_slot_of(entry);
    if (slot < 0) {
        return CAP_INVALID;
    }
    return cap_encode((uint16_t)slot, entry->generation);
}

static tcb_t *cap_task_by_id(uint8_t task_id) {
    if (task_id >= KERNEL_MAX_TASKS) {
        return NULL;
    }
    return task_get_tcb((task_id_t)task_id);
}

static uint64_t cap_cnode_bit(uint8_t slot) {
    return UINT64_C(1) << slot;
}

static cnode_t *cap_task_cnode(tcb_t *task) {
    if (task == NULL || task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return NULL;
    }

    cnode_t *cnode = &task_cspaces[task->id];
    if (task->cspace != cnode || cnode->owner != task ||
        cnode->hdr.obj_type != CAP_OBJ_CNODE ||
        kobj_generation_is_retired(cnode->hdr.generation)) {
        return NULL;
    }
    return cnode;
}

static cap_entry_t *cap_get_local_entry(tcb_t *task, cap_id_t local_cap) {
    uint8_t cnode_index;
    uint8_t slot;
    uint32_t generation;
    cnode_t *cnode = cap_task_cnode(task);
    if (cnode == NULL ||
        !cap_local_decode(local_cap, &cnode_index, &slot, &generation) ||
        cnode_index != (uint8_t)task->id) {
        return NULL;
    }

    cnode_slot_t *cslot = &cnode->slots[slot];
    if ((cnode->occupied & cap_cnode_bit(slot)) == 0 ||
        cslot->generation != generation || cslot->cap != local_cap ||
        cslot->backing_cap == CAP_INVALID) {
        return NULL;
    }

    cap_entry_t *entry = cap_get_entry(cslot->backing_cap);
    if (entry == NULL || entry->cnode != cnode || entry->cnode_slot != slot) {
        return NULL;
    }
    return entry;
}

static cap_entry_t *cap_get_entry_for(tcb_t *owner, cap_id_t cap) {
    if (cap_is_local_cptr(cap)) {
        return cap_get_local_entry(owner, cap);
    }
    /* A user task never receives or resolves internal pool handles. */
    if (owner != NULL && (owner->attrs & TASK_ATTR_USER) != 0) {
        return NULL;
    }
    return cap_get_entry(cap);
}

static tcb_t *cap_task_from_local(cap_id_t cap) {
    uint8_t cnode_index;
    uint8_t slot;
    uint32_t generation;
    if (!cap_local_decode(cap, &cnode_index, &slot, &generation)) {
        return NULL;
    }
    (void)slot;
    (void)generation;
    return cap_task_by_id(cnode_index);
}

cnode_t *cap_space_of(tcb_t *task) {
    return cap_task_cnode(task);
}

kern_err_t cap_space_init(tcb_t *task) {
    if (task == NULL || task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cnode_t *cnode = &task_cspaces[task->id];

    if (task->cspace == cnode && cnode->owner == task) {
        CAP_UNLOCK(crit);
        return KERN_OK;
    }
    if (task->cspace != NULL || cnode->owner != NULL || cnode->occupied != 0) {
        CAP_UNLOCK(crit);
        return KERN_ERR_STATE;
    }
    if (kobj_generation_is_retired(cnode->hdr.generation)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_RESOURCE;
    }

    if (cnode->hdr.generation == 0) {
        memset(cnode, 0, sizeof(*cnode));
        kobj_header_init(&cnode->hdr, CAP_OBJ_CNODE);
        for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
            cnode->slots[i].cap = CAP_INVALID;
            cnode->slots[i].backing_cap = CAP_INVALID;
            cnode->slots[i].generation = 1;
        }
    } else {
        cnode->hdr.obj_type = CAP_OBJ_CNODE;
        cnode->hdr.flags = 0;
        cnode->hdr.refs = 0;
    }

    cnode->owner = task;
    task->cspace = cnode;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_space_destroy(tcb_t *task) {
    if (task == NULL || task->id < 0 || task->id >= KERNEL_MAX_TASKS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cnode_t *cnode = cap_task_cnode(task);
    if (cnode == NULL) {
        CAP_UNLOCK(crit);
        return KERN_ERR_NOEXIST;
    }
    if (cnode->occupied != 0) {
        CAP_UNLOCK(crit);
        return KERN_ERR_BUSY;
    }

    uint32_t next_generation = kobj_header_prepare_reuse(&cnode->hdr);
    cnode->hdr.obj_type = CAP_OBJ_CNODE;
    cnode->hdr.flags = 0;
    cnode->hdr.refs = 0;
    cnode->hdr.generation = next_generation;
    cnode->owner = NULL;
    task->cspace = NULL;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

static void cap_cnode_release_slot(cnode_t *cnode, uint8_t slot,
                                   cap_id_t expected_backing) {
    if (cnode == NULL || slot >= KERN_TASK_CAP_SLOTS) {
        return;
    }

    uint64_t bit = cap_cnode_bit(slot);
    cnode_slot_t *cslot = &cnode->slots[slot];
    if ((cnode->occupied & bit) == 0 ||
        cslot->backing_cap != expected_backing) {
        return;
    }

    cnode->occupied &= ~bit;
    cslot->cap = CAP_INVALID;
    cslot->backing_cap = CAP_INVALID;
    if (cslot->generation >= CAP_LOCAL_GENERATION_MAX) {
        cslot->generation = CAP_CNODE_GENERATION_RETIRED;
    } else {
        cslot->generation++;
    }
}

static void cap_task_remove_entry(cap_entry_t *entry, cap_id_t backing_cap) {
    if (entry == NULL || entry->cnode == NULL) {
        return;
    }

    cap_cnode_release_slot(entry->cnode, entry->cnode_slot, backing_cap);
    entry->cnode = NULL;
    entry->cnode_slot = CAP_CNODE_SLOT_NONE;
}

static int cap_cnode_find_free_slot(cnode_t *cnode) {
    if (cnode == NULL) {
        return -1;
    }
    for (uint8_t i = 0; i < KERN_TASK_CAP_SLOTS; i++) {
        const cnode_slot_t *slot = &cnode->slots[i];
        if ((cnode->occupied & cap_cnode_bit(i)) == 0 &&
            slot->generation > 0 &&
            slot->generation <= CAP_LOCAL_GENERATION_MAX) {
            return (int)i;
        }
    }
    return -1;
}

static cap_id_t cap_cnode_bind_slot(cnode_t *cnode, uint8_t slot,
                                    cap_entry_t *entry,
                                    cap_id_t backing_cap) {
    cap_id_t local = cap_local_encode((uint8_t)cnode->owner->id, slot,
                                      cnode->slots[slot].generation);
    if (local == CAP_INVALID) {
        return CAP_INVALID;
    }
    cnode->slots[slot].cap = local;
    cnode->slots[slot].backing_cap = backing_cap;
    cnode->occupied |= cap_cnode_bit(slot);
    entry->cnode = cnode;
    entry->cnode_slot = slot;
    return local;
}

static kern_err_t cap_cnode_add(cnode_t *cnode, cap_entry_t *entry,
                                cap_id_t backing_cap, cap_id_t *out_local) {
    if (cnode == NULL || entry == NULL || cnode->owner == NULL ||
        cap_task_cnode(cnode->owner) != cnode) {
        return KERN_ERR_STATE;
    }
    if (entry->cnode != NULL) {
        return KERN_ERR_STATE;
    }
    int slot = cap_cnode_find_free_slot(cnode);
    if (slot < 0) {
        return KERN_ERR_RESOURCE;
    }
    cap_id_t local = cap_cnode_bind_slot(cnode, (uint8_t)slot, entry,
                                         backing_cap);
    if (local == CAP_INVALID) {
        return KERN_ERR_RESOURCE;
    }
    if (out_local != NULL) {
        *out_local = local;
    }
    return KERN_OK;
}

static int cap_task_has(tcb_t *task, cap_id_t cap) {
    return cap_get_local_entry(task, cap) != NULL;
}

static kern_err_t cap_task_add(tcb_t *task, cap_id_t backing_cap,
                               cap_id_t *out_visible) {
    if (task == NULL || task->id < 0) {
        if (out_visible != NULL) {
            *out_visible = backing_cap;
        }
        return KERN_OK;
    }
    /* 特权任务不强制登记 cspace (内核内部 cap 操作靠 cap_owner_allowed
     * 的特权放行)。user 任务严格登记。Phase G shell 改 user 后再收紧。 */
    if ((task->attrs & TASK_ATTR_USER) == 0) {
        if (out_visible != NULL) {
            *out_visible = backing_cap;
        }
        return KERN_OK;
    }

    cnode_t *cnode = cap_task_cnode(task);
    cap_entry_t *entry = cap_get_entry(backing_cap);
    if (cnode == NULL || entry == NULL) {
        return KERN_ERR_STATE;
    }
    if (entry->cnode != NULL) {
        if (entry->cnode == cnode &&
            entry->cnode_slot < KERN_TASK_CAP_SLOTS) {
            if (out_visible != NULL) {
                *out_visible = cnode->slots[entry->cnode_slot].cap;
            }
            return KERN_OK;
        }
        return KERN_ERR_STATE;
    }

    return cap_cnode_add(cnode, entry, backing_cap, out_visible);
}

static int cap_owner_allowed(tcb_t *task, cap_id_t cap, const cap_entry_t *entry) {
    if (entry == NULL) {
        return 0;
    }
    if (task == NULL) {
        /* 内核上下文 (无 current,如 panic/early boot 路径): 允许。 */
        return 1;
    }
    /* Phase E1: cap 系统对 user 任务严格生效。
     *
     * 特权任务 (非 user) 暂保留放行 —— 它们是内核 TCB 的一部分
     * (shell/timer_svc/bh_svc/test_runner),内核内部 cap 操作
     * (bh_sem/timer/irq 等) 依赖此放行。后续 Phase G 把 shell 等
     * 改成 user 任务后,特权任务数量减少,可进一步收紧。
     *
     * user 任务严格走 owner + has 检查 —— 这是 cap 系统的核心约束。
     */
    if ((task->attrs & TASK_ATTR_USER) == 0) {
        return 1;
    }
    return entry->owner == (uint8_t)task->id && cap_task_has(task, cap);
}

static int cap_alloc_slot(void) {
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (!cap_pool[i].in_use && !cap_pool[i].retired) {
            if (cap_pool[i].generation == 0) {
                cap_pool[i].generation = 1;
            }
            return i;
        }
    }
    return -1;
}

static int cap_init_child_slot(const cap_entry_t *parent, uint8_t rights) {
    int child_slot = cap_alloc_slot();
    if (child_slot < 0) {
        return -1;
    }

    cap_pool[child_slot].parent = CAP_NO_SLOT;
    cap_pool[child_slot].rights = rights;
    cap_pool[child_slot].owner = parent->owner;
    cap_pool[child_slot].object = parent->object;
    cap_pool[child_slot].obj_type = parent->obj_type;
    cap_pool[child_slot].obj_generation = parent->obj_generation;  /* M2-Step3a: 透传 */
    /* Ordinary derive/copy preserves badge policy.  Mint may replace it with
     * a validated attenuated value below. */
    cap_pool[child_slot].badge = parent->badge;
    cap_pool[child_slot].in_use = 1;
    cap_pool[child_slot].cnode_slot = CAP_CNODE_SLOT_NONE;
    cap_pool[child_slot].cnode = NULL;
    cap_pool[child_slot].first_child = CAP_NO_SLOT;
    cap_pool[child_slot].next_sibling = CAP_NO_SLOT;

    return child_slot;
}

static void cap_init_child_slot_at(const cap_entry_t *parent, uint8_t rights,
                                   int child_slot) {
    cap_pool[child_slot].parent = CAP_NO_SLOT;
    cap_pool[child_slot].rights = rights;
    cap_pool[child_slot].owner = parent->owner;
    cap_pool[child_slot].object = parent->object;
    cap_pool[child_slot].obj_type = parent->obj_type;
    cap_pool[child_slot].obj_generation = parent->obj_generation;
    cap_pool[child_slot].badge = parent->badge;
    cap_pool[child_slot].in_use = 1;
    cap_pool[child_slot].cnode_slot = CAP_CNODE_SLOT_NONE;
    cap_pool[child_slot].cnode = NULL;
    cap_pool[child_slot].first_child = CAP_NO_SLOT;
    cap_pool[child_slot].next_sibling = CAP_NO_SLOT;
}

static void cap_link_child(int parent_slot, int child_slot) {
    cap_pool[child_slot].parent = (int16_t)parent_slot;
    cap_pool[child_slot].next_sibling = cap_pool[parent_slot].first_child;
    cap_pool[parent_slot].first_child = (int16_t)child_slot;
}

static void cap_unlink_from_parent(int slot) {
    int16_t parent = cap_pool[slot].parent;

    if (parent < 0 || parent >= CAP_MAX_COUNT_VAL) {
        return;
    }

    int16_t *link = &cap_pool[parent].first_child;
    while (*link != CAP_NO_SLOT) {
        if (*link == slot) {
            *link = cap_pool[slot].next_sibling;
            break;
        }
        link = &cap_pool[*link].next_sibling;
    }

    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;
}

static void cap_clear_slot(int slot) {
    cap_id_t backing_cap = cap_encode((uint16_t)slot,
                                      cap_pool[slot].generation);
    cap_id_t visible_cap = backing_cap;
    if (cap_pool[slot].cnode != NULL &&
        cap_pool[slot].cnode_slot < KERN_TASK_CAP_SLOTS) {
        visible_cap = cap_pool[slot].cnode
                          ->slots[cap_pool[slot].cnode_slot].cap;
    }
    void *object = cap_pool[slot].object;
    uint8_t obj_type = cap_pool[slot].obj_type;
    uint32_t obj_generation = cap_pool[slot].obj_generation;
    int16_t child = cap_pool[slot].first_child;
    /* M2-#9: revoke hook 延迟到 CAP_UNLOCK 后执行 (避免持锁调 hook 死锁) */
    if (visible_cap != CAP_INVALID && obj_type < CAP_OBJ_TYPE_MAX) {
        cap_defer_hook(object, obj_type, 1, visible_cap);  /* kind=1=revoke */
    }

    if (backing_cap != CAP_INVALID) {
        cap_task_remove_entry(&cap_pool[slot], backing_cap);
    }

    uint32_t generation = cap_pool[slot].generation;
    uint8_t retire = generation >= CAP_GENERATION_MAX;
    if (!retire) {
        generation++;
    }

    while (child != CAP_NO_SLOT) {
        int16_t next = cap_pool[child].next_sibling;
        cap_pool[child].parent = CAP_NO_SLOT;
        cap_pool[child].next_sibling = CAP_NO_SLOT;
        child = next;
    }

    cap_unlink_from_parent(slot);
    memset(&cap_pool[slot], 0, sizeof(cap_pool[slot]));
    cap_pool[slot].generation = generation;
    cap_pool[slot].retired = retire;
    cap_pool[slot].cnode_slot = CAP_CNODE_SLOT_NONE;
    cap_pool[slot].cnode = NULL;
    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].first_child = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;

    /* Only the removal of the last cap queues cleanup.  This decision is made
     * atomically under cap_pool_lock, avoiding duplicate callbacks for a deep
     * derive tree and ensuring every revoke hook is queued before cleanup. */
    int has_ref = 0;
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (cap_pool[i].in_use && cap_pool[i].object == object &&
            cap_pool[i].obj_type == obj_type) {
            has_ref = 1;
            break;
        }
    }
    if (!has_ref && obj_type < CAP_OBJ_TYPE_MAX) {
        if (obj_generation != 0U && object != NULL) {
            kobject_header_t *header = (kobject_header_t *)object;
            if (header->generation == obj_generation && header->refs != 0U) {
                header->flags |= KOBJ_FLAG_CLEANUP_PENDING;
                return;
            }
        }
        cap_defer_hook(object, obj_type, 0, visible_cap); /* kind=0=cleanup */
    }
}

static void cap_revoke_slot_tree(int slot) {
    while (cap_pool[slot].first_child != CAP_NO_SLOT) {
        cap_revoke_slot_tree(cap_pool[slot].first_child);
    }
    cap_clear_slot(slot);
}

void cap_init(void) {
    irq_spin_init_rank(&cap_pool_lock, LOCKDEP_RANK_RESOURCE);
    spin_init(&cap_deferred_flush_lock);
    cap_deferred_head = 0;
    cap_deferred_count = 0;
    __atomic_store_n(&cap_deferred_pending, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&cap_deferred_owner_cpu, CAP_DEFERRED_OWNER_NONE,
                     __ATOMIC_RELAXED);
    memset(cap_pool, 0, sizeof(cap_pool));
    memset(task_cspaces, 0, sizeof(task_cspaces));
    memset(cap_cleanup_table, 0, sizeof(cap_cleanup_table));
    memset(cap_revoke_hook_table, 0, sizeof(cap_revoke_hook_table));
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        cap_pool[i].generation = 1;
        cap_pool[i].cnode_slot = CAP_CNODE_SLOT_NONE;
        cap_pool[i].parent = CAP_NO_SLOT;
        cap_pool[i].first_child = CAP_NO_SLOT;
        cap_pool[i].next_sibling = CAP_NO_SLOT;
    }
    for (int i = 0; i < KERNEL_MAX_TASKS; i++) {
        kobj_header_init(&task_cspaces[i].hdr, CAP_OBJ_CNODE);
        for (int j = 0; j < CAP_TASK_CSPACE_SLOTS; j++) {
            task_cspaces[i].slots[j].cap = CAP_INVALID;
            task_cspaces[i].slots[j].backing_cap = CAP_INVALID;
            task_cspaces[i].slots[j].generation = 1;
        }
    }
}

cap_id_t cap_create_for_gen_badge(tcb_t *owner, void *object,
                                  uint8_t obj_type, uint8_t rights,
                                  uint32_t obj_generation, uint32_t badge) {
    if (obj_type >= CAP_OBJ_TYPE_MAX ||
        kobj_generation_is_retired(obj_generation)) {
        return CAP_INVALID;
    }

    uint32_t crit = CAP_LOCK();
    int slot = cap_alloc_slot();
    if (slot < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    uint8_t owner_id = 0;
    if (owner != NULL && owner->id >= 0) {
        owner_id = (uint8_t)owner->id;
    }

    cap_id_t backing_cap = cap_encode((uint16_t)slot,
                                      cap_pool[slot].generation);
    if (backing_cap == CAP_INVALID) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_pool[slot].rights = rights;
    cap_pool[slot].owner = owner_id;
    cap_pool[slot].object = object;
    cap_pool[slot].obj_type = obj_type;
    cap_pool[slot].obj_generation = obj_generation;  /* M2-Step3a */
    cap_pool[slot].badge = badge;
    cap_pool[slot].in_use = 1;
    cap_pool[slot].cnode_slot = CAP_CNODE_SLOT_NONE;
    cap_pool[slot].cnode = NULL;
    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].first_child = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;

    cap_id_t visible_cap = backing_cap;
    if (cap_task_add(owner, backing_cap, &visible_cap) != KERN_OK) {
        cap_clear_slot(slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if (obj_generation != 0U && object != NULL) {
        kobject_header_t *header = (kobject_header_t *)object;
        if (header->generation == obj_generation) {
            header->flags &= ~KOBJ_FLAG_CLEANUP_PENDING;
        }
    }

    CAP_UNLOCK(crit);
    return visible_cap;
}

cap_id_t cap_create_for_gen(tcb_t *owner, void *object, uint8_t obj_type,
                            uint8_t rights, uint32_t obj_generation) {
    return cap_create_for_gen_badge(owner, object, obj_type, rights,
                                    obj_generation, 0U);
}

cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner) {
    tcb_t *current = sched_get_current();

    if (current != NULL && (current->attrs & TASK_ATTR_USER) != 0) {
        return cap_create_for_gen(current, object, obj_type, rights, 0);
    }

    cap_id_t cap = cap_create_for_gen(NULL, object, obj_type, rights, 0);
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry != NULL) {
        entry->owner = owner;
    } else {
        cap = CAP_INVALID;
    }
    CAP_UNLOCK(crit);
    return cap;
}

cap_id_t cap_cnode_cap_create(tcb_t *holder, tcb_t *target,
                              uint8_t rights) {
    cnode_t *cnode = cap_space_of(target);
    if (cnode == NULL) {
        return CAP_INVALID;
    }
    return cap_create_for_gen(holder, cnode, CAP_OBJ_CNODE, rights,
                              cnode->hdr.generation);
}

void *cap_lookup_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type, uint8_t required_rights) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    if (entry == NULL) {
        CAP_UNLOCK(crit);
        return NULL;
    }
    if (entry->obj_type != obj_type) {
        CAP_UNLOCK(crit);
        return NULL;
    }
    if ((entry->rights & required_rights) != required_rights) {
        CAP_UNLOCK(crit);
        return NULL;
    }
    if (!cap_owner_allowed(owner, cap, entry)) {
        CAP_UNLOCK(crit);
        return NULL;
    }
    void *object = entry->object;
    CAP_UNLOCK(crit);
    return object;
}

void *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights) {
    tcb_t *current = sched_get_current();
    if (cap_is_local_cptr(cap) &&
        (current == NULL || (current->attrs & TASK_ATTR_USER) == 0)) {
        current = cap_task_from_local(cap);
    }
    return cap_lookup_for(current, cap, obj_type, required_rights);
}

kern_err_t cap_get_type_for(tcb_t *owner, cap_id_t cap, uint8_t *out_type) {
    if (out_type == NULL) {
        return KERN_ERR_PARAM;
    }
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    if (entry == NULL || !cap_owner_allowed(owner, cap, entry)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    *out_type = entry->obj_type;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_get_type(cap_id_t cap, uint8_t *out_type) {
    tcb_t *current = sched_get_current();
    if (cap_is_local_cptr(cap) &&
        (current == NULL || (current->attrs & TASK_ATTR_USER) == 0)) {
        current = cap_task_from_local(cap);
    }
    return cap_get_type_for(current, cap, out_type);
}

kern_err_t cap_get_rights_for(tcb_t *owner, cap_id_t cap,
                              uint8_t *out_rights) {
    if (out_rights == NULL) {
        return KERN_ERR_PARAM;
    }
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    if (entry == NULL || !cap_owner_allowed(owner, cap, entry)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    *out_rights = entry->rights;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_get_rights(cap_id_t cap, uint8_t *out_rights) {
    tcb_t *current = sched_get_current();
    if (cap_is_local_cptr(cap) &&
        (current == NULL || (current->attrs & TASK_ATTR_USER) == 0)) {
        current = cap_task_from_local(cap);
    }
    return cap_get_rights_for(current, cap, out_rights);
}

kern_err_t cap_delete_for(tcb_t *owner, cap_id_t cap) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    if (entry == NULL || !cap_owner_allowed(owner, cap, entry)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    int slot = cap_slot_of(entry);
    cap_clear_slot(slot);
    CAP_UNLOCK(crit);
    return KERN_OK;
}

void cap_delete(cap_id_t cap) {
    if (cap_is_local_cptr(cap)) {
        tcb_t *owner = sched_get_current();
        if (owner == NULL || (owner->attrs & TASK_ATTR_USER) == 0) {
            owner = cap_task_from_local(cap);
        }
        (void)cap_delete_for(owner, cap);
        return;
    }

    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry != NULL) {
        cap_clear_slot(cap_slot_of(entry));
    }
    CAP_UNLOCK(crit);
}

void cap_revoke_all(uint8_t owner) {
    uint32_t crit = CAP_LOCK();
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (cap_pool[i].in_use && cap_pool[i].owner == owner) {
            cap_clear_slot(i);
        }
    }
    CAP_UNLOCK(crit);
}

cap_id_t cap_derive_for(tcb_t *owner, cap_id_t parent_cap, uint8_t subset_rights) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *parent = cap_get_entry_for(owner, parent_cap);
    if (parent == NULL) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if (!cap_owner_allowed(owner, parent_cap, parent)) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((parent->rights & CAP_GRANT) == 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((subset_rights & ~parent->rights) != 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    int child_slot = cap_init_child_slot(parent, subset_rights);
    if (child_slot < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_id_t child_backing = cap_encode((uint16_t)child_slot,
                                        cap_pool[child_slot].generation);
    if (child_backing == CAP_INVALID) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_link_child(cap_slot_of(parent), child_slot);

    tcb_t *install_owner = NULL;
    if (owner != NULL && cap_task_has(owner, parent_cap)) {
        install_owner = owner;
    } else {
        tcb_t *registered_owner = cap_task_by_id(parent->owner);
        if (cap_task_has(registered_owner, parent_cap)) {
            install_owner = registered_owner;
        }
    }

    if (install_owner != NULL &&
        cap_task_add(install_owner, child_backing, NULL) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_id_t child_visible = child_backing;
    if (install_owner != NULL) {
        cap_entry_t *child_entry = &cap_pool[child_slot];
        if (child_entry->cnode != NULL) {
            child_visible = child_entry->cnode->slots[child_entry->cnode_slot].cap;
        }
    }

    CAP_UNLOCK(crit);
    return child_visible;
}

cap_id_t cap_derive(cap_id_t parent_cap, uint8_t subset_rights) {
    return cap_derive_for(sched_get_current(), parent_cap, subset_rights);
}

cap_id_t cap_mint_for(tcb_t *owner, cap_id_t parent_cap,
                      uint8_t subset_rights, uint32_t badge) {
    /* M2-#7: mint = derive + badge。复用 cap_derive_for 的所有不变量
     * (rights 衰减、GRANT drop、child 共享 parent object+generation),
     * 只额外写 badge 字段。 */
    uint32_t crit = CAP_LOCK();
    cap_entry_t *parent = cap_get_entry_for(owner, parent_cap);
    if (parent == NULL) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if (!cap_owner_allowed(owner, parent_cap, parent)) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((parent->rights & CAP_GRANT) == 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((subset_rights & ~parent->rights) != 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    /* A Factory badge is an object-type authorization mask, so mint may only
     * attenuate the parent's mask.  Other object badges retain their normal
     * endpoint/client-identity semantics. */
    if (parent->obj_type == CAP_OBJ_FACTORY &&
        (badge & ~parent->badge) != 0U) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    int child_slot = cap_init_child_slot(parent, subset_rights);
    if (child_slot < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_id_t child_backing = cap_encode((uint16_t)child_slot,
                                        cap_pool[child_slot].generation);
    if (child_backing == CAP_INVALID) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    /* M2-#7: 写 badge (cap_init_child_slot 不写 badge,这里补) */
    cap_pool[child_slot].badge = badge;

    cap_link_child(cap_slot_of(parent), child_slot);

    /* 安装到 owner (或 registered owner) 的 cspace */
    tcb_t *install_owner = NULL;
    if (owner != NULL && cap_task_has(owner, parent_cap)) {
        install_owner = owner;
    } else {
        tcb_t *registered_owner = cap_task_by_id(parent->owner);
        if (cap_task_has(registered_owner, parent_cap)) {
            install_owner = registered_owner;
        }
    }

    if (install_owner != NULL &&
        cap_task_add(install_owner, child_backing, NULL) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_id_t child_visible = child_backing;
    if (install_owner != NULL) {
        cap_entry_t *child_entry = &cap_pool[child_slot];
        if (child_entry->cnode != NULL) {
            child_visible = child_entry->cnode->slots[child_entry->cnode_slot].cap;
        }
    }

    CAP_UNLOCK(crit);
    return child_visible;
}

uint32_t cap_get_badge(cap_id_t cap) {
    uint32_t crit = CAP_LOCK();
    tcb_t *owner = sched_get_current();
    if (cap_is_local_cptr(cap) &&
        (owner == NULL || (owner->attrs & TASK_ATTR_USER) == 0)) {
        owner = cap_task_from_local(cap);
    }
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    uint32_t badge = entry != NULL ? entry->badge : 0;
    CAP_UNLOCK(crit);
    return badge;
}

kern_err_t cap_object_pin_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type,
                              uint8_t required_rights, void **out_object,
                              uint32_t *out_badge) {
    if (out_object == NULL || obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }
    *out_object = NULL;
    if (out_badge != NULL) {
        *out_badge = 0U;
    }

    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry_for(owner, cap);
    if (entry == NULL || entry->obj_type != obj_type ||
        (entry->rights & required_rights) != required_rights ||
        !cap_owner_allowed(owner, cap, entry) ||
        entry->obj_generation == 0U || entry->object == NULL) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    kobject_header_t *header = (kobject_header_t *)entry->object;
    if (header->generation != entry->obj_generation ||
        header->obj_type != obj_type ||
        (header->flags & KOBJ_FLAG_DYING) != 0U) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }
    if (header->refs == UINT16_MAX) {
        CAP_UNLOCK(crit);
        return KERN_ERR_RESOURCE;
    }

    header->refs++;
    *out_object = entry->object;
    if (out_badge != NULL) {
        *out_badge = entry->badge;
    }
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_object_unpin(void *object, uint8_t obj_type) {
    if (object == NULL || obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    kobject_header_t *header = (kobject_header_t *)object;
    if (header->obj_type != obj_type || header->refs == 0U) {
        CAP_UNLOCK(crit);
        return KERN_ERR_STATE;
    }

    header->refs--;
    if (header->refs == 0U &&
        (header->flags & KOBJ_FLAG_CLEANUP_PENDING) != 0U) {
        header->flags &= ~KOBJ_FLAG_CLEANUP_PENDING;
        cap_defer_hook(object, obj_type, 0U, CAP_INVALID);
    }
    CAP_UNLOCK(crit);
    return KERN_OK;
}

#if CAP_RESTART_SUBSET
cap_id_t cap_derive_for_restart(tcb_t *supervisor,
                                cap_id_t parent_cap,
                                tcb_t *new_task,
                                uint8_t rights) {
    /* Defined here (not in cap_subset.c) because it depends on the static
     * helpers cap_get_entry / cap_owner_allowed / cap_init_child_slot /
     * cap_link_child / cap_task_add / cap_clear_slot / cap_slot_of / cap_encode.
     * Declared publicly via cap_subset.h. */
    uint32_t crit = CAP_LOCK();
    cap_entry_t *parent = cap_get_entry_for(supervisor, parent_cap);
    if (parent == NULL || new_task == NULL || new_task->id < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    /* Supervisor must authorize the derive and hold CAP_GRANT on the parent. */
    if (!cap_owner_allowed(supervisor, parent_cap, parent)) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((parent->rights & CAP_GRANT) == 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    /* §2.4 core invariant: child ⊆ parent, CAP_GRANT ALWAYS dropped. */
    uint8_t effective = (uint8_t)(rights & parent->rights & ~CAP_GRANT);

    int child_slot = cap_init_child_slot(parent, effective);
    if (child_slot < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    /* cap_init_child_slot inherits parent->owner (the supervisor). But this
     * child is being INSTALLED into new_task's cspace and owned by new_task,
     * so fix the owner field to match — mirroring cap_copy_to (line ~488).
     * Otherwise cap_get_rights_for(new_task, ...) / cap_owner_allowed would
     * reject it (owner mismatch → KERN_ERR_CAP). */
    cap_pool[child_slot].owner = (uint8_t)new_task->id;

    cap_id_t child_backing = cap_encode((uint16_t)child_slot,
                                        cap_pool[child_slot].generation);
    if (child_backing == CAP_INVALID) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_link_child(cap_slot_of(parent), child_slot);

    /* Force-install into the NEW task's cspace. cap_task_add is a no-op for
     * non-user tasks (kernel tasks have no per-task cspace), so a kernel-mode
     * restart target simply won't receive the handle — which is correct. */
    cap_id_t child_visible = child_backing;
    if (cap_task_add(new_task, child_backing, &child_visible) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    CAP_UNLOCK(crit);
    return child_visible;
}
#endif /* CAP_RESTART_SUBSET */

static cnode_t *cap_cnode_resolve_locked(tcb_t *caller, cap_id_t cnode_cap,
                                         uint8_t required_rights) {
    cap_entry_t *entry = cap_get_entry_for(caller, cnode_cap);
    if (entry == NULL || entry->obj_type != CAP_OBJ_CNODE ||
        (entry->rights & required_rights) != required_rights ||
        !cap_owner_allowed(caller, cnode_cap, entry)) {
        return NULL;
    }

    cnode_t *cnode = (cnode_t *)entry->object;
    if (cnode == NULL || cnode->owner == NULL ||
        cap_task_cnode(cnode->owner) != cnode) {
        return NULL;
    }
    return cnode;
}

typedef struct {
    cap_entry_t *source;
    cnode_t     *target_cnode;
    tcb_t       *target_task;
    int16_t      source_slot;
    int16_t      new_pool_slot;
    int16_t      target_cnode_slot;
    uint8_t      effective_rights;
    uint32_t     effective_badge;
} cap_txn_plan_op_t;

static uint32_t cap_txn_task_generation(const tcb_t *task) {
    return task != NULL ? task->hdr.generation : 0U;
}

static int cap_txn_task_matches(const tcb_t *task, uint32_t generation) {
    if (task == NULL) {
        return generation == 0U;
    }
    return task->id >= 0 && task->id < KERNEL_MAX_TASKS &&
           task->hdr.obj_type == CAP_OBJ_TASK &&
           !kobj_generation_is_retired(task->hdr.generation) &&
           task->hdr.generation == generation;
}

void cap_txn_begin(cap_transaction_t *txn) {
    if (txn == NULL) {
        return;
    }
    memset(txn, 0, sizeof(*txn));
    for (uint8_t i = 0; i < CAP_TRANSACTION_MAX_OPS; i++) {
        txn->results[i] = CAP_INVALID;
    }
    txn->state = CAP_TXN_STATE_PREPARED;
}

static kern_err_t cap_txn_stage(cap_transaction_t *txn,
                                cap_txn_op_kind_t kind,
                                tcb_t *src, cap_id_t cap,
                                tcb_t *dst, cap_id_t dst_cnode,
                                uint8_t rights, uint8_t authority,
                                uint32_t badge, uint8_t cnode_directed,
                                uint8_t preserve_badge) {
    if (txn == NULL || txn->state != CAP_TXN_STATE_PREPARED) {
        return KERN_ERR_STATE;
    }
    if (txn->count >= CAP_TRANSACTION_MAX_OPS) {
        return KERN_ERR_RESOURCE;
    }
    if (kind != CAP_TXN_OP_REVOKE && !cnode_directed &&
        (dst == NULL || dst->id < 0)) {
        return KERN_ERR_PARAM;
    }

    cap_txn_op_t *op = &txn->ops[txn->count++];
    memset(op, 0, sizeof(*op));
    op->source_task = src;
    op->target_task = dst;
    op->source_cap = cap;
    op->target_cnode = dst_cnode;
    op->source_generation = cap_txn_task_generation(src);
    op->target_generation = cap_txn_task_generation(dst);
    op->badge = badge;
    op->kind = (uint8_t)kind;
    op->rights = rights;
    op->source_authority = authority;
    op->cnode_directed = cnode_directed;
    op->preserve_badge = preserve_badge;
    return KERN_OK;
}

kern_err_t cap_txn_prepare_copy(cap_transaction_t *txn, tcb_t *src,
                                cap_id_t cap, tcb_t *dst, uint8_t rights) {
    return cap_txn_stage(txn, CAP_TXN_OP_COPY, src, cap, dst, CAP_INVALID,
                         rights, CAP_TRANSFER, 0U, 0U, 1U);
}

kern_err_t cap_txn_prepare_move(cap_transaction_t *txn, tcb_t *src,
                                cap_id_t cap, tcb_t *dst, uint8_t rights) {
    return cap_txn_stage(txn, CAP_TXN_OP_MOVE, src, cap, dst, CAP_INVALID,
                         rights, CAP_TRANSFER, 0U, 0U, 1U);
}

kern_err_t cap_txn_prepare_revoke(cap_transaction_t *txn, tcb_t *owner,
                                  cap_id_t cap) {
    return cap_txn_stage(txn, CAP_TXN_OP_REVOKE, owner, cap, NULL,
                         CAP_INVALID, 0U, 0U, 0U, 0U, 1U);
}

static kern_err_t cap_txn_prepare_cnode(cap_transaction_t *txn,
                                        cap_txn_op_kind_t kind,
                                        tcb_t *caller, cap_id_t source,
                                        cap_id_t dst_cnode, uint8_t rights,
                                        uint8_t authority, uint32_t badge,
                                        uint8_t preserve_badge) {
    return cap_txn_stage(txn, kind, caller, source, NULL, dst_cnode,
                         rights, authority, badge, 1U, preserve_badge);
}

static int cap_txn_slot_descends_from(int slot, int ancestor) {
    int current = slot;
    for (int depth = 0; depth < CAP_MAX_COUNT_VAL && current >= 0; depth++) {
        if (current == ancestor) {
            return 1;
        }
        current = cap_pool[current].parent;
    }
    return 0;
}

static int cap_txn_reserve_pool_slot(uint8_t *reserved) {
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (!reserved[i] && !cap_pool[i].in_use && !cap_pool[i].retired &&
            cap_pool[i].generation > 0 &&
            cap_pool[i].generation <= CAP_GENERATION_MAX) {
            reserved[i] = 1U;
            return i;
        }
    }
    return -1;
}

static int cap_txn_reserve_cnode_slot(
    cnode_t *cnode,
    cnode_t *reserved_cnodes[CAP_TRANSACTION_MAX_OPS],
    uint64_t reserved_slots[CAP_TRANSACTION_MAX_OPS]) {
    int reservation = -1;
    for (uint8_t i = 0; i < CAP_TRANSACTION_MAX_OPS; i++) {
        if (reserved_cnodes[i] == cnode) {
            reservation = (int)i;
            break;
        }
        if (reservation < 0 && reserved_cnodes[i] == NULL) {
            reservation = (int)i;
        }
    }
    if (reservation < 0) {
        return -1;
    }
    if (reserved_cnodes[reservation] == NULL) {
        reserved_cnodes[reservation] = cnode;
    }

    for (uint8_t slot = 0; slot < KERN_TASK_CAP_SLOTS; slot++) {
        uint64_t bit = cap_cnode_bit(slot);
        if ((cnode->occupied & bit) == 0U &&
            (reserved_slots[reservation] & bit) == 0U &&
            cnode->slots[slot].generation > 0 &&
            cnode->slots[slot].generation <= CAP_LOCAL_GENERATION_MAX) {
            reserved_slots[reservation] |= bit;
            return (int)slot;
        }
    }
    return -1;
}

static kern_err_t cap_txn_validate_locked(
    cap_transaction_t *txn,
    cap_txn_plan_op_t plan[CAP_TRANSACTION_MAX_OPS]) {
    uint8_t pool_reserved[CAP_MAX_COUNT_VAL];
    cnode_t *reserved_cnodes[CAP_TRANSACTION_MAX_OPS];
    uint64_t reserved_slots[CAP_TRANSACTION_MAX_OPS];
    memset(pool_reserved, 0, sizeof(pool_reserved));
    memset(reserved_cnodes, 0, sizeof(reserved_cnodes));
    memset(reserved_slots, 0, sizeof(reserved_slots));
    memset(plan, 0, sizeof(cap_txn_plan_op_t) * CAP_TRANSACTION_MAX_OPS);

    for (uint8_t i = 0; i < txn->count; i++) {
        const cap_txn_op_t *op = &txn->ops[i];
        cap_txn_plan_op_t *p = &plan[i];
        p->source_slot = CAP_NO_SLOT;
        p->new_pool_slot = CAP_NO_SLOT;
        p->target_cnode_slot = CAP_NO_SLOT;

        if (op->kind > CAP_TXN_OP_REVOKE ||
            !cap_txn_task_matches(op->source_task,
                                  op->source_generation)) {
            return KERN_ERR_CAP;
        }

        cap_entry_t *source =
            cap_get_entry_for(op->source_task, op->source_cap);
        if (source == NULL ||
            !cap_owner_allowed(op->source_task, op->source_cap, source)) {
            return KERN_ERR_CAP;
        }

        p->source = source;
        p->source_slot = (int16_t)cap_slot_of(source);

        for (uint8_t j = 0; j < i; j++) {
            const cap_txn_op_t *prior = &txn->ops[j];
            if (p->source_slot == plan[j].source_slot &&
                (op->kind != CAP_TXN_OP_COPY ||
                 prior->kind != CAP_TXN_OP_COPY)) {
                return KERN_ERR_STATE;
            }
            if ((op->kind == CAP_TXN_OP_REVOKE ||
                 prior->kind == CAP_TXN_OP_REVOKE) &&
                (cap_txn_slot_descends_from(p->source_slot,
                                            plan[j].source_slot) ||
                 cap_txn_slot_descends_from(plan[j].source_slot,
                                            p->source_slot))) {
                return KERN_ERR_STATE;
            }
        }

        if (op->kind == CAP_TXN_OP_REVOKE) {
            continue;
        }
        if ((source->rights & op->source_authority) == 0U) {
            return KERN_ERR_CAP;
        }

        uint8_t rights = op->rights == CAP_TXN_KEEP_RIGHTS
                             ? source->rights
                             : op->rights;
        if ((rights & ~source->rights) != 0U) {
            return KERN_ERR_CAP;
        }
        p->effective_rights = rights;
        p->effective_badge =
            op->preserve_badge != 0U ? source->badge : op->badge;
        if (source->obj_type == CAP_OBJ_FACTORY &&
            (p->effective_badge & ~source->badge) != 0U) {
            return KERN_ERR_CAP;
        }

        if (op->cnode_directed) {
            p->target_cnode =
                cap_cnode_resolve_locked(op->source_task, op->target_cnode,
                                         CAP_WRITE);
            if (p->target_cnode == NULL) {
                return KERN_ERR_CAP;
            }
            p->target_task = p->target_cnode->owner;
        } else {
            if (!cap_txn_task_matches(op->target_task,
                                      op->target_generation)) {
                return KERN_ERR_STATE;
            }
            p->target_task = op->target_task;
            if ((p->target_task->attrs & TASK_ATTR_USER) != 0U) {
                p->target_cnode = cap_task_cnode(p->target_task);
                if (p->target_cnode == NULL) {
                    return KERN_ERR_STATE;
                }
            }
        }

        if (op->kind == CAP_TXN_OP_COPY) {
            p->new_pool_slot =
                (int16_t)cap_txn_reserve_pool_slot(pool_reserved);
            if (p->new_pool_slot < 0) {
                return KERN_ERR_RESOURCE;
            }
        }

        if (p->target_cnode != NULL &&
            (op->kind == CAP_TXN_OP_COPY ||
             source->cnode != p->target_cnode)) {
            p->target_cnode_slot = (int16_t)cap_txn_reserve_cnode_slot(
                p->target_cnode, reserved_cnodes, reserved_slots);
            if (p->target_cnode_slot < 0) {
                return KERN_ERR_RESOURCE;
            }
        }
    }
    return KERN_OK;
}

static void cap_txn_commit_copy_locked(const cap_txn_plan_op_t *plan,
                                       cap_id_t *result) {
    int child_slot = plan->new_pool_slot;
    cap_init_child_slot_at(plan->source, plan->effective_rights, child_slot);
    cap_pool[child_slot].owner = (uint8_t)plan->target_task->id;
    cap_pool[child_slot].badge = plan->effective_badge;
    cap_link_child(plan->source_slot, child_slot);

    cap_id_t backing = cap_encode((uint16_t)child_slot,
                                  cap_pool[child_slot].generation);
    if (plan->target_cnode != NULL) {
        *result = cap_cnode_bind_slot(plan->target_cnode,
                                     (uint8_t)plan->target_cnode_slot,
                                     &cap_pool[child_slot], backing);
    } else {
        *result = backing;
    }
}

static void cap_txn_commit_move_locked(const cap_txn_plan_op_t *plan,
                                       cap_id_t *result) {
    cap_entry_t *source = plan->source;
    cnode_t *old_cnode = source->cnode;
    uint8_t old_slot = source->cnode_slot;
    cap_id_t backing = cap_backing_of_entry(source);
    cap_id_t old_visible = backing;
    if (old_cnode != NULL && old_slot < KERN_TASK_CAP_SLOTS) {
        old_visible = old_cnode->slots[old_slot].cap;
    }

    /* Moving to another authority invalidates the source-visible handle.
     * Queue only the per-cap revoke hook (SHM mappings/IRQ bindings), not the
     * object cleanup hook: the backing cap entry remains live throughout. */
    if (old_cnode != plan->target_cnode ||
        source->owner != (uint8_t)plan->target_task->id) {
        cap_defer_hook(source->object, source->obj_type, 1U, old_visible);
    }

    if (plan->target_cnode != NULL) {
        if (old_cnode != plan->target_cnode) {
            *result = cap_cnode_bind_slot(
                plan->target_cnode, (uint8_t)plan->target_cnode_slot,
                source, backing);
            cap_cnode_release_slot(old_cnode, old_slot, backing);
        } else {
            *result = plan->target_cnode->slots[old_slot].cap;
        }
    } else {
        cap_cnode_release_slot(old_cnode, old_slot, backing);
        source->cnode = NULL;
        source->cnode_slot = CAP_CNODE_SLOT_NONE;
        *result = backing;
    }
    source->owner = (uint8_t)plan->target_task->id;
    source->rights = plan->effective_rights;
}

kern_err_t cap_txn_commit(cap_transaction_t *txn) {
    if (txn == NULL || txn->state != CAP_TXN_STATE_PREPARED ||
        txn->count > CAP_TRANSACTION_MAX_OPS) {
        return KERN_ERR_STATE;
    }

    cap_txn_plan_op_t plan[CAP_TRANSACTION_MAX_OPS];
    uint32_t crit = CAP_LOCK();
    kern_err_t err = cap_txn_validate_locked(txn, plan);
    if (err != KERN_OK) {
        txn->state = CAP_TXN_STATE_ABORTED;
        CAP_UNLOCK(crit);
        return err;
    }

    /* Validation reserved every pool/CNode slot while CAP_LOCK excluded all
     * observers.  Commit therefore contains no recoverable failure point. */
    for (uint8_t i = 0; i < txn->count; i++) {
        if (txn->ops[i].kind == CAP_TXN_OP_COPY) {
            cap_txn_commit_copy_locked(&plan[i], &txn->results[i]);
        } else if (txn->ops[i].kind == CAP_TXN_OP_MOVE) {
            cap_txn_commit_move_locked(&plan[i], &txn->results[i]);
        }
    }
    for (uint8_t i = 0; i < txn->count; i++) {
        if (txn->ops[i].kind == CAP_TXN_OP_REVOKE) {
            cap_revoke_slot_tree(plan[i].source_slot);
        }
    }
    txn->state = CAP_TXN_STATE_COMMITTED;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_txn_rollback(cap_transaction_t *txn) {
    if (txn == NULL) {
        return KERN_ERR_PARAM;
    }
    if (txn->state == CAP_TXN_STATE_COMMITTED) {
        return KERN_ERR_STATE;
    }
    memset(txn->ops, 0, sizeof(txn->ops));
    for (uint8_t i = 0; i < CAP_TRANSACTION_MAX_OPS; i++) {
        txn->results[i] = CAP_INVALID;
    }
    txn->count = 0;
    txn->state = CAP_TXN_STATE_ABORTED;
    return KERN_OK;
}

cap_id_t cap_cnode_copy(tcb_t *caller, cap_id_t source,
                        cap_id_t dst_cnode, uint8_t rights) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    if (cap_txn_prepare_cnode(&txn, CAP_TXN_OP_COPY, caller, source,
                              dst_cnode, rights, CAP_TRANSFER, 0U, 1U) != KERN_OK ||
        cap_txn_commit(&txn) != KERN_OK) {
        return CAP_INVALID;
    }
    return txn.results[0];
}

cap_id_t cap_cnode_mint(tcb_t *caller, cap_id_t source,
                        cap_id_t dst_cnode, uint8_t rights, uint32_t badge) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    if (cap_txn_prepare_cnode(&txn, CAP_TXN_OP_COPY, caller, source,
                              dst_cnode, rights, CAP_GRANT, badge, 0U) != KERN_OK ||
        cap_txn_commit(&txn) != KERN_OK) {
        return CAP_INVALID;
    }
    return txn.results[0];
}

kern_err_t cap_cnode_move(tcb_t *caller, cap_id_t source,
                          cap_id_t dst_cnode, cap_id_t *out_dst) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    kern_err_t err = cap_txn_prepare_cnode(
        &txn, CAP_TXN_OP_MOVE, caller, source, dst_cnode,
        CAP_TXN_KEEP_RIGHTS, CAP_TRANSFER, 0U, 1U);
    if (err == KERN_OK) {
        err = cap_txn_commit(&txn);
    }
    if (err == KERN_OK && out_dst != NULL) {
        *out_dst = txn.results[0];
    }
    return err;
}

cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    if (cap_txn_prepare_copy(&txn, src, cap, dst, rights) != KERN_OK ||
        cap_txn_commit(&txn) != KERN_OK) {
        return CAP_INVALID;
    }
    return txn.results[0];
}

kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst,
                       cap_id_t *out_dst) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    kern_err_t err = cap_txn_prepare_move(&txn, src, cap, dst,
                                           CAP_TXN_KEEP_RIGHTS);
    if (err == KERN_OK) {
        err = cap_txn_commit(&txn);
    }
    if (err == KERN_OK && out_dst != NULL) {
        *out_dst = txn.results[0];
    }
    return err;
}

kern_err_t cap_transfer(cap_id_t cap, uint8_t target_task) {
    tcb_t *current = sched_get_current();
    tcb_t *target = cap_task_by_id(target_task);

    if (target == NULL) {
        return KERN_ERR_PARAM;
    }
    return cap_move_to(current, cap, target, NULL);
}

/* 通过 task cap 找到目标任务,把 cap 转移过去 (user 态编排用)。
 * task_cap 是调用者持有的目标任务的 TASK cap。 */
kern_err_t cap_transfer_to_task_cap(cap_id_t cap, cap_id_t task_cap) {
    tcb_t *current = sched_get_current();
    if (current == NULL) {
        return KERN_ERR_STATE;
    }
    /* 解析 task cap 拿到目标 TCB */
    void *obj = cap_lookup_for(current, task_cap, CAP_OBJ_TASK, CAP_MANAGE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }
    task_id_t tid = task_id_from_obj(obj);
    tcb_t *target = task_get_tcb(tid);
    if (target == NULL) {
        return KERN_ERR_PARAM;
    }
    return cap_move_to(current, cap, target, NULL);
}

kern_err_t cap_revoke_for(tcb_t *owner, cap_id_t cap) {
    cap_transaction_t txn;
    cap_txn_begin(&txn);
    kern_err_t err = cap_txn_prepare_revoke(&txn, owner, cap);
    if (err == KERN_OK) {
        err = cap_txn_commit(&txn);
    }
    return err;
}

kern_err_t cap_revoke(cap_id_t cap) {
    tcb_t *owner = sched_get_current();
    if (cap_is_local_cptr(cap) &&
        (owner == NULL || (owner->attrs & TASK_ATTR_USER) == 0)) {
        owner = cap_task_from_local(cap);
    }
    return cap_revoke_for(owner, cap);
}

kern_err_t cap_revoke_object(void *object, uint8_t obj_type) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    int revoked = 0;
    for (;;) {
        int slot = -1;
        for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
            if (cap_pool[i].in_use &&
                cap_pool[i].object == object &&
                cap_pool[i].obj_type == obj_type) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            break;
        }
        cap_revoke_slot_tree(slot);
        revoked = 1;
    }

    CAP_UNLOCK(crit);
    return revoked ? KERN_OK : KERN_ERR_NOEXIST;
}

uint16_t cap_object_refcount(void *object, uint8_t obj_type) {
    uint16_t refs = 0;
    uint32_t crit = CAP_LOCK();

    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (cap_pool[i].in_use &&
            cap_pool[i].object == object &&
            cap_pool[i].obj_type == obj_type) {
            refs++;
        }
    }

    CAP_UNLOCK(crit);
    return refs;
}

/*----------------------------------------------------------------------------
 * M2-Step2c: per-task CSpace 自查询。
 *
 * 替代历史 pair-table / packed-arg 机制。task 入口用此 API 在自己
 * CSpace 里按 (obj_type, index) 取出第 index 个匹配的 cap。
 *
 * 顺序: 按 CNode slots[i] (i=0..CAP_TASK_CSPACE_SLOTS-1) 物理顺序扫描,
 * 与 cap_create_for 的插入顺序一致 —— 测试侧按 cap_create_for 调用
 * 顺序即可推断 task 入口的 index (0, 1, 2, ...)。
 *
 * 线程安全: 持 CAP_LOCK 读 cap_pool,防止与 revoke/copy 并发。
 * CNode 是 task 私有命名空间,但 cap_pool 条目可能被其他核 revoke,
 * 故仍需持锁验证 in_use + obj_type。
 *----------------------------------------------------------------------------*/
cap_id_t cap_self_find_slot(tcb_t *owner, uint8_t obj_type, uint8_t index) {
    if (owner == NULL || owner->id < 0) {
        return KERN_INVALID_ID;
    }
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_INVALID_ID;
    }

    uint32_t crit = CAP_LOCK();
    uint8_t seen = 0;
    cap_id_t found = KERN_INVALID_ID;
    cnode_t *cnode = cap_task_cnode(owner);
    if (cnode == NULL) {
        CAP_UNLOCK(crit);
        return KERN_INVALID_ID;
    }

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        if ((cnode->occupied & cap_cnode_bit((uint8_t)i)) == 0) {
            continue;
        }
        cap_id_t cap = cnode->slots[i].cap;
        if (cap == KERN_INVALID_ID) {
            continue;
        }

        const cap_entry_t *entry = cap_get_local_entry(owner, cap);
        if (entry == NULL || entry->cnode != cnode ||
            entry->cnode_slot != (uint8_t)i ||
            entry->obj_type != obj_type) {
            continue;
        }

        if (seen == index) {
            found = cap;
            break;
        }
        seen++;
    }

    CAP_UNLOCK(crit);
    return found;
}

uint16_t cap_free_count(void) {
    uint16_t free_count = 0;
    uint32_t crit = CAP_LOCK();

    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (!cap_pool[i].in_use && !cap_pool[i].retired &&
            cap_pool[i].generation != 0 &&
            cap_pool[i].generation <= CAP_GENERATION_MAX) {
            free_count++;
        }
    }

    CAP_UNLOCK(crit);
    return free_count;
}

#if TEST_ENABLE
uint32_t cap_test_generation_limit(void) {
    return CAP_GENERATION_MAX;
}

uint32_t cap_test_local_generation_limit(void) {
    return CAP_LOCAL_GENERATION_MAX;
}

kern_err_t cap_test_force_local_generation(tcb_t *owner, cap_id_t cap,
                                           uint32_t generation,
                                           cap_id_t *out_cap,
                                           uint8_t *out_slot) {
    if (generation == 0 || generation > CAP_LOCAL_GENERATION_MAX ||
        out_cap == NULL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_local_entry(owner, cap);
    if (entry == NULL || entry->cnode == NULL ||
        entry->cnode_slot >= KERN_TASK_CAP_SLOTS) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    uint8_t slot = entry->cnode_slot;
    cnode_slot_t *cslot = &entry->cnode->slots[slot];
    cap_id_t replacement = cap_local_encode((uint8_t)owner->id, slot,
                                             generation);
    if (replacement == CAP_INVALID) {
        CAP_UNLOCK(crit);
        return KERN_ERR_PARAM;
    }
    cslot->generation = generation;
    cslot->cap = replacement;
    *out_cap = replacement;
    if (out_slot != NULL) {
        *out_slot = slot;
    }
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_test_force_generation(cap_id_t cap, uint32_t generation,
                                     cap_id_t *out_cap, uint16_t *out_slot) {
    if (generation == 0 || generation > CAP_GENERATION_MAX || out_cap == NULL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL || entry->first_child != CAP_NO_SLOT) {
        CAP_UNLOCK(crit);
        return KERN_ERR_STATE;
    }

    int slot = cap_slot_of(entry);
    cap_id_t replacement = cap_encode((uint16_t)slot, generation);
    if (replacement == CAP_INVALID) {
        CAP_UNLOCK(crit);
        return KERN_ERR_PARAM;
    }

    /* Keep the owning leaf slot coherent with the rewritten backing handle. */
    if (entry->cnode != NULL &&
        entry->cnode_slot < KERN_TASK_CAP_SLOTS &&
        entry->cnode->slots[entry->cnode_slot].backing_cap == cap) {
        entry->cnode->slots[entry->cnode_slot].backing_cap = replacement;
    }

    entry->generation = generation;
    *out_cap = replacement;
    if (out_slot != NULL) {
        *out_slot = (uint16_t)slot;
    }
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_test_handle_info(cap_id_t cap, uint16_t *out_slot,
                                uint32_t *out_generation) {
    uint32_t slot;
    uint32_t generation;
    if (out_slot == NULL || out_generation == NULL ||
        !cap_decode(cap, &slot, &generation)) {
        return KERN_ERR_PARAM;
    }
    *out_slot = (uint16_t)slot;
    *out_generation = generation;
    return KERN_OK;
}

kern_err_t cap_test_local_handle_info(cap_id_t cap, uint8_t *out_cnode,
                                      uint8_t *out_slot,
                                      uint32_t *out_generation) {
    if (out_cnode == NULL || out_slot == NULL || out_generation == NULL ||
        !cap_local_decode(cap, out_cnode, out_slot, out_generation)) {
        return KERN_ERR_PARAM;
    }
    return KERN_OK;
}

kern_err_t cap_test_reset_retired_slot(uint16_t slot) {
    if (slot >= CAP_MAX_COUNT_VAL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = &cap_pool[slot];
    if (entry->in_use || !entry->retired) {
        CAP_UNLOCK(crit);
        return KERN_ERR_STATE;
    }
    memset(entry, 0, sizeof(*entry));
    entry->generation = 1;
    entry->cnode_slot = CAP_CNODE_SLOT_NONE;
    entry->cnode = NULL;
    entry->parent = CAP_NO_SLOT;
    entry->first_child = CAP_NO_SLOT;
    entry->next_sibling = CAP_NO_SLOT;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_test_reset_retired_local_slot(tcb_t *owner, uint8_t slot) {
    if (slot >= KERN_TASK_CAP_SLOTS) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cnode_t *cnode = cap_task_cnode(owner);
    if (cnode == NULL ||
        (cnode->occupied & cap_cnode_bit(slot)) != 0 ||
        cnode->slots[slot].generation != CAP_CNODE_GENERATION_RETIRED) {
        CAP_UNLOCK(crit);
        return KERN_ERR_STATE;
    }
    cnode->slots[slot].cap = CAP_INVALID;
    cnode->slots[slot].backing_cap = CAP_INVALID;
    cnode->slots[slot].generation = 1;
    CAP_UNLOCK(crit);
    return KERN_OK;
}
#endif

kern_err_t cap_register_cleanup(uint8_t obj_type, cap_cleanup_fn_t cleanup) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cap_cleanup_table[obj_type] = cleanup;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_register_revoke_hook(uint8_t obj_type,
                                    cap_revoke_hook_fn_t hook) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = CAP_LOCK();
    cap_revoke_hook_table[obj_type] = hook;
    CAP_UNLOCK(crit);
    return KERN_OK;
}

#endif /* CAP_ENABLE */
