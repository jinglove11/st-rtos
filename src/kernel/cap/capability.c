/**
 * @file capability.c
 * @brief Capability system — slot/generation handles + revoke tree
 */

#include "capability.h"
#include "scheduler.h"
#include "task.h"
#include "hal.h"
#include <string.h>

#if CAP_ENABLE

#define CAP_MAX_COUNT_VAL  CAP_MAX_COUNT
#define CAP_INVALID        ((cap_id_t)-1)
#define CAP_SLOT_BITS      7
#define CAP_SLOT_MASK      ((1U << CAP_SLOT_BITS) - 1U)
#define CAP_GENERATION_MAX (0x7FFFU >> CAP_SLOT_BITS)
#define CAP_NO_SLOT        ((int16_t)-1)

typedef char cap_slot_bits_fit[(CAP_MAX_COUNT_VAL <= (1U << CAP_SLOT_BITS)) ? 1 : -1];
typedef char cap_shm_type_registered[(CAP_OBJ_SHM < CAP_OBJ_TYPE_MAX) ? 1 : -1];

static cap_entry_t cap_pool[CAP_MAX_COUNT_VAL];
static cap_cleanup_fn_t cap_cleanup_table[CAP_OBJ_TYPE_MAX];
static cap_revoke_hook_fn_t cap_revoke_hook_table[CAP_OBJ_TYPE_MAX];

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

static int cap_decode(cap_id_t cap, uint16_t *slot, uint16_t *generation) {
    uint16_t raw;

    if (cap == CAP_INVALID || cap < 0) {
        return 0;
    }

    raw = (uint16_t)cap;
    *slot = raw & CAP_SLOT_MASK;
    *generation = raw >> CAP_SLOT_BITS;
    if (*slot >= CAP_MAX_COUNT_VAL || *generation == 0) {
        return 0;
    }
    return 1;
}

static cap_entry_t *cap_get_entry(cap_id_t cap) {
    uint16_t slot;
    uint16_t generation;

    if (!cap_decode(cap, &slot, &generation)) {
        return NULL;
    }

    cap_entry_t *entry = &cap_pool[slot];
    if (!entry->in_use || entry->generation != generation) {
        return NULL;
    }

    return entry;
}

static int cap_slot_of(const cap_entry_t *entry) {
    if (entry == NULL) {
        return -1;
    }
    return (int)(entry - cap_pool);
}

static int cap_current_is_privileged(void) {
    tcb_t *current = sched_get_current();
    return current == NULL || (current->attrs & TASK_ATTR_USER) == 0;
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
        if ((task->capabilities & (uint32_t)BIT(i)) != 0 &&
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
    if ((task->attrs & TASK_ATTR_USER) == 0) {
        return KERN_OK;
    }

    if (cap_task_has(task, cap)) {
        return KERN_OK;
    }

    for (int i = 0; i < CAP_TASK_CSPACE_SLOTS; i++) {
        uint32_t bit = (uint32_t)BIT(i);
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
        uint32_t bit = (uint32_t)BIT(i);
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
        return cap_current_is_privileged();
    }
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
    if (cap != CAP_INVALID &&
        obj_type < CAP_OBJ_TYPE_MAX &&
        cap_revoke_hook_table[obj_type] != NULL) {
        cap_revoke_hook_table[obj_type](cap, object, obj_type);
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

    if (obj_type < CAP_OBJ_TYPE_MAX &&
        cap_cleanup_table[obj_type] != NULL &&
        cap_object_refcount(object, obj_type) == 0) {
        cap_cleanup_table[obj_type](object, obj_type);
    }
}

static void cap_revoke_slot_tree(int slot) {
    while (cap_pool[slot].first_child != CAP_NO_SLOT) {
        cap_revoke_slot_tree(cap_pool[slot].first_child);
    }
    cap_clear_slot(slot);
}

void cap_init(void) {
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

cap_id_t cap_create_for(tcb_t *owner, void *object, uint8_t obj_type, uint8_t rights) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return CAP_INVALID;
    }

    int slot = cap_alloc_slot();
    if (slot < 0) {
        return CAP_INVALID;
    }

    uint8_t owner_id = 0;
    if (owner != NULL && owner->id >= 0) {
        owner_id = (uint8_t)owner->id;
    }

    cap_id_t cap = cap_encode((uint16_t)slot, cap_pool[slot].generation);
    if (cap == CAP_INVALID) {
        return CAP_INVALID;
    }

    cap_pool[slot].rights = rights;
    cap_pool[slot].owner = owner_id;
    cap_pool[slot].object = object;
    cap_pool[slot].obj_type = obj_type;
    cap_pool[slot].in_use = 1;
    cap_pool[slot].parent = CAP_NO_SLOT;
    cap_pool[slot].first_child = CAP_NO_SLOT;
    cap_pool[slot].next_sibling = CAP_NO_SLOT;

    if (cap_task_add(owner, cap) != KERN_OK) {
        cap_clear_slot(slot);
        return CAP_INVALID;
    }

    return cap;
}

cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner) {
    tcb_t *current = sched_get_current();

    if (current != NULL && (current->attrs & TASK_ATTR_USER) != 0) {
        return cap_create_for(current, object, obj_type, rights);
    }

    cap_id_t cap = cap_create_for(NULL, object, obj_type, rights);
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
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        return;
    }

    int slot = cap_slot_of(entry);
    cap_clear_slot(slot);
}

void cap_revoke_all(uint8_t owner) {
    for (int i = 0; i < CAP_MAX_COUNT_VAL; i++) {
        if (cap_pool[i].in_use && cap_pool[i].owner == owner) {
            cap_clear_slot(i);
        }
    }
}

cap_id_t cap_derive_for(tcb_t *owner, cap_id_t parent_cap, uint8_t subset_rights) {
    cap_entry_t *parent = cap_get_entry(parent_cap);
    if (parent == NULL) {
        return CAP_INVALID;
    }
    if (!cap_owner_allowed(owner, parent_cap, parent)) {
        return CAP_INVALID;
    }
    if ((parent->rights & CAP_GRANT) == 0) {
        return CAP_INVALID;
    }
    if ((subset_rights & ~parent->rights) != 0) {
        return CAP_INVALID;
    }

    int child_slot = cap_init_child_slot(parent, subset_rights);
    if (child_slot < 0) {
        return CAP_INVALID;
    }

    cap_id_t child_cap = cap_encode((uint16_t)child_slot,
                                    cap_pool[child_slot].generation);
    if (child_cap == CAP_INVALID) {
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
        return CAP_INVALID;
    }

    return child_cap;
}

cap_id_t cap_derive(cap_id_t parent_cap, uint8_t subset_rights) {
    return cap_derive_for(sched_get_current(), parent_cap, subset_rights);
}

cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL || dst == NULL || dst->id < 0) {
        return CAP_INVALID;
    }
    if (!cap_owner_allowed(src, cap, entry)) {
        return CAP_INVALID;
    }
    if ((entry->rights & CAP_TRANSFER) == 0) {
        return CAP_INVALID;
    }
    if ((rights & ~entry->rights) != 0) {
        return CAP_INVALID;
    }

    int child_slot = cap_init_child_slot(entry, rights);
    if (child_slot < 0) {
        return CAP_INVALID;
    }

    cap_id_t copied = cap_encode((uint16_t)child_slot,
                                 cap_pool[child_slot].generation);
    if (copied == CAP_INVALID) {
        cap_clear_slot(child_slot);
        return CAP_INVALID;
    }

    cap_pool[child_slot].owner = (uint8_t)dst->id;
    cap_link_child(cap_slot_of(entry), child_slot);

    if (cap_task_add(dst, copied) != KERN_OK) {
        cap_clear_slot(child_slot);
        return CAP_INVALID;
    }

    return copied;
}

kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst, cap_id_t *out_dst) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        return KERN_ERR_CAP;
    }
    if (dst == NULL || dst->id < 0) {
        return KERN_ERR_PARAM;
    }
    if (!cap_owner_allowed(src, cap, entry)) {
        return KERN_ERR_CAP;
    }
    if ((entry->rights & CAP_TRANSFER) == 0) {
        return KERN_ERR_CAP;
    }
    if (cap_task_add(dst, cap) != KERN_OK) {
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

kern_err_t cap_revoke_for(tcb_t *owner, cap_id_t cap) {
    cap_entry_t *entry = cap_get_entry(cap);
    if (entry == NULL) {
        return KERN_ERR_CAP;
    }
    if (!cap_owner_allowed(owner, cap, entry)) {
        return KERN_ERR_CAP;
    }

    cap_revoke_slot_tree(cap_slot_of(entry));
    return KERN_OK;
}

kern_err_t cap_revoke(cap_id_t cap) {
    return cap_revoke_for(sched_get_current(), cap);
}

kern_err_t cap_revoke_object(void *object, uint8_t obj_type) {
    if (obj_type >= CAP_OBJ_TYPE_MAX) {
        return KERN_ERR_PARAM;
    }

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
