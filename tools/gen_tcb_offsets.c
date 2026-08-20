/*
 * TCB offset generator input. Cross-compiled with -S to assembly; the
 * Python wrapper (scripts/gen_tcb_offsets.py) parses the .word directives
 * below each symbol and emits src/kernel/include/tcb_offsets.inc.
 *
 * Using offsetof() against the real TCB keeps the assembly offsets in sync
 * with kernel_types.h automatically.
 */

#include <stddef.h>
#include "kernel_types.h"

const unsigned int off_sp          = (unsigned int)offsetof(tcb_t, sp);
const unsigned int off_exc_return  = (unsigned int)offsetof(tcb_t, exc_return);
const unsigned int off_fp_high     = (unsigned int)offsetof(tcb_t, fp_high);
const unsigned int off_state       = (unsigned int)offsetof(tcb_t, state);
const unsigned int off_attrs       = (unsigned int)offsetof(tcb_t, attrs);
const unsigned int off_sp_limit    = (unsigned int)offsetof(tcb_t, sp_limit);
const unsigned int off_stack_base  = (unsigned int)offsetof(tcb_t, stack_base);
/* P1-2: mpu_regions 移入 address_space 对象;asm 不直接访问区表,
 * 上下文切换经 mpu_load_task_regions(tcb) C 调用。保留 aspace 偏移
 * 备诊断器使用。 */
const unsigned int off_aspace      = (unsigned int)offsetof(tcb_t, aspace);
