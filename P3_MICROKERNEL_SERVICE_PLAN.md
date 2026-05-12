# My-RTOS P3 Microkernel Service Plan

Status: detailed execution plan, P3-1 implementation started.

Scope: STM32F767 mainline. P3 starts from the P2 service diagnostics baseline.
P3 does not move every kernel service to user space at once. It first builds
the contracts that make service migration safe: usercopy, fault cleanup,
request/reply IPC, deterministic service deletion, and one minimal user-space
service path.

## Goal

P3 turns hardened in-kernel services into microkernel-ready service contracts:

- syscall arguments from user tasks are validated before dereference
- task death and user faults release all kernel resources deterministically
- IPC request/reply can support real servers without reply confusion
- timer/BH/threaded IRQ deletion is deterministic while callbacks run
- devices can notify waiters/readers without ad hoc polling
- at least one minimal user-space service request crosses an IPC boundary

## Non-Goals

- No RP2350 or multi-board expansion.
- No dynamic executable loader.
- No complete VFS/devfs/driver migration in one step.
- No scheduler rewrite unless a specific P3 contract requires a small change.
- No POSIX compatibility target; semantics stay My-RTOS native.

## Design Rules

1. User pointers are data, never trusted kernel pointers.
2. All syscall buffer/string arguments must pass through usercopy helpers.
3. User task fault is a task-local failure; kernel fault is still panic.
4. Every task death path must call one cleanup routine.
5. IPC reply authority must be request-scoped, not endpoint-global.
6. Delete/release must have a defined result while callbacks are pending or running.
7. New service-facing APIs must expose trace/stats hooks from P2.
8. Board tests remain the checkpoint after every implementation slice.

## P3-1: Usercopy And Syscall Boundary

Priority: P0.

### Problem

`syscall.c` currently performs some pointer checks inline, but the policy is
local to that file and syscall handlers still pass several user buffers directly
to lower subsystems after validation. P3 needs a single boundary API so later
service migration and MPU tightening do not duplicate pointer logic.

### Files

- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/user_api.h`
- `src/kernel/usercopy/usercopy.c`
- `src/kernel/usercopy/usercopy.h`
- `src/kernel/mpu/mpu.c`
- `src/kernel/mpu/mpu.h`
- `src/tests/test_usercopy.c`
- `Makefile`

### API

```c
typedef enum {
    USER_ACCESS_READ  = 1,
    USER_ACCESS_WRITE = 2,
} user_access_t;

int user_access_ok(const void *ptr, uint32_t len, uint32_t access);
kern_err_t copy_from_user(void *dst, const void *user_src, uint32_t len);
kern_err_t copy_to_user(void *user_dst, const void *src, uint32_t len);
kern_err_t strncpy_from_user(char *dst, const char *user_src, uint32_t max_len);
```

### Semantics

- `len == 0` succeeds for non-NULL and NULL pointers.
- NULL with nonzero length fails.
- Kernel tasks may access known Flash/SRAM ranges for compatibility.
- User tasks must pass current-task MPU region checks.
- Write access requires user-writable MPU permissions.
- String copy is bounded and always NUL-terminates on error.

### Implementation Steps

1. Add the `usercopy` module and Makefile include/source entries.
2. Move existing MPU range logic from `syscall.c` into `usercopy.c`.
3. Replace inline `user_readable/user_writable/user_copy_string` calls.
4. Convert high-risk syscalls to bounce buffers:
   - `open`: bounded path copy
   - `write`: copy user buffer into a bounded kernel buffer before VFS write
   - `read`: read into a bounded kernel buffer then copy back to user
   - IPC send/recv: use fixed-size kernel bounce buffers
5. Add bad pointer tests.

### Acceptance

- Bad path pointer returns `KERN_ERR_PARAM`.
- Bad read/write buffers return `KERN_ERR_PARAM`.
- Kernel address passed by a user task returns `KERN_ERR_PERM` or `KERN_ERR_PARAM`.
- No syscall handler directly dereferences string or buffer user pointers.
- Existing syscall/VFS/IPC tests pass.

### Progress

- Done: added `usercopy` module and Makefile source/include entries.
- Done: moved syscall MPU pointer policy into `usercopy`.
- Done: converted syscall string copies to `strncpy_from_user`.
- Done: converted VFS `read`/`write` syscalls to bounded kernel bounce buffers.
- Done: added focused `test_usercopy` coverage and bad syscall pointer tests.
- Done: converted mqueue, endpoint, and channel IPC payload/cap-transfer syscall
  paths to kernel bounce buffers.
- Remaining: define command-specific `ioctl` pointer policy if device ioctls start
  accepting user buffers.

## P3-2: Fault Cleanup And Task Death Contract

Priority: P0.

### Problem

P1/P2 improved task deletion and object cleanup, but fault, exit, and delete
paths still need a single resource cleanup contract. Without this, user-space
services can leave wait queues, fd refs, caps, and reply states behind after a
fault.

### Files

- `src/kernel/task/task.c`
- `src/kernel/task/task.h`
- `src/kernel/fault/fault.c`
- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/channel.c`
- `src/kernel/cap/capability.c`
- `src/kernel/vfs/vfs.c`
- `src/tests/test_fault.c`
- `src/tests/test_task.c`

### Target API

```c
kern_err_t task_cleanup_resources(tcb_t *task, kern_err_t reason);
kern_err_t task_terminate_with_result(tcb_t *task, kern_err_t result);
```

### Cleanup Scope

- remove from all wait queues
- clear IPC endpoint/channel pending states
- invalidate reply tokens or reply waits
- close fd table entries
- revoke task-owned capabilities
- release joiners with deterministic result
- clear pending sleep/block state

### Acceptance

- User task fault does not panic the kernel.
- Other tasks continue running after user task fault.
- Faulting task caps and fds are released.
- `task_join()` returns a deterministic fault result for faulted tasks.
- Wait queue diagnostics do not show stale blocked tasks.

### Progress

- Done: added explicit task termination result storage in `tcb_t`.
- Done: retained task termination result across delayed reclaim.
- Done: added `task_terminate_with_result()` for fault/delete style callers.
- Done: user fault path now terminates with `KERN_ERR_FAULT`.
- Done: `task_join()` now reports fault termination distinctly from normal exit.
- Done: added fd ref cleanup coverage for a faulted user task.
- Done: added cap revoke coverage for a faulted user task.
- Remaining: board-run validation for the new fault result contract.

## P3-3: Timer/BH/IRQ Running-State Deletion

Priority: P1.

### Problem

P2 added cancellation and diagnostics. P3 must define what happens when delete
or release races with a callback currently running in service task context.

### Files

- `src/kernel/timer/timer.c`
- `src/kernel/timer/timer.h`
- `src/kernel/irq/bh.c`
- `src/kernel/irq/bh.h`
- `src/kernel/irq/irq.c`
- `src/tests/test_timer.c`
- `src/tests/test_irq.c`

### Target States

Timer:

- `IDLE`
- `ACTIVE`
- `RUNNING`
- `CANCELING`
- `DELETED`

BH:

- `FREE`
- `IDLE`
- `PENDING`
- `RUNNING`
- `CANCELING`
- `DELETED`

Threaded IRQ:

- `FREE`
- `REGISTERED`
- `PENDING`
- `RUNNING`
- `STOPPING`

### Acceptance

- Delete while pending prevents future callback.
- Delete while running marks object for final cleanup after callback returns.
- Release threaded IRQ while handler is running cannot call freed handler.
- Trace/stats records cancel/delete/running cleanup.

### Progress

- Done: P3-2 board-run validation passed.
- Done: BH descriptors now track `running` and `delete_pending`.
- Done: deleting a running BH marks it for cleanup after the handler returns
  and prevents future schedules.
- Done: threaded IRQ descriptors now track `running` and `stopping`.
- Done: releasing a threaded IRQ from its own handler marks stop and lets the
  IRQ thread exit itself instead of clearing a live descriptor.
- Done: added BH delete-while-running regression coverage.
- Done: board-run validation passed for BH/threaded IRQ lifecycle changes.
- Done: timers now track `delete_pending` after delete is requested.
- Done: delete requested from a timer callback prevents periodic reinsertion
  and blocks later start/reset/change calls.
- Done: added timer delete-while-running regression coverage.
- Remaining: board-run validation for timer delete-pending changes.

## P3-4: Endpoint Request/Reply Contract

Priority: P1.

### Problem

Endpoint reply state must become request-scoped. A microkernel server may handle
multiple clients and must not reply to the wrong sender after timeout,
cancelation, or deletion.

### Files

- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/endpoint.h`
- `src/kernel/ipc/ipc_transfer.c`
- `src/kernel/cap/capability.c`
- `src/tests/test_ipc_upgrade.c`

### Target Model

Each pending request stores:

- sender task
- server task that received it
- message buffer
- reply token/cap
- timeout state
- transferred caps
- request generation

### New Semantics

- `endpoint_recv()` returns request identity.
- `endpoint_reply()` consumes request-scoped reply authority.
- Client timeout invalidates reply authority.
- Endpoint delete wakes senders/receivers/repliers with `KERN_ERR_NOEXIST`.
- Capability transfer rollback is atomic on failure.

### Acceptance

- Multi-client calls cannot reply cross-wired.
- Reply after timeout fails cleanly.
- Delete endpoint invalidates outstanding reply tokens.
- IPC cap transfer tests cover copy, move, rollback, and revoke.

### Progress

- Done: P3-3 timer delete-pending board validation passed.
- Done: endpoint reply authority no longer falls back to endpoint-global sender.
- Done: endpoint requests now carry per-request generation IDs.
- Done: server reply validates sender state and request generation before writing
  into the client reply buffer.
- Done: added regression coverage that a task which did not receive the request
  cannot reply to it.
- Done: board-run validation passed for request-scoped reply binding.
- Done: added regression coverage that reply after client timeout fails cleanly.
- Done: board-run validation passed for reply-after-timeout coverage.
- Done: IPC cap copy/move rollback is covered by existing capability tests.
- Done: P3-4 endpoint request/reply contract baseline complete.

## P3-5: Device Event Notification

Priority: P2.

### Problem

Device operations are unified through devfs, but readable/writable/error events
are not exposed. Before moving drivers to user space, the kernel needs a minimal
notification model.

### Files

- `src/kernel/dev/device.c`
- `src/kernel/dev/device.h`
- `src/kernel/vfs/devfs.c`
- `src/drivers/uart_dev.c`
- `src/kernel/ipc/endpoint.c`
- `src/tests/test_driver.c`

### Target Model

- Device can hold optional notification endpoint/channel.
- Driver marks readable/writable/error.
- Blocking read/write can wait on notification or return stable nonblocking error.
- Device remove wakes blocked readers/writers.

### Acceptance

- UART or a test device exposes readable notification.
- Remove invalidates or wakes blocked operations.
- Trace/stats records device event paths.

### Progress

- Done: device descriptors now hold a generic `DEVICE_EVENT_*` bitmask.
- Done: added `device_notify_events`, `device_clear_events`, and
  `device_get_events`.
- Done: devfs supports generic get/clear event ioctls before driver-specific
  ioctls.
- Done: added driver test coverage for notify/get/clear event semantics.
- Done: board-run validation passed for generic device event ioctls.
- Done: UART driver now marks writable/readable state through generic device
  events during registration and read/write/open paths.
- Done: added `/dev/uart0` writable event coverage.
- Done: board-run validation passed for UART device events.

## P3-6: First User-Space Service Candidate

Priority: P2.

### Problem

P3 should prove that at least one real request can be served outside the kernel
using endpoint/cap/syscall contracts, while preserving kernel fallback paths.

### Candidate Order

1. diagnostic shell facade
2. simple name registry
3. device manager facade
4. devfs facade

### Minimal Service Contract

- root/kernel creates service task
- service receives endpoint cap
- client sends request by endpoint cap
- service replies with result
- faulting service task is cleaned up without kernel panic

### Acceptance

- One user service handles one real request through IPC.
- Service fault cleanup works.
- Existing shell/kernel path remains available.

### Progress

- Constraint: blocking endpoint syscalls currently sleep inside the SVC handler
  path (`endpoint_send/recv` wait loops). This can deadlock because the handler
  does not return to thread mode before waiting.
- Reverted: the first user endpoint service regression test was removed until
  P3-6 adds a safe sleepable-syscall or async IPC syscall contract.
- Done: adopted the P3-6 phase-A contract: user services use nonblocking IPC
  syscalls (`timeout == 0`) until sleepable syscall continuations exist.
- Done: re-enabled the first user endpoint service test with nonblocking
  `sys_ep_recv`; the kernel test client uses direct endpoint send from thread
  mode so scheduling remains safe.
- Remaining: board-run validation for the nonblocking user-space service path.

## Execution Order

1. P3-1 usercopy module and syscall boundary.
2. P3-2 task death cleanup.
3. P3-3 timer/BH/IRQ running-state deletion.
4. P3-4 endpoint request/reply contract.
5. P3-5 device event notification.
6. P3-6 first user-space service candidate.

## Checkpoints

### Checkpoint A: Usercopy Foundation

- `make` passes.
- `test_usercopy` passes.
- bad syscall pointers return stable errors.

### Checkpoint B: Death Cleanup

- user task fault does not panic.
- caps/fds/wait queues are cleaned.

### Checkpoint C: Callback Deletion

- timer/BH/IRQ delete/release during running callback is deterministic.

### Checkpoint D: Request/Reply IPC

- endpoint request/reply is request-scoped and timeout-safe.

### Checkpoint E: Device Events

- device notification exists and remove wakes waiters.

### Checkpoint F: First Service

- one user service serves one IPC request.

## P3 Done Criteria

- Malicious user pointers cannot crash the kernel through syscalls.
- User task death cleanup is deterministic.
- Request/reply IPC supports safe multi-client service behavior.
- Device notification exists for at least UART or a test device.
- At least one minimal service request crosses a user-space IPC boundary.
