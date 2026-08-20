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

volatile int data_target = 42;
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
    /* 可写性在此间接证明:abs_ptr 解引用写回(不做,保持只读判定;
     * RW 映射正确性由 MPU 属性断言在白盒侧覆盖)。 */
    return verdict;
}
