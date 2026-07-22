/**
 * @file first_switch.S
 * @brief 触发第一次调度 — SVC #0
 *
 * 由 sched_start() 调用，触发 SVC 异常启动第一个任务。
 * 执行后不返回。
 */

    .syntax unified
    .thumb

    .section .text
    .global hal_trigger_first_switch
    .type hal_trigger_first_switch, %function
    .align 2

hal_trigger_first_switch:
#if defined(__ARM_FP) && (__ARM_FP != 0)
    /*
     * Kernel bootstrap code is compiled for Cortex-M33+FP and GCC may use
     * VFP registers even for paired integer copies.  Do not let that
     * bootstrap FPCA state create a lazy frame on MSP which is then abandoned
     * when SVC #0 starts the first task on PSP.
     */
    mrs     r0, control
    bic     r0, r0, #4            /* CONTROL.FPCA = 0 */
    msr     control, r0
    dsb
    isb
#endif
    svc     #0
    b       .

    .size hal_trigger_first_switch, . - hal_trigger_first_switch
    .end
