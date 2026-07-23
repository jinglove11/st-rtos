/**
 * @file cap_subset.h
 * @brief Phase 2 §2.4 — capability subset on task restart
 *
 * When the supervisor restarts a faulted task, the restarted task must NOT
 * inherit CAP_GRANT. This prevents a runaway child from minting further
 * capabilities and escalating.
 *
 * Problem this solves: cap_derive_for() installs the derived cap into the
 * cspace of whoever HOLDS the parent cap (the supervisor). But on restart we
 * need the derived cap installed into the NEW task's cspace, not the
 * supervisor's. cap_derive_for_restart() forces the install target to be the
 * freshly-created task.
 *
 * The drop-CAP_GRANT mask is applied unconditionally here (it is the whole
 * point of §2.4); the caller passes the desired rights and CAP_GRANT is
 * stripped regardless.
 *
 * Kconfig: CAP_RESTART_SUBSET (depends on SUPERVISOR + CAP_ENABLE).
 */

#ifndef CAP_SUBSET_H
#define CAP_SUBSET_H

#include "kernel_types.h"
#include "kernel_config.h"

#if CAP_ENABLE && CAP_RESTART_SUBSET

#include "capability.h"

/**
 * cap_derive_for_restart — derive a reduced-rights cap from a parent cap and
 * install it into new_task's cspace (NOT the supervisor's).
 *
 * Semantics:
 *   - supervisor must hold parent_cap with CAP_GRANT (else CAP_INVALID).
 *   - effective rights = requested_rights & parent->rights & ~CAP_GRANT
 *     (child ⊆ parent, CAP_GRANT always dropped).
 *   - the derived cap is linked under the parent in the cap tree and installed
 *     into new_task's CNode; the supervisor does NOT receive a handle.
 *   - new_task must be a user task with a free cspace slot.
 *
 * @param supervisor  the task authorizing the derive (must hold parent_cap).
 * @param parent_cap  the capability to derive from (typically a CAP_OBJ_TASK
 *                    or CAP_OBJ_ENDPOINT held by the supervisor).
 * @param new_task    the freshly-created task whose cspace receives the child.
 * @param rights      requested rights; CAP_GRANT bit is ignored (forced off).
 * @return            the derived cap id (>=0), or CAP_INVALID on denial.
 */
cap_id_t cap_derive_for_restart(tcb_t *supervisor,
                                cap_id_t parent_cap,
                                tcb_t *new_task,
                                uint8_t rights);

#endif /* CAP_ENABLE && CAP_RESTART_SUBSET */
#endif /* CAP_SUBSET_H */
