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
    svc     #0
    b       .

    .size hal_trigger_first_switch, . - hal_trigger_first_switch
    .end
