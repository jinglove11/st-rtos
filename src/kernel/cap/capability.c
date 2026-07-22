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
#define CAP_GENERATION_MAX (0x7FFFU >> CAP_SLOT_BITS)
#define CAP_NO_SLOT        ((int16_t)-1)

/* Phase #3: cap_pool 自旋锁 (SMP 安全)。
 * 单核下 irq_spinlock 退化为关中断 (与之前隐式行为一致)。
 * SMP 下两核同时操作 cap_pool 互斥。
 * 用法: uint32_t crit = CAP_LOCK(); ... CAP_UNLOCK(crit); */
static irq_spinlock_t cap_pool_lock;

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
 * CAP_UNLOCK 宏在释放 cap_pool_lock 后自动 flush 队列 (锁外执行 hook)。
 * pending 队列是 per-CPU (避免两核竞争同一队列),足够大覆盖单次
 * cap_clear_slot 调用 (max 1 revoke + 1 cleanup)。
 *============================================================================*/
#define CAP_DEFERRED_MAX 4
typedef struct {
    void     *object;
    uint8_t   obj_type;
    uint8_t   kind;        /* 0=cleanup, 1=revoke_hook */
    cap_id_t  cap;        /* revoke_hook 用 */
} cap_deferred_t;

/* per-CPU deferred queue。SMP_MAX_CPUS 在 kernel_types.h 已 fallback 到 1。 */
static cap_deferred_t cap_deferred_queue[SMP_MAX_CPUS][CAP_DEFERRED_MAX];
static uint8_t cap_deferred_count[SMP_MAX_CPUS];

static void cap_defer_hook(void *object, uint8_t obj_type, uint8_t kind, cap_id_t cap) {
    uint32_t cpu = hal_get_cpu_id();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    uint8_t i = cap_deferred_count[cpu];
    if (i < CAP_DEFERRED_MAX) {
        cap_deferred_queue[cpu][i].object   = object;
        cap_deferred_queue[cpu][i].obj_type = obj_type;
        cap_deferred_queue[cpu][i].kind     = kind;
        cap_deferred_queue[cpu][i].cap      = cap;
        cap_deferred_count[cpu] = i + 1;
    }
    /* 队列满:静默丢弃 (CAP_DEFERRED_MAX=4 足够单次 cap_clear_slot,正常不会满) */
}

/* 锁外 flush:执行所有 pending hook。由 CAP_UNLOCK 自动调用。 */
static void cap_flush_deferred(void) {
    uint32_t cpu = hal_get_cpu_id();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    uint8_t n = cap_deferred_count[cpu];
    cap_deferred_count[cpu] = 0;
    for (uint8_t i = 0; i < n; i++) {
        cap_deferred_t *d = &cap_deferred_queue[cpu][i];
        if (d->kind == 1) {
            /* revoke hook */
            if (d->obj_type < CAP_OBJ_TYPE_MAX &&
                cap_revoke_hook_table[d->obj_type] != NULL) {
                cap_revoke_hook_table[d->obj_type](d->cap, d->object, d->obj_type);
            }
        } else {
            /* cleanup hook (仅当 refcount==0) */
            if (d->obj_type < CAP_OBJ_TYPE_MAX &&
                cap_cleanup_table[d->obj_type] != NULL &&
                cap_object_refcount(d->object, d->obj_type) == 0) {
                cap_cleanup_table[d->obj_type](d->object, d->obj_type);
            }
        }
    }
}

#define CAP_LOCK()   irq_spin_lock(&cap_pool_lock)
/* M2-#9: CAP_UNLOCK 释放锁后自动 flush 延迟 hook (锁外执行) */
#define CAP_UNLOCK(crit) do { irq_spin_unlock(&cap_pool_lock, crit); cap_flush_deferred(); } while (0)

typedef char cap_slot_bits_fit[(CAP_MAX_COUNT_VAL <= (1U << CAP_SLOT_BITS)) ? 1 : -1];
typedef char cap_shm_type_registered[(CAP_OBJ_SHM < CAP_OBJ_TYPE_MAX) ? 1 : -1];

static cap_entry_t cap_pool[CAP_MAX_COUNT_VAL];
/* cap_cleanup_table / cap_revoke_hook_table 已在上方 M2-#9 deferred 队列前声明 */

#define CAP_TASK_CSPACE_SLOTS \
    ((int)(sizeof(((tcb_t *)0)->cap_set) / sizeof(((tcb_t *)0)->cap_set[0])))

typedef char cap_task_cspace_size_matches_config[
    (CAP_TASK_CSPACE_SLOTS == KERN_TASK_CAP_SLOTS) ? 1 : -1];

static cap_id_t cap_encode(uint16_t slot, uint16_t generation) {
    if (slot >= CAP_MAX_COUNT_VAL || generation == 0 ||
        generation > CAP_GENERATION_MAX) {
        return CAP_INVALID;
    }
    return (cap_id_t)((generation << CAP_SLOT_BITS) | slot);
}

static int cap_decode(cap_id_t cap, uint32_t *slot, uint32_t *generation) {
    uint32_t raw;

    if (cap == CAP_INVALID || cap < 0) {
        return 0;
    }

    raw = (uint32_t)cap;
    *slot = raw & CAP_SLOT_MASK;
    *generation = raw >> CAP_SLOT_BITS;
    if (*slot >= CAP_MAX_COUNT_VAL || *generation == 0) {
        return 0;
    }
    return 1;
}

static cap_entry_t *cap_get_entry(cap_id_t cap) {
    uint32_t slot;
    uint32_t generation;

    if (!cap_decode(cap, &slot, &generation)) {
        return NULL;
    }

    cap_entry_t *entry = &cap_pool[slot];
    if (!entry->in_use || entry->generation != generation) {
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
        if (kh->generation != entry->obj_generation) {
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

static tcb_t *cap_task_by_id(uint8_t task_id) {
    if (task_id >= KERNEL_MAX_TASKS) {
        return NULL;
    }
    return task_get_tcb((task_id_t)task_id);
}

static int cap_task_has(tcb_t *task, cap_id_t cap) {
    if (task == NULL || task->id < 0) {
        return 0;
    }

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        if ((task->capabilities & (uint64_t)BIT(i)) != 0 &&
            task->cap_set[i] == cap) {
            return 1;
        }
    }
    return 0;
}

static kern_err_t cap_task_add(tcb_t *task, cap_id_t cap) {
    if (task == NULL || task->id < 0) {
        return KERN_OK;
    }
    /* 特权任务不强制登记 cspace (内核内部 cap 操作靠 cap_owner_allowed
     * 的特权放行)。user 任务严格登记。Phase G shell 改 user 后再收紧。 */
    if ((task->attrs & TASK_ATTR_USER) == 0) {
        return KERN_OK;
    }

    if (cap_task_has(task, cap)) {
        return KERN_OK;
    }

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        uint64_t bit = (uint64_t)BIT(i);
        if ((task->capabilities & bit) == 0) {
            task->cap_set[i] = cap;
            task->capabilities |= bit;
            return KERN_OK;
        }
    }

    return KERN_ERR_RESOURCE;
}

static void cap_task_remove(tcb_t *task, cap_id_t cap) {
    if (task == NULL || task->id < 0) {
        return;
    }

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        uint64_t bit = (uint64_t)BIT(i);
        if ((task->capabilities & bit) != 0 && task->cap_set[i] == cap) {
            task->capabilities &= ~bit;
            task->cap_set[i] = 0;
            return;
        }
    }
}

static void cap_remove_from_owner(uint8_t owner_id, cap_id_t cap) {
    cap_task_remove(cap_task_by_id(owner_id), cap);
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
        if (!cap_pool[i].in_use) {
            if (cap_pool[i].generation == 0 ||
                cap_pool[i].generation > CAP_GENERATION_MAX) {
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

    cap_pool[child_slot].rights = rights;
    cap_pool[child_slot].owner = parent->owner;
    cap_pool[child_slot].object = parent->object;
    cap_pool[child_slot].obj_type = parent->obj_type;
    cap_pool[child_slot].obj_generation = parent->obj_generation;  /* M2-Step3a: 透传 */
    cap_pool[child_slot].badge = 0;  /* M2-#7: derive 不带 badge (mint 才设) */
    cap_pool[child_slot].in_use = 1;
    cap_pool[child_slot].first_child = CAP_NO_SLOT;
    cap_pool[child_slot].next_sibling = CAP_NO_SLOT;

    return child_slot;
}

static uint16_t cap_next_generation(uint16_t generation) {
    generation++;
    if (generation == 0 || generation > CAP_GENERATION_MAX) {
        generation = 1;
    }
    return generation;
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
    cap_id_t cap = cap_encode((uint16_t)slot, cap_pool[slot].generation);
    void *object = cap_pool[slot].object;
    uint8_t obj_type = cap_pool[slot].obj_type;
    int16_t child = cap_pool[slot].first_child;
    /* M2-#9: revoke hook 延迟到 CAP_UNLOCK 后执行 (避免持锁调 hook 死锁) */
    if (cap != CAP_INVALID && obj_type < CAP_OBJ_TYPE_MAX) {
        cap_defer_hook(object, obj_type, 1, cap);  /* kind=1=revoke */
    }

    if (cap != CAP_INVALID) {
        cap_remove_from_owner(cap_pool[slot].owner, cap);
    }

    uint16_t generation = cap_next_generation(cap_pool[slot].generation);

    while (child != CAP_NO_SLOT) {
        int16_t next = cap_pool[child].next_sibling;
        cap_pool[child].parent = CAP_NO_SLOT;
        cap_pool[child].next_sibling = CAP_NO_SLOT;
        child = next;
    }

    cap_unlink_from_parent(slot);
    memset(&cap_pool[slot], 0, sizeof(cap_pool[slot]));
    cap_pool[slot].generation = generation;
    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].first_child = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;

    /* M2-#9: cleanup hook 延迟到 CAP_UNLOCK 后执行 */
    if (obj_type < CAP_OBJ_TYPE_MAX) {
        cap_defer_hook(object, obj_type, 0, cap);  /* kind=0=cleanup */
    }
}

static void cap_revoke_slot_tree(int slot) {
    while (cap_pool[slot].first_child != CAP_NO_SLOT) {
        cap_revoke_slot_tree(cap_pool[slot].first_child);
    }
    cap_clear_slot(slot);
}

void cap_init(void) {
    irq_spin_init(&cap_pool_lock);
    memset(cap_pool, 0, sizeof(cap_pool));
    memset(cap_cleanup_table, 0, sizeof(cap_cleanup_table));
    memset(cap_revoke_hook_table, 0, sizeof(cap_revoke_hook_table));
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        cap_pool[i].generation = 1;
        cap_pool[i].parent = CAP_NO_SLOT;
        cap_pool[i].first_child = CAP_NO_SLOT;
        cap_pool[i].next_sibling = CAP_NO_SLOT;
    }
}

cap_id_t cap_create_for_gen(tcb_t *owner, void *object, uint8_t obj_type,
                            uint8_t rights, uint16_t obj_generation) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
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

    cap_id_t cap = cap_encode((uint16_t)slot, cap_pool[slot].generation);
    if (cap == CAP_INVALID) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_pool[slot].rights = rights;
    cap_pool[slot].owner = owner_id;
    cap_pool[slot].object = object;
    cap_pool[slot].obj_type = obj_type;
    cap_pool[slot].obj_generation = obj_generation;  /* M2-Step3a */
    cap_pool[slot].badge = 0;  /* M2-#7: 新建 cap 无 badge */
    cap_pool[slot].in_use = 1;
    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].first_child = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;

    if (cap_task_add(owner, cap) != KERN_OK) {
        cap_clear_slot(slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    CAP_UNLOCK(crit);
    return cap;
}

cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner) {
    tcb_t *current = sched_get_current();

    if (current != NULL && (current->attrs & TASK_ATTR_USER) != 0) {
        return cap_create_for_gen(current, object, obj_type, rights, 0);
    }

    cap_id_t cap = cap_create_for_gen(NULL, object, obj_type, rights, 0);
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry != NULL) {
        entry->owner = owner;
    }
    return cap;
}

void *cap_lookup_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type, uint8_t required_rights) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        return NULL;
    }
    if (entry->obj_type != obj_type) {
        return NULL;
    }
    if ((entry->rights & required_rights) != required_rights) {
        return NULL;
    }
    if (!cap_owner_allowed(owner, cap, entry)) {
        return NULL;
    }
    return entry->object;
}

void *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights) {
    return cap_lookup_for(sched_get_current(), cap, obj_type, required_rights);
}

kern_err_t cap_get_type_for(tcb_t *owner, cap_id_t cap, uint8_t *out_type) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (out_type == NULL) {
        return KERN_ERR_PARAM;
    }
    if (entry == NULL || !cap_owner_allowed(owner, cap, entry)) {
        return KERN_ERR_CAP;
    }

    *out_type = entry->obj_type;
    return KERN_OK;
}

kern_err_t cap_get_type(cap_id_t cap, uint8_t *out_type) {
    return cap_get_type_for(sched_get_current(), cap, out_type);
}

kern_err_t cap_get_rights_for(tcb_t *owner, cap_id_t cap,
                              uint8_t *out_rights) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (out_rights == NULL) {
        return KERN_ERR_PARAM;
    }
    if (entry == NULL || !cap_owner_allowed(owner, cap, entry)) {
        return KERN_ERR_CAP;
    }

    *out_rights = entry->rights;
    return KERN_OK;
}

kern_err_t cap_get_rights(cap_id_t cap, uint8_t *out_rights) {
    return cap_get_rights_for(sched_get_current(), cap, out_rights);
}

void cap_delete(cap_id_t cap) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        CAP_UNLOCK(crit);
        return;
    }

    int slot = cap_slot_of(entry);
    cap_clear_slot(slot);
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
    cap_entry_t *parent = cap_get_entry(parent_cap);
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

    cap_id_t child_cap = cap_encode((uint16_t)child_slot,
                                    cap_pool[child_slot].generation);
    if (child_cap == CAP_INVALID) {
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
        cap_task_add(install_owner, child_cap) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    CAP_UNLOCK(crit);
    return child_cap;
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
    cap_entry_t *parent = cap_get_entry(parent_cap);
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

    cap_id_t child_cap = cap_encode((uint16_t)child_slot,
                                    cap_pool[child_slot].generation);
    if (child_cap == CAP_INVALID) {
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
        cap_task_add(install_owner, child_cap) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    CAP_UNLOCK(crit);
    return child_cap;
}

uint32_t cap_get_badge(cap_id_t cap) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    uint32_t badge = entry != NULL ? entry->badge : 0;
    CAP_UNLOCK(crit);
    return badge;
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
    cap_entry_t *parent = cap_get_entry(parent_cap);
    if (parent == NULL || new_task == NULL || new_task->id < 0) {
        return CAP_INVALID;
    }

    /* Supervisor must authorize the derive and hold CAP_GRANT on the parent. */
    if (!cap_owner_allowed(supervisor, parent_cap, parent)) {
        return CAP_INVALID;
    }
    if ((parent->rights & CAP_GRANT) == 0) {
        return CAP_INVALID;
    }

    /* §2.4 core invariant: child ⊆ parent, CAP_GRANT ALWAYS dropped. */
    uint8_t effective = (uint8_t)(rights & parent->rights & ~CAP_GRANT);

    int child_slot = cap_init_child_slot(parent, effective);
    if (child_slot < 0) {
        return CAP_INVALID;
    }

    /* cap_init_child_slot inherits parent->owner (the supervisor). But this
     * child is being INSTALLED into new_task's cspace and owned by new_task,
     * so fix the owner field to match — mirroring cap_copy_to (line ~488).
     * Otherwise cap_get_rights_for(new_task, ...) / cap_owner_allowed would
     * reject it (owner mismatch → KERN_ERR_CAP). */
    cap_pool[child_slot].owner = (uint8_t)new_task->id;

    cap_id_t child_cap = cap_encode((uint16_t)child_slot,
                                    cap_pool[child_slot].generation);
    if (child_cap == CAP_INVALID) {
        return CAP_INVALID;
    }

    cap_link_child(cap_slot_of(parent), child_slot);

    /* Force-install into the NEW task's cspace. cap_task_add is a no-op for
     * non-user tasks (kernel tasks have no per-task cspace), so a kernel-mode
     * restart target simply won't receive the handle — which is correct. */
    if (cap_task_add(new_task, child_cap) != KERN_OK) {
        cap_clear_slot(child_slot);
        return CAP_INVALID;
    }

    return child_cap;
}
#endif /* CAP_RESTART_SUBSET */

cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL || dst == NULL || dst->id < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if (!cap_owner_allowed(src, cap, entry)) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((entry->rights & CAP_TRANSFER) == 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }
    if ((rights & ~entry->rights) != 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    int child_slot = cap_init_child_slot(entry, rights);
    if (child_slot < 0) {
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_id_t copied = cap_encode((uint16_t)child_slot,
                                 cap_pool[child_slot].generation);
    if (copied == CAP_INVALID) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    cap_pool[child_slot].owner = (uint8_t)dst->id;
    cap_link_child(cap_slot_of(entry), child_slot);

    if (cap_task_add(dst, copied) != KERN_OK) {
        cap_clear_slot(child_slot);
        CAP_UNLOCK(crit);
        return CAP_INVALID;
    }

    CAP_UNLOCK(crit);
    return copied;
}

kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst, cap_id_t *out_dst) {
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }
    if (dst == NULL || dst->id < 0) {
        CAP_UNLOCK(crit);
        return KERN_ERR_PARAM;
    }
    if (!cap_owner_allowed(src, cap, entry)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }
    if ((entry->rights & CAP_TRANSFER) == 0) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }
    if (cap_task_add(dst, cap) != KERN_OK) {
        CAP_UNLOCK(crit);
        return KERN_ERR_RESOURCE;
    }

    cap_remove_from_owner(entry->owner, cap);
    if (src != NULL) {
        cap_task_remove(src, cap);
    }
    entry->owner = (uint8_t)dst->id;
    if (out_dst != NULL) {
        *out_dst = cap;
    }
    CAP_UNLOCK(crit);
    return KERN_OK;
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
    uint32_t crit = CAP_LOCK();
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }
    if (!cap_owner_allowed(owner, cap, entry)) {
        CAP_UNLOCK(crit);
        return KERN_ERR_CAP;
    }

    cap_revoke_slot_tree(cap_slot_of(entry));
    CAP_UNLOCK(crit);
    return KERN_OK;
}

kern_err_t cap_revoke(cap_id_t cap) {
    return cap_revoke_for(sched_get_current(), cap);
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

    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (cap_pool[i].in_use &&
            cap_pool[i].object == object &&
            cap_pool[i].obj_type == obj_type) {
            refs++;
        }
    }

    return refs;
}

/*----------------------------------------------------------------------------
 * M2-Step2c: per-task CSpace 自查询。
 *
 * 替代历史 pair-table / packed-arg 机制。task 入口用此 API 在自己
 * CSpace 里按 (obj_type, index) 取出第 index 个匹配的 cap。
 *
 * 顺序: 按 cap_set[i] (i=0..CAP_TASK_CSPACE_SLOTS-1) 物理顺序扫描,
 * 与 cap_create_for 的插入顺序一致 —— 测试侧按 cap_create_for 调用
 * 顺序即可推断 task 入口的 index (0, 1, 2, ...)。
 *
 * 线程安全: 持 CAP_LOCK 读 cap_pool,防止与 revoke/copy 并发。
 * cap_set 是 task 私有数据,但 cap_pool 条目可能被其他核 revoke,
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

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        uint64_t bit = (uint64_t)BIT(i);
        if ((owner->capabilities & bit) == 0) {
            continue;
        }
        cap_id_t cap = owner->cap_set[i];
        if (cap == 0 || cap == KERN_INVALID_ID) {
            continue;
        }

        uint32_t slot, gen;
        if (!cap_decode(cap, &slot, &gen)) {
            continue;
        }
        const cap_entry_t *entry = &cap_pool[slot];
        if (!entry->in_use ||
            entry->generation != gen ||
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

    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (!cap_pool[i].in_use &&
            cap_pool[i].generation != 0 &&
            cap_pool[i].generation <= CAP_GENERATION_MAX) {
            free_count++;
        }
    }

    return free_count;
}

kern_err_t cap_register_cleanup(uint8_t obj_type, cap_cleanup_fn_t cleanup) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    cap_cleanup_table[obj_type] = cleanup;
    return KERN_OK;
}

kern_err_t cap_register_revoke_hook(uint8_t obj_type,
                                    cap_revoke_hook_fn_t hook) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

    cap_revoke_hook_table[obj_type] = hook;
    return KERN_OK;
}

#endif /* CAP_ENABLE */
