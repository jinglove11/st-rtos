/**
 * @file cap_subset.c
 * @brief Phase 2 §2.4 — capability subset on task restart (module anchor)
 *
 * The primary primitive, cap_derive_for_restart(), lives in capability.c
 * because it depends on the static cap-pool helpers (cap_get_entry,
 * cap_owner_allowed, cap_init_child_slot, cap_link_child, cap_task_add,
 * cap_clear_slot, cap_slot_of, cap_encode). Exposing those as non-static
 * would pollute the cap namespace for no other consumer.
 *
 * This translation unit is the home for future restart-policy helpers
 * (e.g. cap_subset_template_apply, restart-rights bookkeeping) that do not
 * need the static helpers. Today it intentionally carries only the feature
 * guard so the module is compiled, linked, and visible in the source tree.
 */

#include "kernel_config.h"

#if CAP_ENABLE && CAP_RESTART_SUBSET

#include "cap_subset.h"   /* pulls in capability.h + the public prototype */

/*
 * Nothing to define here yet — cap_derive_for_restart() is implemented in
 * capability.c. Reserved for §2.4 follow-ons (restart cap templates,
 * per-service rights tables driven by the supervisor).
 */

#endif /* CAP_ENABLE && CAP_RESTART_SUBSET */
