/**
 * @file kobject.h
 * @brief M2-Step3a: 统一内核对象 header + per-object generation
 *
 * ============================================================================
 * 设计动机 (微内核计划.md M2 任务 1+2+3)
 * ============================================================================
 *
 * 历史 cap 系统的 object 字段两种用法:
 *   - 8 个静态池对象 (sem/mutex/mqueue/event/timer/task/endpoint/channel)
 *     塞 (id+1) 当指针 → cap_resolve 拿到的是 fake 指针,无法验证对象身份
 *   - 动态/资源对象 (frame/irq/mmio/shm/reply) 用真指针
 *
 * 问题: 对象 slot 复用 (sem_id=N 被释放后重新分配) 时,旧 cap 持有者拿到
 * 的是同一个 (id+1) 值,cap_resolve 成功,获得对新 sem 实例的越权访问。
 * cap slot generation 只防 cap_id 复用,不防对象 slot 复用。
 *
 * 解决: 在每个内核对象前嵌入 kobject_header_t。cap 创建时记录当前对象
 * generation (cap_entry.obj_generation),cap_get_entry cross-check
 * 对象当前 generation 与创建时记录的值。对象 delete 时 bump generation,
 * 使所有旧 cap 失效。
 *
 * ============================================================================
 * 字段说明
 * ============================================================================
 *
 *   obj_type    CAP_OBJ_* (重复 cap_entry.obj_type,cross-check 双保险)
 *   generation  对象代,1..KOBJ_GENERATION_MAX
 *               (0 = 未初始化,UINT32_MAX = 永久退役)
 *   refs        cap 引用计数 (M2-Step9 cleanup hook 用,当前预留)
 *   flags       KOBJ_FLAG_* 状态位
 *
 * ~12 字节/对象 (含 padding),5 个简单对象 BSS 总增长 < 1KB。
 *
 * ============================================================================
 * 锁约束 (见 docs/SMP_INVARIANTS.md §2)
 * ============================================================================
 *
 * generation 写入: 持对象子系统锁 (sem_lock/mux_lock/...)。
 * generation 读取: cap_get_entry 持 cap_pool_lock (顶层 → 底层)。
 * 跨层只读访问,与 task_get_tcb 已接受的 volatile 读模式一致。
 * refs/flags 当前未使用,Step9 cleanup hook 延迟执行队列会启用。
 */

#ifndef KOBJECT_H
#define KOBJECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * kobject_header_t
 *
 * 故意不依赖 kernel_types.h (避免循环 include),只用 stdint。
 * 内核对象 (sem_t/mutex_t/.../timer_t) 在 kernel_types.h 里把它放在
 * struct 第一个字段,cap_get_entry 通过此 header 做对象 generation
 * cross-check (cap_entry.obj_generation)。
 *============================================================================*/

typedef struct {
    uint32_t     generation;   /* 0=未初始化,UINT32_MAX=退役 */
    uint32_t     flags;        /* KOBJ_FLAG_* */
    uint16_t     refs;         /* cap 引用计数 (Step9 cleanup hook 用) */
    uint8_t      obj_type;     /* CAP_OBJ_* (cross-check 与 cap_entry.obj_type) */
    uint8_t      reserved;
} kobject_header_t;

/* 对象状态标志 (flags 字段) */
#define KOBJ_FLAG_DYING           0x1U /* 对象正在 delete,拒绝新 cap_create */
#define KOBJ_FLAG_CLEANUP_PENDING 0x2U /* last cap gone; wait for refs == 0 */

/* UINT32_MAX is a sentinel, not a valid generation.  Once the final valid
 * generation is released, the containing pool slot must never be allocated
 * again.  This turns wraparound into bounded capacity loss instead of ABA. */
#define KOBJ_GENERATION_INITIAL UINT32_C(1)
#define KOBJ_GENERATION_MAX     (UINT32_MAX - UINT32_C(1))
#define KOBJ_GENERATION_RETIRED UINT32_MAX

/*============================================================================
 * Helpers
 *============================================================================*/

/* 推进 generation；不回绕。首次从 0 进入 1，最后一代再释放后进入退役哨兵。 */
static inline uint32_t kobj_next_generation(uint32_t g) {
    if (g == 0) {
        return KOBJ_GENERATION_INITIAL;
    }
    if (g >= KOBJ_GENERATION_MAX) {
        return KOBJ_GENERATION_RETIRED;
    }
    return g + 1U;
}

static inline int kobj_generation_is_retired(uint32_t generation) {
    return generation == KOBJ_GENERATION_RETIRED;
}

/* 初始化 header (alloc 路径用) */
static inline void kobj_header_init(kobject_header_t *h, uint8_t obj_type) {
    h->obj_type   = obj_type;
    h->generation = KOBJ_GENERATION_INITIAL;
    h->refs       = 0;
    h->flags      = 0;
}

/* 对象 delete 路径用:bump generation 后 memset 整个对象再恢复 generation,
 * 让下次 alloc 拿到带新 generation 的 slot。返回 next_gen 供调用方 memset
 * 后写回 header。 */
static inline uint32_t kobj_header_prepare_reuse(kobject_header_t *h) {
    return kobj_next_generation(h->generation);
}

#ifdef __cplusplus
}
#endif

#endif /* KOBJECT_H */
