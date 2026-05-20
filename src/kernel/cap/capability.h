/**
 * @file capability.h
 * @brief 能力系统 — 基于令牌的访问控制
 *
 * 能力 ID 是一个 16-bit slot+generation 句柄。
 * 用户任务通过句柄访问内核对象，内核校验类型、权限和所有权。
 */

#ifndef CAPABILITY_H
#define CAPABILITY_H

#include "kernel_types.h"
#include "kernel_config.h"

#if CAP_ENABLE

/*============================================================================
 * 权限位图 (5 bit)
 *============================================================================*/

#define CAP_NONE     0x00
#define CAP_READ     BIT(0)   /* sem_wait, mqueue_recv, event_wait, mutex_lock */
#define CAP_WRITE    BIT(1)   /* sem_post, mqueue_send, event_set, mutex_unlock */
#define CAP_MANAGE   BIT(2)   /* delete, reset, stop */
#define CAP_TRANSFER BIT(3)   /* 可以转移给其他任务 */
#define CAP_GRANT    BIT(4)   /* 可以创建子能力 (derive) */

#define CAP_RW       (CAP_READ | CAP_WRITE)
#define CAP_FULL     (CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER | CAP_GRANT)

/*============================================================================
 * 对象类型
 *============================================================================*/

#define CAP_OBJ_SEMAPHORE  0
#define CAP_OBJ_MUTEX      1
#define CAP_OBJ_MQUEUE     2
#define CAP_OBJ_EVENT      3
#define CAP_OBJ_TIMER      4
#define CAP_OBJ_IRQ        5
#define CAP_OBJ_BH         6
#define CAP_OBJ_MEMBLOCK   7
#define CAP_OBJ_FILE       8
#define CAP_OBJ_TASK       9
#define CAP_OBJ_ENDPOINT   10
#define CAP_OBJ_CHANNEL    11
#define CAP_OBJ_REPLY      12
#define CAP_OBJ_MMIO       13
#define CAP_OBJ_SHM        14
#define CAP_OBJ_TYPE_MAX   15

/*============================================================================
 * 能力池条目 (内部)
 *============================================================================*/

typedef struct {
    uint16_t    generation; /* slot generation; prevents stale cap reuse */
    uint8_t     rights;     /* 权限位图 */
    uint8_t     owner;      /* 拥有者 task_id */
    void       *object;     /* 内核对象指针 */
    uint8_t     obj_type;   /* 对象类型 */
    uint8_t     in_use;     /* 槽是否使用中 */
    int16_t     parent;     /* parent slot, -1 if root */
    int16_t     first_child;/* first derived child slot */
    int16_t     next_sibling;/* next child under same parent */
} cap_entry_t;

typedef void (*cap_cleanup_fn_t)(void *object, uint8_t obj_type);
typedef void (*cap_revoke_hook_fn_t)(cap_id_t cap, void *object,
                                     uint8_t obj_type);

/*============================================================================
 * API
 *============================================================================*/

void     cap_init(void);
cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner);
void     cap_delete(cap_id_t cap);
void    *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
void     cap_revoke_all(uint8_t owner);
cap_id_t cap_derive(cap_id_t cap, uint8_t subset_rights);
kern_err_t cap_transfer(cap_id_t cap, uint8_t target_task);
kern_err_t cap_revoke(cap_id_t cap);

cap_id_t cap_create_for(tcb_t *owner, void *object, uint8_t obj_type, uint8_t rights);
void    *cap_lookup_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
cap_id_t cap_derive_for(tcb_t *owner, cap_id_t cap, uint8_t subset_rights);
cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights);
kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst, cap_id_t *out_dst);
kern_err_t cap_revoke_for(tcb_t *owner, cap_id_t cap);
uint16_t cap_object_refcount(void *object, uint8_t obj_type);
uint16_t cap_free_count(void);
kern_err_t cap_register_cleanup(uint8_t obj_type, cap_cleanup_fn_t cleanup);
kern_err_t cap_register_revoke_hook(uint8_t obj_type,
                                    cap_revoke_hook_fn_t hook);

#endif /* CAP_ENABLE */
#endif /* CAPABILITY_H */
