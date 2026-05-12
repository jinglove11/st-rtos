/**
 * @file fault.c
 * @brief Fault 异常处理实现 — 寄存器诊断 + 任务终止 + panic
 */

#include "fault.h"
#include "scheduler.h"
#include "hal.h"
#include "kernel.h"
#include "task.h"
#include "trace.h"
#include "stats.h"

#if FAULT_ENABLE

/*============================================================================
 * Crash dump 全局实例 — 放置在 .crash_dump section (重启后保留)
 *============================================================================*/

__attribute__((section(".crash_dump"), used))
crash_dump_t crash_dump;

/*============================================================================
 * 读取 SCB 故障寄存器
 *============================================================================*/

#define SCB_CFSR   (*((volatile uint32_t *)0xE000ED28))
#define SCB_HFSR   (*((volatile uint32_t *)0xE000ED2C))
#define SCB_MMFAR  (*((volatile uint32_t *)0xE000ED34))
#define SCB_BFAR   (*((volatile uint32_t *)0xE000ED38))
#define SCB_AFSR   (*((volatile uint32_t *)0xE000ED3C))

/* CFSR 字节分解 */
#define CFSR_MMFSR  ((SCB_CFSR >> 0)  & 0xFF)
#define CFSR_BFSR   ((SCB_CFSR >> 8)  & 0xFF)
#define CFSR_UFSR   ((SCB_CFSR >> 16) & 0xFFFF)

static void fault_put_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];

    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[value & 0xF];
        value >>= 4;
    }
    buf[8] = '\0';
    hal_debug_puts(buf);
}

static void fault_put_dec_i32(int32_t value) {
    char buf[12];
    int i = 0;

    if (value == 0) {
        hal_debug_puts("0");
        return;
    }
    if (value < 0) {
        hal_debug_puts("-");
        value = -value;
    }
    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        char c[2] = { buf[--i], '\0' };
        hal_debug_puts(c);
    }
}

static void fault_print_dump(void) {
    hal_debug_puts("\r\nTask: ");
    fault_put_dec_i32((int32_t)crash_dump.task_id);
    hal_debug_puts("\r\nR0: 0x");
    fault_put_hex32(crash_dump.r0);
    hal_debug_puts(" R1: 0x");
    fault_put_hex32(crash_dump.r1);
    hal_debug_puts("\r\nR2: 0x");
    fault_put_hex32(crash_dump.r2);
    hal_debug_puts(" R3: 0x");
    fault_put_hex32(crash_dump.r3);
    hal_debug_puts("\r\nR12: 0x");
    fault_put_hex32(crash_dump.r12);
    hal_debug_puts("\r\nPC: 0x");
    fault_put_hex32(crash_dump.pc);
    hal_debug_puts(" LR: 0x");
    fault_put_hex32(crash_dump.lr);
    hal_debug_puts(" XPSR: 0x");
    fault_put_hex32(crash_dump.xpsr);
    hal_debug_puts("\r\nCFSR: 0x");
    fault_put_hex32(crash_dump.cfsr);
    hal_debug_puts(" HFSR: 0x");
    fault_put_hex32(crash_dump.hfsr);
    hal_debug_puts("\r\nMMFAR: 0x");
    fault_put_hex32(crash_dump.mmfar);
    hal_debug_puts(" BFAR: 0x");
    fault_put_hex32(crash_dump.bfar);
    hal_debug_puts("\r\nMSP: 0x");
    fault_put_hex32(crash_dump.msp);
    hal_debug_puts(" PSP: 0x");
    fault_put_hex32(crash_dump.psp);
    hal_debug_puts(" CONTROL: 0x");
    fault_put_hex32(crash_dump.reserved[0]);
    hal_debug_puts(" EXC_RETURN: 0x");
    fault_put_hex32(crash_dump.reserved[3]);
    hal_debug_puts("\r\n");
}

/*============================================================================
 * 异常栈帧 (硬件自动保存)
 *============================================================================*/

typedef struct {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
} exception_frame_t;

/*============================================================================
 * 触发 PendSV — 写 ICSR.PENDSVSET
 *============================================================================*/

#define SCB_ICSR_ADDR 0xE000ED04
#define PENDSVSET_BIT (1U << 28)

static inline void trigger_pendsv(void) {
    *(volatile uint32_t *)SCB_ICSR_ADDR = PENDSVSET_BIT;
    __asm volatile("dsb; isb");
}

/*============================================================================
 * task_fault_exit — fault 终止后的安全出口
 *
 * fault_handler_c 将异常帧的 PC 指向此函数。
 * 异常返回后 CPU 执行此函数：触发 PendSV 切换到下一个任务。
 * PendSV 会发现 current 状态为 TERMINATED 并回收。
 *============================================================================*/

void task_fault_exit(void) {
    /* PendSV already triggered by fault_handler_c from Handler mode.
     * Here as safety net — loop forever in case PendSV somehow fails. */
    while (1) {
        __asm volatile("wfi");
    }
}

/*============================================================================
 * fault_handler_c — 由汇编入口调用
 *
 * 参数:
 *   fault_type: FAULT_TYPE_MEMMANAGE/BUS/USAGE/HARD
 *   exc_frame:  指向异常栈帧 (MSP 上的 exception_frame_t)
 *
 * 流程:
 *   1. 读取故障寄存器 (如果 fault 提供了地址寄存器)
 *   2. 判断故障来自用户模式还是内核模式 (xPSR bit 24)
 *   3. 用户任务 fault → task_terminate → PendSV
 *   4. 内核 fault → crash_dump → kern_panic
 *============================================================================*/

void fault_handler_c(uint32_t fault_type, void *exc_frame, uint32_t exc_return) {
    exception_frame_t *frame = (exception_frame_t *)exc_frame;
    tcb_t *current = sched_get_current();

    trace_record(TRACE_FAULT, current ? (uint8_t)current->id : 0, (uint16_t)fault_type);
    stats_record_fault();

    /* 填充 crash dump */
    crash_dump.fault_type = fault_type;
    crash_dump.task_id    = current ? current->id : -1;

    if (frame) {
        crash_dump.r0   = frame->r0;
        crash_dump.r1   = frame->r1;
        crash_dump.r2   = frame->r2;
        crash_dump.r3   = frame->r3;
        crash_dump.r12  = frame->r12;
        crash_dump.lr   = frame->lr;
        crash_dump.pc   = frame->pc;
        crash_dump.xpsr = frame->xpsr;
    }
    /* R4-R11, MSP, PSP 由汇编入口保存到 crash_dump */

    /* 读取 CONTROL 寄存器 (确认 nPRIV 位) */
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));

    /* 先清 CFSR (W1C)，再读 — 避免残留位干扰诊断 */
    uint32_t cfsr = SCB_CFSR;
    SCB_CFSR = cfsr;  /* write-1-to-clear */
    crash_dump.cfsr  = cfsr;
    crash_dump.hfsr  = SCB_HFSR;
    crash_dump.mmfar = SCB_MMFAR;
    crash_dump.bfar  = SCB_BFAR;

    /* 读取 MPU 状态 (诊断用) */
    volatile uint32_t *mpu_rnr  = (volatile uint32_t *)0xE000ED98;
    volatile uint32_t *mpu_rbar = (volatile uint32_t *)0xE000ED9C;
    volatile uint32_t *mpu_ctrl = (volatile uint32_t *)0xE000ED94;

    *mpu_rnr = 0;  /* 选择 region 0 */
    crash_dump.reserved[0] = control;           /* CONTROL */
    crash_dump.reserved[1] = *mpu_ctrl;         /* MPU_CTRL */
    crash_dump.reserved[2] = *mpu_rbar;         /* region 0 RBAR */
    crash_dump.reserved[3] = exc_return;        /* EXC_RETURN */

    /*
     * 判断用户任务 fault。
     *
     * 这里不能用 xPSR[24]：那是 Thumb T-bit，正常异常帧里几乎总是 1。
     * 可靠信号是 CONTROL.nPRIV 加当前 TCB 属性：非特权线程触发的 fault
     * 才允许被当作用户任务故障隔离；内核/Handler fault 必须 panic。
     */
    int is_user_fault = 0;
    int from_thread_mode = (exc_return & (1U << 3)) != 0;
    if (from_thread_mode &&
        (control & 1U) &&
        current &&
        (current->attrs & TASK_ATTR_USER)) {
        is_user_fault = 1;
    }

    if (is_user_fault) {
        /* 用户任务 fault → 终止任务 */
        (void)task_terminate_with_result(current, KERN_ERR_FAULT);

        /* 修改异常帧的 PC，使其指向 task_fault_exit */
        frame->pc = (uint32_t)(uintptr_t)task_fault_exit | 1U;

        /* 在 Handler 模式 (特权) 下触发 PendSV。
         * task_fault_exit 以 unpriv 模式运行，无法访问 SCB，
         * 所以必须在这里触发。*/
        trigger_pendsv();
    } else {
        /* 内核 fault → panic */
        __asm volatile("cpsid i");
        hal_debug_puts("\r\n!!! FAULT !!!\r\n");
        hal_debug_puts("Type: ");
        if (fault_type == FAULT_TYPE_HARD)      hal_debug_puts("HardFault\r\n");
        if (fault_type == FAULT_TYPE_MEMMANAGE) hal_debug_puts("MemManage\r\n");
        if (fault_type == FAULT_TYPE_BUS)       hal_debug_puts("BusFault\r\n");
        if (fault_type == FAULT_TYPE_USAGE)     hal_debug_puts("UsageFault\r\n");
        fault_print_dump();
#if TRACE_ENABLE
        trace_dump_recent(16);
#endif

        kern_panic("fault in kernel mode");
    }
}

#endif /* FAULT_ENABLE */
