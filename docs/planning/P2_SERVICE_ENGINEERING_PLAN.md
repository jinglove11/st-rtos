# My-RTOS P2 Service Engineering Plan

Scope: STM32F767 mainline only. P2 starts after P1 microkernel core is accepted on board tests. Keep the current direct `make` workflow and test harness. P2 focuses on hardening kernel services and preparing them for later user-space service migration. It does not require moving VFS, shell, drivers, or timers out of the kernel yet.

## Goal

P2 turns the P1 object/IPC/capability foundation into a more service-ready system. The work is about bounded queues, deterministic cleanup, clearer driver/device contracts, memory-object ownership, and diagnostics that can explain failures without interactive debugging.

P2 includes:

- Timer service hardening
- IRQ/threaded IRQ/BH lifecycle hardening
- Device and driver framework tightening
- Memory management observability and memory-object capability groundwork
- Shell and diagnostics improvements
- Trace/stats expansion for random issue localization

P2 does not include:

- Full root/init and name server migration
- Moving UART/GPIO drivers to user space
- Moving VFS/devfs/ramfs to user space
- Dynamic process loading
- Multi-board bring-up

## Design Rules

1. Every service loop must block on an explicit event source or bounded timeout. No blind `while (1)` polling without a reason.
2. Every queue-full path must return a stable error, increment a stat, and leave object state unchanged.
3. Deleting an object must wake or cancel all waiters and pending callbacks deterministically.
4. ISR paths must stay bounded and must not call blocking APIs.
5. Threaded IRQ/BH callbacks run in task context and must have documented stack budgets.
6. Device operations must share a common open/read/write/ioctl model through devfs/VFS.
7. Memory ownership must be visible through stats and, where possible, capability-backed handles.
8. Trace/stats must help explain timeout, queue-full, delete, revoke, IRQ, and fault events.

## P2-1: Timer Service Hardening

Status: implemented for P2 diagnostics pass; deeper cancel/running-state work is deferred to P3.

### Files

- `src/kernel/timer/timer.c`
- `src/kernel/timer/timer.h`
- `src/tests/test_timer.c`
- `src/kernel/stats/stats.c`
- `src/kernel/trace/trace.c`

### Current Issues

Timer service exists and is functional, but P2 should harden:

- service task stack budget
- command queue full behavior
- delete/stop while callback pending or running
- deterministic wakeup of waiters on delete
- stats for active timers, queue full, late callbacks, callback runtime
- trace events for create/start/stop/delete/fire

### Target Semantics

- `timer_create()` allocates a timer object with a stable lifecycle.
- `timer_start()` and `timer_stop()` enqueue commands atomically or fail with `KERN_ERR_RESOURCE`.
- Queue full never mutates timer state partially.
- `timer_delete()` removes the timer from heap/queue, prevents future callbacks, and wakes any waiter with `KERN_ERR_NOEXIST`.
- If a callback is running, delete marks the timer deleted and final cleanup happens after callback returns.
- Periodic timers reschedule only if still active after callback.
- Timer service task has a documented minimum stack and high-water diagnostics.

### Required Changes

1. Add explicit timer state transitions:
   - `IDLE`
   - `ACTIVE`
   - `RUNNING`
   - `CANCELING`
   - `DELETED`
2. Make command enqueue transactional.
3. Add queue-full stats and trace.
4. Add delete/cancel path that removes heap entries and stale queue commands.
5. Add service task stack high-water reporting if stack instrumentation is available.
6. Add tests:
   - command queue full fails cleanly
   - delete active timer prevents callback
   - delete periodic timer while callback is pending
   - stop inactive timer returns stable status
   - timer service survives callback that starts/stops another timer

### Acceptance

- No timer callback fires after successful delete.
- Timer command queue full returns `KERN_ERR_RESOURCE` and increments stat.
- Periodic timer can be stopped/deleted without late callback.
- Timer trace shows start/fire/stop/delete sequence.
- Existing timer tests and full board suite pass.

## P2-2: IRQ, Threaded IRQ, and Bottom Half

Status: implemented for P2 lifecycle/diagnostics pass; full running-callback zombie state is deferred.

### Files

- `src/kernel/irq/irq.c`
- `src/kernel/irq/irq.h`
- `src/kernel/irq/bh.c`
- `src/kernel/irq/bh.h`
- `src/tests/test_irq.c`
- `docs/IRQ_DESIGN.md`

### Current Issues

IRQ/BH support exists, but P2 should harden:

- BH delete/cancel semantics
- scheduling deleted or canceling BH
- queue or pending overflow behavior
- threaded IRQ release while pending/running
- IRQ mask/unmask policy for threaded handlers
- stack budget for service tasks and IRQ threads
- trace/stats for IRQ/BH lifecycle

### Target Semantics

BH:

- `bh_create()` returns a handle to a deferred callback.
- `bh_schedule()` is ISR-safe and idempotent while pending.
- `bh_cancel()` clears pending work that has not started.
- `bh_delete()` prevents future schedule, cancels pending work, and waits or marks zombie if callback is running.
- Scheduling a deleted BH returns `KERN_ERR_NOEXIST`.

Threaded IRQ:

- ISR stub masks IRQ, records pending, wakes thread, exits quickly.
- Thread executes handler in task context.
- Handler completion acknowledges/unmasks IRQ.
- Release disables IRQ, wakes the thread, and prevents re-entry.
- Duplicate request and invalid IRQ return stable errors.

### Required Changes

1. Add BH states:
   - `FREE`
   - `IDLE`
   - `PENDING`
   - `RUNNING`
   - `CANCELING`
   - `DELETED`
2. Add `bh_cancel(bh_id)` if not already present.
3. Make `bh_delete()` deterministic for pending/running BH.
4. Add BH stats:
   - scheduled
   - coalesced
   - canceled
   - deleted
   - schedule after delete
5. Add threaded IRQ release handshake:
   - disable/mask IRQ
   - mark thread stop requested
   - wake thread
   - clear descriptor after thread acknowledges
6. Add IRQ stats:
   - irq count per registered IRQ
   - masked count
   - threaded wake count
   - spurious/unregistered count
7. Add tests:
   - BH cancel before run
   - BH delete while pending
   - BH schedule after delete
   - threaded IRQ duplicate/release/re-request
   - release threaded IRQ while pending

### Acceptance

- BH delete/cancel leaves no pending work behind.
- Threaded IRQ release cannot call freed handler.
- ISR path remains bounded and does not block.
- IRQ/BH trace and stats expose lifecycle events.
- Existing IRQ tests pass with no wait queue warnings beyond known benign cleanup messages.

## P2-3: Device and Driver Model

Status: implemented for P2 lifecycle/diagnostics pass.

### Files

- `src/kernel/dev/device.c`
- `src/kernel/dev/device.h`
- `src/kernel/vfs/devfs.c`
- `src/kernel/vfs/vfs.c`
- `src/drivers/uart_dev.c`
- `src/drivers/include/uart_dev.h`
- `src/board/stm32f767/board_drivers.c`
- `src/tests/test_driver.c`
- `src/tests/test_vfs.c`

### Current Issues

The device model is useful but still simple:

- probe/remove are not a first-class lifecycle
- devfs nodes and device objects are loosely coupled
- blocking read/write semantics are not uniform
- select/poll or event notification is absent
- driver permissions are not fully modeled through caps

### Target Semantics

Each device has:

- stable device object
- driver ops table
- optional probe/remove hooks
- devfs node binding
- open instance tracking
- common read/write/ioctl behavior
- optional event notification endpoint
- permission model mapped to VFS/file caps

### Required Changes

1. Define `device_driver_t`:
   - `probe(device_t *)`
   - `remove(device_t *)`
   - `open(device_t *, flags)`
   - `close(device_t *)`
   - `read/write/ioctl`
2. Split device allocation from driver binding.
3. Make `devfs_register_device()` bind device lifetime to inode lifetime.
4. Add `device_remove()`:
   - reject busy device or mark removing
   - wake blocking readers/writers
   - unregister devfs node if safe
5. Normalize UART semantics:
   - blocking read if RX empty and blocking mode
   - nonblocking mode returns timeout/busy consistently
   - write returns bytes accepted or stable error
6. Add optional device event notification:
   - endpoint/channel notification for readable/writable/error
   - keep minimal if poll/select is too large for P2
7. Add tests:
   - probe/remove lifecycle
   - open removed device fails
   - duplicate devfs node rejected
   - UART read/write/ioctl through VFS path
   - remove busy device returns `KERN_ERR_BUSY` or wakes according to chosen policy

### Acceptance

- Every devfs node points to a valid device object or fails cleanly.
- Removing a device cannot leave a dangling inode private pointer.
- UART device operations use the same VFS cdev path as `/dev/null`.
- Driver tests cover duplicate registration and removal.

## P2-4: Memory Management and Memory Objects

Status: implemented for P2 diagnostics and capability-groundwork pass.

### Files

- `src/kernel/mem/mem.c`
- `src/kernel/mem/mem.h`
- `src/kernel/mem/mempool.c`
- `src/kernel/mem/mempool.h`
- `src/kernel/cap/capability.c`
- `src/kernel/mpu/mpu.c`
- `src/kernel/include/kernel_types.h`
- `src/tests/test_mpu.c`
- `src/tests/test_capability.c`

### Current Issues

Memory is usable but simple:

- allocations are mostly kernel-internal
- no first-class memory object for shared IPC/channel mappings
- leak statistics are limited
- OOM policy is not explicit
- object slab ownership is not visible

### Target Semantics

P2 introduces kernel memory-object groundwork without fully dynamic user processes:

- memory objects are kernel-tracked allocations or static regions
- memory objects can be referenced by capabilities
- shared memory APIs return cap-backed objects where user exposure is needed
- heap and mempool expose allocation, free, high-water, and failure stats
- OOM returns stable errors and emits trace/stats

### Required Changes

1. Add memory allocation stats:
   - current bytes
   - peak bytes
   - allocation count
   - free count
   - failed allocation count
2. Add optional allocation tags:
   - subsystem id
   - object type
3. Add slab-like pools for fixed kernel objects if useful:
   - timer command nodes
   - IPC request metadata
   - device objects
4. Add `mem_object_t`:
   - base
   - size
   - flags
   - owner task
   - refcount
5. Add memory-object caps:
   - `CAP_OBJ_MEMBLOCK`
   - read/write/map rights
   - cleanup callback releases object when last cap goes away
6. Replace or wrap channel shared memory raw pointer with memory object cap for user-facing paths.
7. Add OOM policy:
   - return `KERN_ERR_RESOURCE`
   - no partial allocation state
   - trace allocation failure

### Acceptance

- Memory stats show current/peak/fail counts.
- Last memory-object cap cleanup releases or marks object free.
- OOM tests fail cleanly without corrupting allocator state.
- Channel shared memory has a migration path to memory-object caps.

## P2-5: Shell and Diagnostic Tools

Status: implemented for P2 diagnostics pass.

### Files

- `src/app/shell.c`
- `src/tests/test_shell.c`
- `src/kernel/stats/stats.c`
- `src/kernel/trace/trace.c`
- `docs/DIAGNOSTIC_GUIDE.md`

### Current Issues

Shell is useful for manual validation but needs better diagnostics:

- command output can be too large
- no paging/rate limiting
- several internals are printed ad hoc
- crash/trace/stats commands need structured views
- shell still exists as kernel app for now

### Target Commands

Minimum P2 shell commands:

- `tasks`: task id/state/priority/stack high-water
- `caps [task]`: cap slots, type, rights, object refs
- `ipc`: endpoint/channel state summaries
- `timers`: active timers, next expiry, queue stats
- `irq`: registered IRQs, threaded IRQs, counts
- `bh`: BH slots, state, scheduled/canceled counts
- `dev`: device table and devfs nodes
- `mem`: heap/mempool current/peak/fail stats
- `trace [n] [filter]`: recent trace events
- `stats`: global counters

### Required Changes

1. Add output pager or line budget:
   - default max lines per command
   - `--all` to print all
2. Add rate limiting for commands that can spam UART.
3. Use structured subsystem dump APIs instead of direct private struct access where possible.
4. Add diagnostic tests for command parser and bounded output.
5. Update `docs/DIAGNOSTIC_GUIDE.md`.

### Acceptance

- Shell commands do not lock the system with unbounded output.
- Diagnostic commands expose timer/IRQ/BH/mem/device state.
- Manual board debugging can inspect P1/P2 objects without recompiling.

## P2-6: Trace and Stats Expansion

Status: implemented for P2 service diagnostics pass.

### Files

- `src/kernel/trace/trace.c`
- `src/kernel/trace/trace.h`
- `src/kernel/stats/stats.c`
- `src/kernel/stats/stats.h`
- `src/tests/test_trace.c`
- `src/tests/test_stats.c`
- `tools/trace_parser.py`

### Current Issues

Trace/stats APIs exist but are not yet rich enough for random failures:

- event classes are too coarse
- payload fields are inconsistent
- queue-full/delete/timeout/cancel paths are not all visible
- stats are not tied to service objects

### Target Semantics

Trace events should capture:

- task id
- object id
- object type
- event code
- result/error code
- timestamp/tick

Stats should include:

- per-subsystem counters
- queue full counts
- timeout counts
- delete/cancel counts
- IRQ/BH counts
- memory allocation failures
- IPC transfer failures

### Required Changes

1. Expand trace event enum:
   - timer start/stop/fire/delete/queue-full
   - IRQ register/fire/release/spurious
   - BH schedule/run/cancel/delete
   - device open/read/write/ioctl/remove
   - memory alloc/free/fail
   - IPC death/timeout/cap-transfer-fail
2. Add typed helper wrappers:
   - `trace_timer(...)`
   - `trace_irq(...)`
   - `trace_bh(...)`
   - `trace_dev(...)`
   - `trace_mem(...)`
3. Add stats APIs:
   - increment by subsystem
   - snapshot for shell/tests
4. Update trace parser to print new event classes.
5. Add tests:
   - event recording and filtering
   - overflow behavior
   - stats increments on controlled failures

### Acceptance

- A queue-full/timer-delete/BH-cancel/IRQ-fire path is visible in trace.
- Stats counters can be read without clearing unless explicitly requested.
- Trace parser recognizes all P2 event classes.

### Progress

- Done: keep `trace_entry_t` ABI at 8 bytes and add P2 event classes for timer/IRQ/BH/device/memory/IPC/cap/VFS.
- Done: add typed trace helper wrappers (`trace_timer`, `trace_irq`, `trace_bh`, `trace_dev`, `trace_mem`, `trace_ipc_event`).
- Done: add generic subsystem counters for OK/error/queue-full/timeout/delete/cancel/busy/noexist.
- Done: add trace/stats regression coverage for typed helpers and subsystem counters.
- Done: update `tools/myrtos_tools.py` event names for P2 trace classes.
- Done: wire timer create/start/stop/reset/change/fire/delete paths into trace/stats.
- Done: classify timer command queue saturation as `QUEUE_FULL` for diagnostics.
- Done: add `bh_cancel()` and make BH create/schedule/run/cancel/delete visible in trace/stats.
- Done: wire IRQ register/release/mask/unmask and threaded dispatch fire/spurious paths into trace/stats.
- Done: add device `probe/remove` API with devfs registration/unregistration and busy-open protection.
- Done: make device open/read/write/ioctl/remove paths visible in trace/stats.
- Done: add memory outstanding/fail/invalid-free diagnostics and trace/stats for kmalloc/kfree.
- Done: add mempool trace/stats for create/alloc/free/delete and exhaustion.
- Done: add memory capability allocation/free helpers for `CAP_OBJ_MEMBLOCK`.
- Done: expand shell `trace` filters to all P2 event classes.
- Done: expand shell `stats`, `mem`, and `free` output with subsystem/live/OOM counters.
- Done: add shell `dev` command for device registry inspection.
- Done: add bounded `trace [n] [event]` output, defaulting to the latest 20 entries.
- Remaining: deeper P3 work only: full timer/BH running-callback zombie states, select/poll, and user-space service migration.

## P2 Execution Order

1. Trace/stats small foundation: add counters/events needed by later P2 tasks.
2. Timer service hardening.
3. BH lifecycle hardening.
4. Threaded IRQ release/mask/unmask hardening.
5. Device/devfs lifecycle and UART semantics.
6. Memory stats and memory-object cap groundwork.
7. Shell diagnostic views and paging.
8. Final integration pass and docs.

This order gives each service a diagnostic surface before deeper behavior changes.

## P2 Checkpoints

### Checkpoint A: Diagnostics Foundation

- `make clean && make` passes.
- Trace/stats tests pass.
- New event/counter APIs compile with all current subsystems.

### Checkpoint B: Timer Service

- Timer queue-full/delete/stop tests pass.
- No callback fires after delete.
- Timer stats/trace visible from shell or tests.

### Checkpoint C: IRQ/BH

- BH cancel/delete tests pass.
- Threaded IRQ release/re-request tests pass.
- IRQ/BH stats expose lifecycle counters.

### Checkpoint D: Device Model

- Device probe/remove tests pass.
- `/dev/null` and `uart0` use common VFS cdev semantics.
- Removing busy devices has documented behavior and tests.

### Checkpoint E: Memory and Diagnostics

- Memory stats tests pass.
- Memory object caps have create/ref/release coverage.
- Shell diagnostics are bounded and useful.

## P2 Completion Criteria

P2 is complete when:

- Timer, IRQ, BH, device, memory, trace, stats, and shell diagnostic paths are deterministic under delete/cancel/full/error cases.
- Every service has tests for normal lifecycle and at least one failure path.
- Queue-full and OOM paths are observable through stats/trace.
- Shell can inspect P1/P2 kernel objects without direct ad hoc traversal in every command.
- The codebase is ready for P3 user-space service migration work.
