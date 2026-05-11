/**
 * @file fault.h
 * @brief Fault 异常处理 — 崩溃诊断 + 任务终止
 */

#ifndef FAULT_H
#define FAULT_H

#include "kernel_types.h"
#include "kernel_config.h"

#if FAULT_ENABLE

/*============================================================================
 * 故障类型
 *============================================================================*/

#define FAULT_TYPE_HARD      0
#define FAULT_TYPE_MEMMANAGE 1
#define FAULT_TYPE_BUS       2
#define FAULT_TYPE_USAGE     3

/*============================================================================
 * Crash Dump 结构 (≤128B, 驻留在 .crash_dump section)
 *============================================================================*/

typedef struct {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc, xpsr;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t msp, psp;
    uint32_t fault_type;
    uint32_t task_id;
    uint32_t reserved[4];
} crash_dump_t;

/* 编译时校验 crash_dump_t 大小 */
_Static_assert(sizeof(crash_dump_t) <= 128, "crash_dump_t exceeds 128 bytes");

/*============================================================================
 * API
 *============================================================================*/

void fault_handler_c(uint32_t fault_type, void *exc_frame, uint32_t exc_return);

extern crash_dump_t crash_dump;

void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void HardFault_Handler(void);

void task_terminate(tcb_t *tcb);
void task_fault_exit(void);

#endif /* FAULT_ENABLE */
#endif /* FAULT_H */
