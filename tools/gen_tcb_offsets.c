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
const unsigned int off_state       = (unsigned int)offsetof(tcb_t, state);
const unsigned int off_attrs       = (unsigned int)offsetof(tcb_t, attrs);
const unsigned int off_sp_limit    = (unsigned int)offsetof(tcb_t, sp_limit);
const unsigned int off_stack_base  = (unsigned int)offsetof(tcb_t, stack_base);
const unsigned int off_mpu_regions = (unsigned int)offsetof(tcb_t, mpu_regions);
