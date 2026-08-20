/**
 * @file abi.h
 * @brief M3-Step1: 内核 syscall ABI 版本 + 错误码稳定性 + 结构对齐规则
 *
 * ============================================================================
 * 为什么需要这个文件
 * ============================================================================
 *
 * M3 的目标是把 syscall 从大量散立接口收敛为稳定、可版本化的最小 ABI。
 * 本文件定义 ABI 基线:
 *   1. ABI 版本号 — 用户应用通过 sys_abi_version() 查询,跟编译时比对
 *   2. 错误码冻结声明 — kern_err_t 枚举值不可重新编号
 *   3. 结构对齐规则 — 跨 syscall 边界的结构体规范
 *   4. abi_header_t — 未来 syscall 参数结构的前缀 (version + size)
 *
 * 本 step 只加定义 + 一个查询 syscall,不改现有 syscall 的调用方式。
 * 后续 step 会逐个给现有 syscall 参数结构加 abi_header_t 前缀。
 *
 * ============================================================================
 * ABI 版本规则
 * ============================================================================
 *
 * MAJOR: 不兼容变更 (删除 syscall、改参数语义、改返回值格式) 时 +1
 * MINOR: 向后兼容扩展 (新增 syscall、结构体追加字段) 时 +1
 *
 * 用户应用编译时记录 KERN_ABI_VERSION_MAJOR/MINOR,运行时 sys_abi_version()
 * 比对:
 *   - MAJOR 不同 → 不兼容,拒绝运行
 *   - MINOR 不同 → 兼容 (旧应用可以跑在新内核上)
 *
 * ============================================================================
 * 错误码冻结声明
 * ============================================================================
 *
 * kern_err_t 枚举 (kernel_types.h) 的值 -1..-16 已冻结:
 *   - 不可重新编号
 *   - 不可删除已有值
 *   - 新错误码只能追加到 -17, -18, ...
 *   - KERN_SYSCALL_BLOCKED (-128) 是内部哨兵,不返回给用户
 */

#ifndef KERN_ABI_H
#define KERN_ABI_H

#include "kernel_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * ABI 版本 (单一来源: kernel_config.h 的 KERN_ABI_MAJOR/MINOR)
 *============================================================================*/

/* ABI 版本单一来源(原注释称 kernel_config.h,实际从未在那里定义)。
 * 1.1 (P0-2): SYSCALL_MEM_ALLOC 增加 rights 参数(显式衰减,不再默认
 * 全权)。调用方全部在树内同步更新,无外部消费者,按兼容扩展处理。 */
#ifndef KERN_ABI_MAJOR
#define KERN_ABI_MAJOR  1
#endif

#ifndef KERN_ABI_MINOR
#define KERN_ABI_MINOR  1
#endif

/* sys_abi_version() 返回值: (MAJOR << 16) | MINOR */
#define KERN_ABI_VERSION  ((uint32_t)(((uint32_t)KERN_ABI_MAJOR << 16) | \
                                      (uint32_t)KERN_ABI_MINOR))

/*============================================================================
 * 错误码稳定性
 *============================================================================*/

/* kern_err_t 枚举值 KERN_OK(0)..KERN_ERR_NOSYS(-16) 已冻结。
 * 新错误码从 -17 开始追加,不可复用已冻结值。 */
#define KERN_ERR_FROZEN_RANGE  16  /* 0 到 -16 共 17 个值已冻结 */

/*============================================================================
 * 结构对齐规则 (Cortex-M 32 位 ABI)
 *============================================================================*/
/*
 * 跨 syscall 边界的结构体必须遵守:
 *
 * 1. 自然对齐成员 (uint16_t 2 对齐, uint32_t 4 对齐, 指针 4 对齐)
 * 2. 指针字段固定 4 字节 (32 位架构)
 * 3. 新结构体以 abi_header_t 开头 (version + size),用于向前兼容
 * 4. 用 _Static_assert 锁定 sizeof,防止意外修改
 *
 * abi_header_t 的 version 字段关联 ABI MINOR 版本:
 *   - 结构体首次引入时 version=0
 *   - 追加字段时 version+1,旧 version 的 size 仍被接受 (只读已知字段)
 *   - ABI_CHECK 宏验证 size + version 范围
 */

typedef struct {
    uint16_t version;   /* 结构版本 */
    uint16_t size;      /* sizeof(具体结构),调用方填,内核校验 */
} abi_header_t;

_Static_assert(sizeof(abi_header_t) == 4, "abi_header_t must be 4 bytes");

/* 通用校验:检查 user 传入结构的 abi_header。
 * - hdr 必须是指向 abi_header_t (或以 abi_header_t 开头的结构) 的指针
 * - size 必须精确匹配 (不允许 size 不一致,未来扩展用 version 区分)
 * - version 不超过当前 ABI MINOR */
#define ABI_CHECK(hdr, expected_size) \
    ((hdr) != NULL && \
     ((const abi_header_t *)(hdr))->size == (expected_size) && \
     ((const abi_header_t *)(hdr))->version <= KERN_ABI_MINOR)

#ifdef __cplusplus
}
#endif

#endif /* KERN_ABI_H */
