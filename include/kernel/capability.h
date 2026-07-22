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
/* M2-#6: 微内核标准对象类型 (placeholder,功能留 M4 地址域 + M6 mint):
 * - CNODE:   CSpace 节点对象 (CSpace 本身作为可派生 cap,跨 task 共享)
 * - FACTORY: 对象工厂 cap (创建新内核对象的权限,如 task_create_cap)
 * - FRAME:   物理内存帧 (M4 地址域:用户私有 data domain 的 backing memory)
 * - SYSTEM:  系统操作 cap (reboot/info/debug 控制等特权操作)
 * 当前无 backing 内核对象,enum 占位让 mint (#7) + 未来 ABI 可引用。 */
#define CAP_OBJ_CNODE       15
#define CAP_OBJ_FACTORY     16
#define CAP_OBJ_FRAME       17
#define CAP_OBJ_SYSTEM      18
#define CAP_OBJ_TYPE_MAX   19

/*============================================================================
 * 能力池条目 (内部)
 *============================================================================*/

typedef struct {
    uint16_t    generation;     /* slot generation; prevents stale cap reuse */
    uint16_t    obj_generation; /* M2-Step3a: 目标对象 generation (0=不校验) */
    uint8_t     rights;         /* 权限位图 */
    uint8_t     owner;          /* 拥有者 task_id */
    void       *object;         /* 内核对象指针 */
    uint8_t     obj_type;       /* 对象类型 */
    uint8_t     in_use;         /* 槽是否使用中 */
    uint32_t    badge;          /* M2-#7: mint 设置的 badge (server 识别 client) */
    int16_t     parent;         /* parent slot, -1 if root */
    int16_t     first_child;    /* first derived child slot */
    int16_t     next_sibling;   /* next child under same parent */
} cap_entry_t;

typedef void (*cap_cleanup_fn_t)(void *object, uint8_t obj_type);
typedef void (*cap_revoke_hook_fn_t)(cap_id_t cap, void *object,
                                     uint8_t obj_type);

/*============================================================================
 * API
 *============================================================================*/

void     cap_init(void);
cap_id_t cap_create_for_gen(tcb_t *owner, void *object, uint8_t obj_type,
                            uint8_t rights, uint16_t obj_generation);
cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner);
void     cap_delete(cap_id_t cap);
void    *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
void     cap_revoke_all(uint8_t owner);
cap_id_t cap_derive(cap_id_t cap, uint8_t subset_rights);
kern_err_t cap_transfer(cap_id_t cap, uint8_t target_task);
kern_err_t cap_transfer_to_task_cap(cap_id_t cap, cap_id_t task_cap);
kern_err_t cap_revoke(cap_id_t cap);

/*============================================================================
 * M2-#7: mint — 派生 cap 同时衰减 rights + 设置 badge
 *
 * 与 cap_derive 区别: mint 允许设置 badge (uint32,server 端识别 client 用)。
 * 典型用途: nameserver 给 client 派发带 badge 的 endpoint cap,server recv
 * 时通过 badge 区分 client 身份 (无需查 task_id)。
 *
 * 不变性:
 * - child.rights ⊆ parent.rights (rights 只能衰减,不能放大)
 * - child.badge 由 mint 调用方指定 (badge=0 表示无 badge)
 * - child 共享 parent 的 object (不创建新内核对象)
 *============================================================================*/
cap_id_t cap_mint_for(tcb_t *owner, cap_id_t parent_cap,
                      uint8_t subset_rights, uint32_t badge);
uint32_t cap_get_badge(cap_id_t cap);

/* M2-Step3a: cap_create_for 是 cap_create_for_gen 的 wrapper。
 * obj_generation=0 → cap_get_entry 跳过对象 generation cross-check
 * (用于无 header 的栈/堆临时对象,如 test_capability.c 的 &test_obj)。
 * 真池对象 (sem/mutex/mqueue/event/timer/...) 应直接调 cap_create_for_gen
 * 传入对象当前 hdr.generation。 */
static inline cap_id_t cap_create_for(tcb_t *owner, void *object,
                                       uint8_t obj_type, uint8_t rights) {
    return cap_create_for_gen(owner, object, obj_type, rights, 0);
}
void    *cap_lookup_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
kern_err_t cap_get_type_for(tcb_t *owner, cap_id_t cap, uint8_t *out_type);
kern_err_t cap_get_type(cap_id_t cap, uint8_t *out_type);
kern_err_t cap_get_rights_for(tcb_t *owner, cap_id_t cap, uint8_t *out_rights);
kern_err_t cap_get_rights(cap_id_t cap, uint8_t *out_rights);
cap_id_t cap_derive_for(tcb_t *owner, cap_id_t cap, uint8_t subset_rights);

/*============================================================================
 * M2-#8: cap transfer 事务语义
 *
 * copy/move/revoke 各自在单次 CAP_LOCK 临界区内完成全操作 (capability.c)。
 * 失败路径在解锁前 cap_clear_slot 回滚 → 源/目标均无半提交状态。
 * ipc_transfer_caps (多 cap 批量) 用 staged[] + ipc_rollback_caps 实现跨
 * cap 的 all-or-nothing。
 *
 * 当前没有显式 begin/commit/rollback API —— 事务边界 = CAP_LOCK acquire/
 * release。M2 验收 #2 (CSpace 满原子失败) 由 test_cap_copy_atomic_on_full
 * 覆盖。若未来需要跨 subsystem 事务 (如 cap + 内存分配原子),再加显式
 * transaction API。
 *============================================================================*/
cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights);
kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst, cap_id_t *out_dst);
kern_err_t cap_revoke_for(tcb_t *owner, cap_id_t cap);
kern_err_t cap_revoke_object(void *object, uint8_t obj_type);
uint16_t cap_object_refcount(void *object, uint8_t obj_type);
uint16_t cap_free_count(void);
kern_err_t cap_register_cleanup(uint8_t obj_type, cap_cleanup_fn_t cleanup);
kern_err_t cap_register_revoke_hook(uint8_t obj_type,
                                    cap_revoke_hook_fn_t hook);

/*============================================================================
 * M2-Step2c: per-task CSpace 自查询
 *
 * 替代历史 pair-table/packed-arg 机制。user task 启动后通过此 API
 * 在自己 CSpace 中按对象类型查找第 n 个 cap。无全局状态、无竞态。
 * 测试框架在 task_create_user 后用 cap_create_for 把所需 cap 写入
 * task 的 cap_set (顺序即 cap_set[0], [1], ...),task 入口按
 * (obj_type, 0/1/2/...) 索引取出。
 *============================================================================*/
cap_id_t cap_self_find_slot(tcb_t *owner, uint8_t obj_type, uint8_t index);

#endif /* CAP_ENABLE */
#endif /* CAPABILITY_H */
