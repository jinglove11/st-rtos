/**
 * @file test_elf_rel_app.c
 * @brief P1-6 — freestanding ELF with static relocations (ABS32 + THM MOVW/MOVT)
 *
 * Linked with .text in flash (LMA) and .data/.bss at a RAM *link* VMA
 * (0x20080000 — scratch address; the loader re-binds it to the runtime
 * frame via relocations). Carries:
 *   - R_ARM_ABS32        (.rel.data): initialized pointer global
 *   - R_ARM_THM_MOVW_ABS_NC / R_ARM_THM_MOVT_ABS (.rel.text):
 *     explicit #:lower16:/:upper16: address materialization
 *
 * The presence of SHT_REL forces the loader's RAM-text mode (XIP flash is
 * not writable). _start returns a verdict bitmask in R0; 0 = all checks
 * passed (relocated pointer/movw address/initial data/zero bss).
 */

/* freestanding SVC 封装: SYSCALL_TASK_YIELD = 0 */
static inline int sys_task_yield(void) {
    register int r0 __asm("r0") = 0;
    __asm volatile("svc #1" : "+r"(r0) :: "memory");
    return r0;
}

volatile int data_target = 42;
/* P1-7: 同名全局隔离自检 —— 双实例并发运行,各自 +100(50 轮 +2/yield)。
 * 隔离正确: 终值恒 1100;若实例共享数据帧(回归),终值趋近 1200。 */
static volatile int iso_counter = 1000;
static volatile int bss_check;               /* .bss: loader must zero it */
static int * volatile abs_ptr = &data_target; /* 对象 volatile:禁止折叠 → R_ARM_ABS32(.rel.data) */

static int * volatile movw_result;

static volatile int *movw_ptr(void) {
    volatile int *p;
    __asm volatile("movw %0, #:lower16:data_target\n\t"
                   "movt %0, #:upper16:data_target"
                   : "=r"(p));
    return p;
}

int _start(void *arg) {
    (void)arg;
    int verdict = 0;

    if (bss_check != 0) {
        verdict |= 1;            /* bss not zeroed */
    }
    if (data_target != 42) {
        verdict |= 2;            /* .data initial values not copied */
    }
    if (abs_ptr != &data_target) {
        verdict |= 4;            /* R_ARM_ABS32 not applied */
    }
    if (*abs_ptr != 42) {
        verdict |= 8;
    }
    movw_result = movw_ptr();
    if (movw_result != &data_target) {
        verdict |= 16;           /* MOVW/MOVT not applied */
    }
    if (*movw_result != 42) {
        verdict |= 32;
    }
    /* P1-7: 隔离自检 —— 与可能并存的兄弟实例交错推进各自计数器 */
    for (int i = 0; i < 50; i++) {
        iso_counter += 2;
        (void)sys_task_yield();
    }
    if (iso_counter != 1100) {
        verdict |= 64;           /* 数据帧被共享(同名全局互见了) */
    }
    return verdict;
}
