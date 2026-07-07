/**
 * @file smp.h
 * @brief Core completion #7 — SMP core1 launch
 */

#ifndef SMP_H
#define SMP_H

#include "kernel_config.h"

#if SMP

/** Launch the second core. Call from core0 after kern_start(). */
void smp_init_core1(void);

#endif /* SMP */

#endif /* SMP_H */
