/**
 * @file capability.h
 * @brief 能力系统 — 基于令牌的访问控制
 *
 * 内核 backing handle 是 32-bit slot+generation；用户任务只看到其
 * 根 CNode 中的本地 CPtr。两类句柄都保持为正数，generation 耗尽后
 * slot 永久退役，不回绕到旧句柄。内核按调用任务的 CNode 校验类型、
 * 权限和所有权。
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
#define CAP_OBJ_MEMBLOCK   7  /* legacy type: new allocations use FRAME */
#define CAP_OBJ_FILE       8
#define CAP_OBJ_TASK       9
#define CAP_OBJ_ENDPOINT   10
#define CAP_OBJ_CHANNEL    11
#define CAP_OBJ_REPLY      12
#define CAP_OBJ_MMIO       13
#define CAP_OBJ_SHM        14
/* M2-#6: 微内核标准对象类型:
 * - CNODE:   CSpace 节点对象 (CSpace 本身作为可派生 cap,跨 task 共享)
 * - FACTORY: 对象工厂 cap (创建新内核对象的权限,如 task_create_cap)
 * - FRAME:   内存帧 backing object；M4 将在此之上建立完整地址域
 * - SYSTEM:  系统操作 cap (reboot/info/debug 控制等特权操作)
 * SYSTEM 仍留给 M5 的 root/management 边界。 */
#define CAP_OBJ_CNODE       15
#define CAP_OBJ_FACTORY     16
#define CAP_OBJ_FRAME       17
#define CAP_OBJ_SYSTEM      18
#define CAP_OBJ_TYPE_MAX   19

/*============================================================================
 * Per-task CSpace / CNode leaf
 *============================================================================*/

typedef struct {
    cap_id_t cap;          /* 用户可见的 CPtr (CNode+slot+generation) */
    cap_id_t backing_cap;  /* 仅内核可见的全局 pool handle */
    uint32_t generation;   /* local slot generation，不回绕 */
} cnode_slot_t;

typedef struct cnode {
    kobject_header_t hdr;  /* CAP_OBJ_CNODE，可作为真实能力对象 */
    tcb_t      *owner;
    uint64_t    occupied;
    cnode_slot_t slots[KERN_TASK_CAP_SLOTS];
} cnode_t;

/*============================================================================
 * 能力池条目 (内部)
 *============================================================================*/

typedef struct {
    uint32_t    generation;     /* slot generation; never wraps */
    uint32_t    obj_generation; /* M2-Step3a: 目标对象 generation (0=不校验) */
    uint32_t    badge;          /* M2-#7: mint 设置的 badge (server 识别 client) */
    uint8_t     rights;         /* 权限位图 */
    uint8_t     owner;          /* 拥有者 task_id */
    void       *object;         /* 内核对象指针 */
    uint8_t     obj_type;       /* 对象类型 */
    uint8_t     in_use;         /* 槽是否使用中 */
    uint8_t     retired;        /* generation 耗尽，永久禁止再分配 */
    uint8_t     cnode_slot;     /* owner CNode leaf slot，UINT8_MAX=内核 cap */
    cnode_t    *cnode;          /* NULL=特权/内核全局 cap */
    int16_t     parent;         /* parent slot, -1 if root */
    int16_t     first_child;    /* first derived child slot */
    int16_t     next_sibling;   /* next child under same parent */
} cap_entry_t;

typedef void (*cap_cleanup_fn_t)(void *object, uint8_t obj_type);
typedef void (*cap_revoke_hook_fn_t)(cap_id_t cap, void *object,
                                     uint8_t obj_type);

/* Hooks run from a lock-free deferred safe point.  Implementations must not
 * block or yield; they may acquire ordinary ranked locks and enqueue more
 * capability cleanup work. */

/*============================================================================
 * API
 *============================================================================*/

void     cap_init(void);
kern_err_t cap_space_init(tcb_t *task);
kern_err_t cap_space_destroy(tcb_t *task);
cnode_t   *cap_space_of(tcb_t *task);
int        cap_is_local_cptr(cap_id_t cap);
/* Bootstrap authority: create a first-class cap naming target's root CNode. */
cap_id_t   cap_cnode_cap_create(tcb_t *holder, tcb_t *target,
                                uint8_t rights);
/* Internal safe-point hook used by irq_spin_unlock.  It is public only to
 * avoid a spinlock/capability header dependency cycle. */
void     cap_deferred_poll(void);
cap_id_t cap_create_for_gen_badge(tcb_t *owner, void *object,
                                  uint8_t obj_type, uint8_t rights,
                                  uint32_t obj_generation, uint32_t badge);
cap_id_t cap_create_for_gen(tcb_t *owner, void *object, uint8_t obj_type,
                            uint8_t rights, uint32_t obj_generation);
cap_id_t cap_create(void *object, uint8_t obj_type, uint8_t rights, uint8_t owner);
void     cap_delete(cap_id_t cap);
kern_err_t cap_delete_for(tcb_t *owner, cap_id_t cap);
void    *cap_resolve(cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
void     cap_revoke_all(uint8_t owner);
cap_id_t cap_derive(cap_id_t cap, uint8_t subset_rights);
/* Kernel-internal compatibility API; user syscalls must name the destination
 * with cap_transfer_to_task_cap(), not with a raw task ID. */
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
kern_err_t cap_object_pin_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type,
                              uint8_t required_rights, void **out_object,
                              uint32_t *out_badge);
kern_err_t cap_object_unpin(void *object, uint8_t obj_type);

/* P0-1(B3): cap_create_for 自动从对象 header 取 generation。
 * 所有被 cap 引用的对象必须嵌入 kobject_header_t(真实内核对象已全部
 * 满足);object==NULL 仍允许(纯句柄测试用例),gen=0 且不参与
 * cross-check。显式 gen 差异场景(伪造/stale 测试)用 cap_create_for_gen。 */
static inline cap_id_t cap_create_for(tcb_t *owner, void *object,
                                       uint8_t obj_type, uint8_t rights) {
    return cap_create_for_gen(owner, object, obj_type, rights,
                              object != NULL
                                  ? ((const kobject_header_t *)object)->generation
                                  : 0U);
}
void    *cap_lookup_for(tcb_t *owner, cap_id_t cap, uint8_t obj_type, uint8_t required_rights);
kern_err_t cap_get_type_for(tcb_t *owner, cap_id_t cap, uint8_t *out_type);
kern_err_t cap_get_type(cap_id_t cap, uint8_t *out_type);
kern_err_t cap_get_rights_for(tcb_t *owner, cap_id_t cap, uint8_t *out_rights);
kern_err_t cap_get_rights(cap_id_t cap, uint8_t *out_rights);
cap_id_t cap_derive_for(tcb_t *owner, cap_id_t cap, uint8_t subset_rights);

/*============================================================================
 * M2-#8: explicit capability transaction API
 *
 * prepare_* only records intent and never changes either CSpace.  commit takes
 * CAP_LOCK once, validates/reserves the complete batch, then publishes every
 * copy/move/revoke before releasing the lock.  A failed validation has no
 * rollback writes at all, so it does not consume cap/CNode generations.
 *
 * The transaction descriptor is kernel-private state carried by the caller;
 * it is not part of the user ABI.  A committed transaction cannot be rolled
 * back because its results may already be visible after CAP_LOCK is released.
 *============================================================================*/
#define CAP_TRANSACTION_MAX_OPS 4U
#define CAP_TXN_KEEP_RIGHTS     UINT8_MAX

typedef enum {
    CAP_TXN_OP_COPY = 0,
    CAP_TXN_OP_MOVE,
    CAP_TXN_OP_REVOKE
} cap_txn_op_kind_t;

typedef enum {
    CAP_TXN_STATE_EMPTY = 0,
    CAP_TXN_STATE_PREPARED,
    CAP_TXN_STATE_COMMITTED,
    CAP_TXN_STATE_ABORTED
} cap_txn_state_t;

typedef struct {
    tcb_t       *source_task;
    tcb_t       *target_task;
    cap_id_t     source_cap;
    cap_id_t     target_cnode;
    uint32_t     source_generation;
    uint32_t     target_generation;
    uint32_t     badge;
    uint8_t      kind;
    uint8_t      rights;
    uint8_t      source_authority;
    uint8_t      cnode_directed;
    uint8_t      preserve_badge;
} cap_txn_op_t;

typedef struct {
    cap_txn_op_t ops[CAP_TRANSACTION_MAX_OPS];
    cap_id_t     results[CAP_TRANSACTION_MAX_OPS];
    uint8_t      count;
    uint8_t      state;
    uint8_t      _reserved[2];
} cap_transaction_t;

void       cap_txn_begin(cap_transaction_t *txn);
kern_err_t cap_txn_prepare_copy(cap_transaction_t *txn, tcb_t *src,
                                cap_id_t cap, tcb_t *dst, uint8_t rights);
kern_err_t cap_txn_prepare_move(cap_transaction_t *txn, tcb_t *src,
                                cap_id_t cap, tcb_t *dst, uint8_t rights);
kern_err_t cap_txn_prepare_revoke(cap_transaction_t *txn, tcb_t *owner,
                                  cap_id_t cap);
kern_err_t cap_txn_commit(cap_transaction_t *txn);
kern_err_t cap_txn_rollback(cap_transaction_t *txn);

cap_id_t cap_copy_to(tcb_t *src, cap_id_t cap, tcb_t *dst, uint8_t rights);
kern_err_t cap_move_to(tcb_t *src, cap_id_t cap, tcb_t *dst, cap_id_t *out_dst);
/* CNode-directed operations.  The caller must hold both the source cap and a
 * CAP_WRITE CNode cap.  Copy needs CAP_TRANSFER on source; mint needs
 * CAP_GRANT and may attenuate rights/set badge; move needs CAP_TRANSFER. */
cap_id_t cap_cnode_copy(tcb_t *caller, cap_id_t source,
                        cap_id_t dst_cnode, uint8_t rights);
cap_id_t cap_cnode_mint(tcb_t *caller, cap_id_t source,
                        cap_id_t dst_cnode, uint8_t rights, uint32_t badge);
kern_err_t cap_cnode_move(tcb_t *caller, cap_id_t source,
                          cap_id_t dst_cnode, cap_id_t *out_dst);
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
 * task 的叶 CNode (顺序即 slots[0], [1], ...),task 入口按
 * (obj_type, 0/1/2/...) 索引取出。
 *============================================================================*/
cap_id_t cap_self_find_slot(tcb_t *owner, uint8_t obj_type, uint8_t index);

#if TEST_ENABLE
/* Boundary-test hooks.  They are absent from production builds and exist so
 * generation exhaustion can be verified without executing 16 million
 * create/delete cycles on target hardware. */
uint32_t cap_test_generation_limit(void);
uint32_t cap_test_local_generation_limit(void);
kern_err_t cap_test_force_generation(cap_id_t cap, uint32_t generation,
                                     cap_id_t *out_cap, uint16_t *out_slot);
kern_err_t cap_test_force_local_generation(tcb_t *owner, cap_id_t cap,
                                           uint32_t generation,
                                           cap_id_t *out_cap,
                                           uint8_t *out_slot);
kern_err_t cap_test_handle_info(cap_id_t cap, uint16_t *out_slot,
                                uint32_t *out_generation);
kern_err_t cap_test_local_handle_info(cap_id_t cap, uint8_t *out_cnode,
                                      uint8_t *out_slot,
                                      uint32_t *out_generation);
kern_err_t cap_test_reset_retired_slot(uint16_t slot);
kern_err_t cap_test_reset_retired_local_slot(tcb_t *owner, uint8_t slot);
#endif

#endif /* CAP_ENABLE */
#endif /* CAPABILITY_H */
