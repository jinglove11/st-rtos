# P5 Phase 2 — Fault-Tolerant Infrastructure: Completion Report

**Status:** ✅ Complete · **Date:** 2026-07 · **Board:** Raspberry Pi Pico 2 W (RP2350)
**Test baseline:** 2918/2918 PASS (was 2867 pre-Phase-2)

## Goal

Land the runtime fault→restart loop that distinguishes a microkernel from a
monolith: a user task crashes, the kernel does not die, a user-mode supervisor
is notified, and it restarts the task (with a reduced capability set) under
rate-limiting — or permanently kills it after too many restarts.

This closes gap §1.4 of `MICROKERNEL_GAP_ANALYSIS.md`.

## What landed (by slice)

### §2.1 Fault endpoint
- `src/kernel/fault/fault_endpoint.[ch]`: a kernel-side SPSC ring + bottom-half
  drains `fault_event_t` records into the reserved `kern.fault` endpoint. The
  BH drain holds the crit lock across the whole loop (PRIMASK is reentrant and
  `endpoint_notify` manages its own crit), fixing a lost-update race on the
  ring tail.
- `fault_event_t` carries `task_name` (copied from the faulting TCB before
  reclamation) so the supervisor can identify the crasher by name after its
  slot is reused.
- `fault.c` notifies AFTER `task_terminate_with_result`, so the slot is freed
  by the time the supervisor acts.
- `sys_fault_subscribe` (syscall 71) mints a **CAP_FULL** capability over the
  fault endpoint for the caller (FULL, not READ, so the supervisor can also
  bind a backoff timer onto the same endpoint).

### §2.2 Supervisor monitor loop
- `src/user/supervisor/supervisor.[ch]`: a restart-recipe registry + an
  event-driven monitor loop. The runtime state (recipes, timer handle) lives
  **on the supervisor's own stack**, because USER tasks have no SRAM mapped
  (MPU region 1 is intentionally disabled) and cannot touch global variables.
- The loop recv's on the fault endpoint forever. Each event is dispatched by
  badge: a `fault_event_t` (fault_type 1..4) → `supervisor_on_fault`; a timer
  notify (badge `SUPERVISOR_TIMER_BADGE`) → `supervisor_do_restarts`.
- **Rate-limit / backoff is event-driven, not passive.** The original
  "skip the restart if within the window" design deadlocked: a skipped restart
  left the faulted task dead, which produces no further fault events, so the
  supervisor blocked forever. The fix: on a fault, mark the recipe
  `pending_restart` and arm a one-shot timer (bound to the fault ep) for the
  backoff interval; when the timer fires, `supervisor_do_restarts` recreates
  the task via `sys_task_restart`. Exponential backoff 1/2/4/8s, permanent
  kill after `SUPERVISOR_MAX_RESTARTS` (3).

### §2.3 Init process
- `src/user/init/init.c`: the first user task (under `INIT_PROCESS`). Spawns
  the supervisor as a user task via `sys_task_create`/`sys_task_start`, then
  exits. Launched from `test_runner_task` AFTER the suite passes (so it never
  disturbs tests), before the shell. The shell stays a privileged kernel task
  for now (it calls kernel APIs directly; making it a user child of init is
  deferred to Phase 3).

### §2.4 Capability subset on restart
- `src/kernel/cap/cap_subset.[ch]` + `cap_derive_for_restart(supervisor,
  parent_cap, new_task, rights)`: derives a child cap and force-installs it
  into the NEW task's cspace (`cap_derive_for` can only install into the
  holder). CAP_GRANT is always stripped: `effective = rights & parent->rights
  & ~CAP_GRANT`. The child's owner is set to `new_task` (mirroring
  `cap_copy_to`) so rights queries resolve.
- `sys_task_restart` (syscall 72): atomically recreate a faulted user task and
  install a GRANT-stripped cap, walking the caller's cspace for a
  GRANT-bearing parent TASK cap.

### Supporting syscalls / fixes
- `SYSCALL_GET_TICK` (73): USER tasks cannot read the kernel's global tick
  counter (SRAM, MPU-unmapped). This syscall is the safe way for the
  supervisor to obtain a monotonic time reference for backoff bookkeeping.
- `KERN_WAIT_FOREVER` (`UINT32_MAX`): endpoint send/recv block paths now check
  `timeout != KERN_WAIT_FOREVER` (was `> 0`, which treated "forever" as a
  finite tick count and timed out instantly).
- Smoke task `src/user/apps/crashy_app.c`: a tiny user task that dereferences
  an unmapped address, exercising the whole loop end-to-end.

## Bugs found & fixed along the way (notable)

1. **Ring drain race** (Slice A review): producer overwrite branch mutated the
   tail concurrently with the BH reader → hold crit across the whole drain.
2. **`sys_fault_subscribe` returned a raw ep id** while `sys_ep_recv` requires
   a capability under CAP_ENABLE → supervisor spun (cap_resolve failed every
   call). Now mints a cap.
3. **`KERN_WAIT_FOREVER` mishandled** as a finite timeout → immediate wake.
4. **USER task MPU**: supervisor faulted (MemManage) touching global `recipes[]`
   → moved runtime onto the supervisor's stack.
5. **`sched_get_tick_count` from USER** faulted (reads kernel SRAM) → added
   `sys_get_tick` syscall.
6. **Rate-limit deadlock**: passive "skip within window" left the task dead
   with no wake source → event-driven timer-bound backoff.

## Verification (Pico 2 W, `full` preset)

- `make flash` → `All tests PASSED! (2918/2918)`, 0 panics.
- `ps` from the shell shows `supervisor` (blocked on the fault endpoint) and
  `crashy_app` (managed by the supervisor) alive alongside the shell.
- `crash` shows the test_fault module's intentional NULL-deref, not the
  supervisor.

## Known limitations / deferred

- **shell is still a kernel task**, not a child of init (Phase 3).
- **No meta-supervisor**: if the supervisor itself faults, the system panics
  (Phase 6).
- **Recipe registration is hardcoded** in `supervisor_monitor_loop`; a
  config-driven service manifest is a later phase.
- The "3 restarts then permanent kill" transition is implemented and unit-
  tested, but the full multi-restart smoke timeline is timing-sensitive on
  hardware (backoff windows); the mechanism is verified via the task-pool
  inspection + unit tests.
