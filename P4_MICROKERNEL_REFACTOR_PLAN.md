# My-RTOS P4 Microkernel Refactor Plan

Status: Phase 6 memory-object slice in progress.

Scope: STM32F767 mainline first. Keep the current `make BOARD=stm32f767`
workflow, board test harness, and UART shell as the validation loop. P4 is a
refactor phase: it should reduce microkernel blockers without breaking existing
RTOS behavior.

## Goal

Move My-RTOS from "RTOS with microkernel mechanisms" toward "minimal kernel +
user-space services".

P4 does not need to migrate every service in one pass. It must first close the
architectural holes that would make service migration unsafe:

- raw-id syscall authority
- user callback execution inside kernel service tasks
- incomplete sleepable syscall semantics
- missing root/init bootstrap
- missing service discovery
- missing IRQ/MMIO/shared-memory capability model

## Non-Goals

- No RP2350 stabilization work in this phase.
- No dynamic executable loader.
- No POSIX compatibility layer.
- No full C library port.
- No broad scheduler rewrite unless required by IPC priority inheritance.
- No one-shot rewrite of VFS, drivers, and shell together.

## Refactor Rules

1. Preserve board-test pass after every slice.
2. Add negative tests before or with every security boundary change.
3. Keep old kernel APIs usable internally until the replacement path is tested.
4. Do not expose raw kernel pointers to user tasks.
5. Do not let user tasks install callbacks that execute in kernel service tasks.
6. All new user-visible object access must go through capability lookup.
7. Every blocking syscall must either be continuation-safe or explicitly reject
   blocking with a stable error.
8. Service migration should be incremental: add IPC service path first, then
   switch shell/tests to it, then shrink old in-kernel path.

## Completion Definition

P4 is complete when:

- all user-facing task/timer/IRQ/BH/device/file syscalls use capabilities, not raw ids
- invalid SVC numbers cannot wedge the system
- endpoint, channel, sem, mutex, mqueue, event, task delay, and join have a
  clear sleepable syscall policy
- timer/IRQ/BH no longer execute user-provided callbacks in privileged service
  context
- root/init can start at least name server and one user service
- a user client can discover one service through name server and call it through IPC
- IRQ notification and MMIO capability have first usable tests or stubs with
  strict rejection behavior

## Phase 0: Baseline And Guardrails

Priority: P0.

### Purpose

Freeze the known-good P3 state and make future refactors easy to bisect.

### Work

1. Add `P4` status marker to docs.
2. Record current passing test count from board run.
3. Add a small `test_security_negative` module if it does not already exist.
4. Add regression tests for current known risks before changing behavior:
   - invalid syscall number returns `KERN_ERR_PARAM`
   - invalid SVC immediate does not hang
   - user task cannot suspend/delete another task by raw task id
   - blocking unsupported syscall returns stable `KERN_ERR_BUSY`

### Files

- `src/tests/test_syscall.c`
- `src/tests/test_task.c`
- `src/tests/test_capability.c`
- optional `src/tests/test_security.c`
- `Makefile`

### Acceptance

- Existing board tests still pass.
- At least one failing or skipped test captures every P4 high-risk gap before
  the implementation slice fixes it.

## Phase 1: Syscall Authority Closure

Priority: P0.

### Problem

Some user-facing syscalls still accept raw ids or raw callback pointers. A
microkernel cannot let users guess object ids or install code that later runs in
kernel context.

### Work

1. Convert task control syscalls to cap lookup:
   - `sys_task_suspend`
   - `sys_task_resume`
   - `sys_task_delete`
   - future `sys_task_join` if exposed
2. Audit all syscall table entries for raw-id access.
3. Split APIs into:
   - internal kernel APIs that may use ids/pointers
   - syscall APIs that require capabilities
4. Replace user callback syscalls with fail-fast behavior until notification
   APIs are ready:
   - `sys_irq_register`
   - `sys_bh_create`
   - `sys_timer_create`
5. Add capability rights checks for:
   - task manage
   - timer manage/write
   - IRQ bind/manage
   - BH manage
   - device read/write/ioctl
6. Add tests proving one task cannot control another task without cap transfer.

### Files

- `src/kernel/syscall/syscall.c`
- `src/kernel/cap/capability.c`
- `src/kernel/cap/capability.h`
- `src/kernel/task/task.c`
- `src/kernel/task/task.h`
- `src/tests/test_syscall.c`
- `src/tests/test_capability.c`
- `src/tests/test_task.c`

### Acceptance

- User task cannot suspend/resume/delete by raw task id.
- User task with a task cap and `CAP_MANAGE` can perform allowed management.
- User callback registration from syscall is rejected unless the new
  notification path is used.
- Kernel-internal tests can still create timer/IRQ/BH handlers through internal APIs.

## Phase 2: SVC And Syscall ABI Hardening

Priority: P0.

### Problem

The SVC assembly path still treats unknown SVC immediates as a fatal spin. The
syscall ABI also lacks a centralized argument policy.

### Work

1. Add SVC immediate validation in `svc_handler.S`.
2. Unknown SVC from thread mode should terminate the current user task or return
   a stable error if the stacked frame can be safely patched.
3. Unknown SVC from kernel/handler mode should panic with crash dump.
4. Keep `svc #0` first switch privileged-only.
5. Add syscall metadata for argument classes:
   - scalar
   - input buffer
   - output buffer
   - string
   - cap
6. Use metadata initially for debug assertions and tests; later it can drive
   automated validation.

### Files

- `src/arch/arm/cortex-m7/svc_handler.S`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/syscall.h`
- `src/kernel/fault/fault.c`
- `src/tests/test_syscall.c`
- `src/tests/test_fault.c`

### Acceptance

- `svc #2` from a user task cannot hang the board.
- `svc #0` cannot be re-used by a running user task to restart first switch.
- Invalid syscall number still returns `KERN_ERR_PARAM`.
- Existing user syscall tests pass.

## Phase 3: Unified Sleepable Syscall Continuation

Priority: P0.

### Problem

Endpoint send/recv has a continuation path. Other blocking syscalls still use
old C-call blocking semantics or explicitly reject blocking. P4 needs one
shared model.

### Target Design

Add a generic blocked syscall record in `tcb_t`:

```c
typedef struct {
    uint8_t active;
    uint8_t op;
    uint16_t flags;
    void *object;
    void *user_buf0;
    void *user_buf1;
    uint32_t len0;
    uint32_t len1;
    uint32_t aux0;
    uint32_t aux1;
} syscall_cont_t;
```

The SVC path only needs to know whether a syscall returned
`KERN_SYSCALL_BLOCKED`. The subsystem owns cancel/complete callbacks.

### Work Order

1. Generalize existing endpoint `syscall_blocked` state into `syscall_cont_t`.
2. Add helpers:
   - `syscall_block_current(op, obj, timeout)`
   - `syscall_complete(tcb, result)`
   - `syscall_cancel(tcb, result)`
3. Convert endpoint send/recv to the generic helper.
4. Convert task delay and join.
5. Convert semaphore wait.
6. Convert mutex lock.
7. Convert mqueue send/recv.
8. Convert event wait.
9. Convert channel send/recv.
10. Re-enable cap-bearing endpoint/channel blocking only after cap-transfer
    continuation is implemented.

### Files

- `src/kernel/include/kernel_types.h`
- `src/kernel/task/task.c`
- `src/kernel/task/task.h`
- `src/kernel/core/scheduler.c`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/syscall.h`
- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/channel.c`
- `src/kernel/ipc/semaphore.c`
- `src/kernel/ipc/mutex.c`
- `src/kernel/ipc/mqueue.c`
- `src/kernel/ipc/event.c`
- `src/tests/test_syscall.c`

### Acceptance

- User task can block in delay/join/sem/mutex/mqueue/event/channel without SVC
  handler spin or board hang.
- Timeout wakes the task and writes the syscall return value into the saved frame.
- Deleting the waited object wakes the user task with `KERN_ERR_NOEXIST`.
- Deleting/faulting the blocked task removes it from the subsystem wait queue.

## Phase 4: Reply Capability And IPC ABI

Priority: P1.

### Problem

Endpoint reply binding exists, but reply authority is still endpoint/server-task
state rather than a first-class reply object visible to the IPC contract.

### Work

1. Add `CAP_OBJ_REPLY`.
2. Create reply object when server receives a call.
3. Make reply cap single-use.
4. Invalidate reply cap on:
   - client timeout
   - client death
   - endpoint deletion
   - server death
   - successful reply
5. Add message header:
   - opcode
   - flags
   - badge
   - length
   - cap count
6. Define stable user IPC ABI in `user_api.h`.
7. Re-enable endpoint cap-bearing blocking IPC through continuation.

### Files

- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/endpoint.h`
- `src/kernel/ipc/ipc_transfer.c`
- `src/kernel/ipc/ipc_transfer.h`
- `src/kernel/cap/capability.c`
- `src/kernel/cap/capability.h`
- `src/kernel/syscall/user_api.h`
- `src/tests/test_ipc_upgrade.c`
- `src/tests/test_syscall.c`

### Acceptance

- Server replies using reply cap, not endpoint id alone.
- Replying twice fails.
- Client timeout invalidates reply cap.
- Cap-bearing blocking endpoint send/recv works and rolls back on receiver
  CSpace exhaustion.

## Phase 5: Timer, IRQ, And BH Notification Model

Priority: P1.

### Problem

Timer/BH/threaded IRQ currently execute callback pointers in privileged service
tasks. That is acceptable for RTOS internals but not for user-space services.

### Work

1. Keep internal callback APIs for kernel-only use.
2. Add user-facing notification APIs:
   - `timer_bind(timer_cap, endpoint_cap, badge)`
   - `irq_bind(irq_cap, endpoint_cap, badge)`
   - `bh_bind(bh_cap, endpoint_cap, badge)` or remove user-facing BH creation
3. IRQ arrival path:
   - mask IRQ
   - mark pending
   - send notification to bound endpoint
   - wait for `irq_ack`
   - unmask if still bound
4. Timer expiry path:
   - send notification message
   - avoid user callback execution
5. BH path:
   - keep as internal kernel deferral or convert to notification for specific use cases
6. Add caps:
   - `CAP_OBJ_IRQ`
   - `CAP_OBJ_TIMER`
   - optional `CAP_OBJ_BH`

### Files

- `src/kernel/timer/timer.c`
- `src/kernel/timer/timer.h`
- `src/kernel/irq/irq.c`
- `src/kernel/irq/irq.h`
- `src/kernel/irq/bh.c`
- `src/kernel/irq/bh.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/user_api.h`
- `src/tests/test_timer.c`
- `src/tests/test_irq.c`

### Acceptance

- User timer expiry wakes a user service through endpoint notification.
- User IRQ handler is not a function pointer in kernel context.
- IRQ remains masked if the bound service dies before ack.
- Kernel internal timer/BH tests still pass.

## Phase 6: Memory Objects, Shared Memory, And MMIO Capabilities

Priority: P1.

### Problem

User services need controlled data exchange and controlled peripheral access.
Current MPU setup is intentionally tight: user Flash RO plus user stack RW. That
is safe but too limited for FS/driver services.

### Work

1. Add memory object type:
   - size
   - owner
   - rights
   - backing storage
   - mapped task list
2. Add shared memory APIs:
   - `shm_create(size)`
   - `shm_map(shm_cap, rights)`
   - `shm_unmap(shm_cap)`
   - `shm_grant(dst, shm_cap, rights)`
3. Add MMIO cap type:
   - base
   - size
   - allowed access width
   - device/IRQ association
4. Add MPU region allocator for user tasks.
5. Add tests for MPU region exhaustion and cleanup on task death.

### Files

- `src/kernel/mem/mem.c`
- `src/kernel/mem/mem.h`
- `src/kernel/mpu/mpu.c`
- `src/kernel/mpu/mpu.h`
- `src/kernel/cap/capability.c`
- `src/kernel/include/kernel_types.h`
- `src/kernel/syscall/syscall.c`
- new `src/kernel/mem/shm.c`
- new `src/kernel/mem/shm.h`
- `src/tests/test_mem.c`
- `src/tests/test_mpu.c`

### Acceptance

- User task cannot access unmapped shared memory.
- Granted shared memory maps only with granted rights.
- Task death unmaps all shared memory regions.
- MMIO cap cannot map arbitrary SRAM/kernel memory.

## Phase 7: Root/Init And Initial Capability Bootstrap

Priority: P2.

### Problem

The kernel still directly starts built-in services and apps. A microkernel needs
a first user task that receives initial authority and starts the rest of the
system.

### Work

1. Add root/init task creation after kernel core init.
2. Give root/init initial caps:
   - root task management cap
   - name server endpoint creation rights
   - selected device caps
   - selected IRQ caps
   - selected MMIO caps
   - memory allocator cap
3. Move service startup policy out of `kernel.c` where possible.
4. Keep compatibility path for existing tests until root/init test path is stable.
5. Add root/init crash behavior:
   - first version may panic
   - later version should enter recovery shell or reboot

### Files

- `src/kernel/kernel.c`
- `src/kernel/system_init.c`
- `src/kernel/task/task.c`
- `src/kernel/cap/capability.c`
- new `src/user/init/init.c`
- new `src/user/init/init.h`
- `Makefile`
- `src/tests/test_service_model.c`

### Acceptance

- root/init runs as a user task.
- root/init receives initial caps through a controlled bootstrap path.
- root/init can create/start one user service.
- Existing board tests still run through the compatibility path or root/init path.

## Phase 8: Name Server

Priority: P2.

### Problem

Services need discovery. Hard-coded global endpoint ids are not enough once
drivers and FS move to user space.

### Work

1. Define name server IPC protocol:
   - register service name -> endpoint cap
   - lookup service name -> derived endpoint cap
   - unregister service
2. Name server runs as user task.
3. root/init starts name server and transfers/registers initial caps.
4. Add client helper wrappers in `user_api.h` or a small user runtime header.
5. Add access policy:
   - first version can be simple allowlist
   - later version can use caller badge/cap rights

### Files

- new `src/user/nameserver/nameserver.c`
- new `src/user/nameserver/nameserver.h`
- `src/kernel/syscall/user_api.h`
- `src/tests/test_service_model.c`
- `Makefile`

### Acceptance

- root/init starts name server.
- A user client can look up a service endpoint by name.
- Name server fault does not corrupt kernel state.
- Service endpoint caps are derived with reduced rights when appropriate.

## Phase 9: Driver Server Foundation

Priority: P2.

### Problem

Device and UART/GPIO logic still live in kernel-side driver framework. The next
step is not full driver migration; it is a reusable user-space driver server
pattern.

### Work

1. Define driver server protocol:
   - open
   - close
   - read
   - write
   - ioctl
   - poll/event
2. Implement UART server prototype.
3. Driver server receives:
   - MMIO cap
   - IRQ cap
   - endpoint cap
4. Shell/client talks to UART server through IPC for at least one operation.
5. Keep old kernel UART path for debug console until replacement is stable.

### Files

- new `src/user/drivers/uart_server.c`
- new `src/user/drivers/driver_proto.h`
- `src/kernel/irq/irq.c`
- `src/kernel/mem/shm.c`
- `src/drivers/uart_dev.c`
- `src/app/shell.c`
- `src/tests/test_driver.c`
- `src/tests/test_service_model.c`

### Acceptance

- UART server can receive one request over IPC and reply.
- IRQ notification can wake UART server if enabled.
- UART server fault masks/revokes its IRQ/MMIO caps.
- Debug UART path remains available for panic and shell during transition.

## Phase 10: FS Server Foundation

Priority: P3.

### Problem

VFS is currently an in-kernel VFS. True microkernel structure requires file
service logic in user space.

### Work

1. Define FS protocol:
   - open
   - close
   - read
   - write
   - readdir
   - lseek
   - stat
   - ioctl pass-through policy
2. Implement ramfs server prototype.
3. Add fd broker model:
   - kernel fd cap remains temporary compatibility layer
   - user FS server returns service fd token
4. Move shell `ls/cat` through FS server behind a config flag.
5. Keep kernel VFS path until FS server passes equivalent tests.

### Files

- new `src/user/fs/fs_server.c`
- new `src/user/fs/fs_proto.h`
- `src/kernel/vfs/vfs.c`
- `src/kernel/vfs/ramfs.c`
- `src/kernel/vfs/devfs.c`
- `src/app/shell.c`
- `src/tests/test_vfs.c`
- `src/tests/test_service_model.c`

### Acceptance

- User client can `open/read/readdir` through FS server.
- FS server crash returns deterministic errors to clients.
- Existing kernel VFS tests still pass while compatibility path exists.

## Phase 11: Service Supervisor And Recovery

Priority: P3.

### Problem

Once services move to user space, service failure must not become system failure.

### Work

1. Add supervisor service or extend root/init.
2. Track service tasks and their owned caps.
3. On service death:
   - revoke service caps
   - notify dependent clients if possible
   - restart service if policy says restart
4. Add service health stats:
   - restart count
   - last fault reason
   - pending client count
5. Add shell diagnostics for services.

### Files

- `src/user/init/init.c`
- new `src/user/supervisor/supervisor.c`
- `src/kernel/task/task.c`
- `src/kernel/cap/capability.c`
- `src/app/shell.c`
- `src/kernel/stats/stats.c`
- `src/tests/test_service_model.c`

### Acceptance

- Faulting user service does not panic.
- Clients blocked on the service are woken or time out deterministically.
- Supervisor can restart a simple service.
- Trace/stats show service crash and restart.

## Phase 12: Documentation And ABI Freeze

Priority: P3.

### Work

1. Write syscall ABI document.
2. Write IPC ABI document.
3. Write capability object/right matrix.
4. Write service protocols:
   - name server
   - driver
   - FS
5. Update README status table.
6. Add a "known limitations" list per release tag.

### Files

- new `docs/SYSCALL_ABI.md`
- new `docs/IPC_ABI.md`
- new `docs/CAPABILITY_MODEL.md`
- new `docs/SERVICE_PROTOCOLS.md`
- `README.md`

### Acceptance

- A new contributor can identify which interfaces are stable and which are
  internal.
- Every user-visible syscall and IPC message has documented arguments,
  capability requirements, return values, and failure modes.

## Recommended Execution Order

Use this order to minimize breakage:

1. Phase 0: baseline tests and negative tests.
2. Phase 1: close syscall authority holes.
3. Phase 2: SVC hardening.
4. Phase 3: generic sleepable syscall continuation.
5. Phase 4: reply cap and cap-bearing blocking IPC.
6. Phase 5: timer/IRQ/BH notification model.
7. Phase 6: shared memory and MMIO caps.
8. Phase 7: root/init bootstrap.
9. Phase 8: name server.
10. Phase 9: UART driver server prototype.
11. Phase 10: ramfs/FS server prototype.
12. Phase 11: supervisor and recovery.
13. Phase 12: ABI docs.

## Board Checkpoints

Checkpoint A, after Phase 2:

- all current tests pass
- invalid SVC cannot hang
- raw task id management from user is rejected

Checkpoint B, after Phase 3:

- all blocking syscall tests pass
- no SVC-side spin waits
- timeout/delete/fault cleanup works for every blocking object

Checkpoint C, after Phase 5:

- user timer notification works
- user IRQ callback path is removed or rejected
- IRQ notification test passes

Checkpoint D, after Phase 8:

- root/init starts name server
- user client discovers one service by name
- service fault leaves kernel alive

Checkpoint E, after Phase 10:

- UART server prototype handles one request
- FS server prototype handles `readdir/read`
- shell can use at least one user-space service path

## Risk Register

| Risk | Impact | Mitigation |
| --- | --- | --- |
| SVC assembly frame mismatch | hard fault or wrong return value | keep static offset asserts and add SVC regression tests |
| continuation state leaks | stuck tasks or wrong syscall result | centralize cancel/complete helper and test timeout/delete/fault |
| cap revoke breaks existing tests | broad failures | keep privileged internal bypass explicit and test user path separately |
| IRQ notification increases latency | missed real-time target | keep urgent ISR work minimal and measure trace/stats latency |
| MPU region exhaustion | user services cannot map shm/MMIO | add region allocator tests and deterministic `KERN_ERR_RESOURCE` |
| service migration breaks shell | poor debugging | keep kernel debug UART path until service path is stable |

## First Implementation Slice

Start with a small, high-value slice:

1. Add negative tests for raw task id management and invalid SVC.
2. Convert `sys_task_suspend/resume/delete` to task capability lookup.
3. Add SVC invalid immediate handling.
4. Board-test.

This slice closes real security holes without requiring service migration yet.

### Progress

- Done: added P4 refactor plan.
- Done: added negative tests for invalid syscall number, invalid SVC immediate,
  repeated `svc #0`, and user raw task-id management attempts.
- Done: converted `sys_task_suspend`, `sys_task_resume`, and
  `sys_task_delete` to resolve `CAP_OBJ_TASK` with `CAP_MANAGE`.
- Done: `sys_task_delete` now deletes the task capability after successful
  deletion.
- Done: `svc_handler.S` rejects unknown SVC immediates with `KERN_ERR_PARAM`.
- Done: `svc_handler.S` rejects repeated `svc #0` after the scheduler has a
  current task with `KERN_ERR_STATE`.
- Done: local `make BOARD=stm32f767` passes for the SVC/task-cap slice.
- Done: board-run validation passed for the SVC/task-cap slice.
- Done: user-facing timer, IRQ, and BH callback registration syscalls now return
  `KERN_ERR_PERM` for user tasks until notification APIs replace callback
  execution.
- Done: user-facing raw BH schedule now requires a `CAP_OBJ_BH` write cap.
- Done: added user-task regression coverage for callback syscall rejection.
- Done: board-run validation passed for the callback rejection slice.
- Done: converted `sys_task_delay` to a sleepable syscall continuation instead
  of calling `task_delay()` inside the SVC handler.
- Done: scheduler timeout wake now completes `BLOCK_REASON_SLEEP` with
  `KERN_OK` while preserving `KERN_ERR_TIMEOUT` for other blocking objects.
- Done: local `make BOARD=stm32f767` passes for the task-delay continuation
  slice.
- Done: board-run validation passed for the task-delay continuation slice.
- Done: added `sem_wait_syscall()` so user `SYSCALL_SEM_WAIT` can block through
  a saved SVC continuation rather than spinning in the handler.
- Done: added user-task semaphore wait wake and timeout regression coverage.
- Done: local `make BOARD=stm32f767` passes for the semaphore wait
  continuation slice.
- Done: board-run validation passed for the semaphore wait continuation slice.
- Done: added `mutex_lock_syscall()` so user `SYSCALL_MUTEX_LOCK` can block
  through a saved SVC continuation rather than spinning in the handler.
- Done: added user-task mutex lock wake and timeout regression coverage.
- Done: local `make BOARD=stm32f767` passes for the mutex lock continuation
  slice.
- Done: board-run validation passed for the mutex lock continuation slice.
- Done: added `mqueue_send_syscall()` and `mqueue_recv_syscall()` with per-task
  syscall message/user-buffer state so mqueue blocking no longer waits inside
  the SVC handler.
- Done: added user-task mqueue recv wake, recv timeout, and send wake
  regression coverage.
- Done: local `make BOARD=stm32f767` passes for the mqueue continuation slice.
- Done: board-run validation passed for the mqueue continuation slice.
- Done: added `event_wait_syscall()` so user `SYSCALL_EVENT_WAIT` can block
  through a saved SVC continuation rather than spinning in the handler.
- Done: tightened `event_set()` waiter handling so only valid task wait records
  are evaluated and completed.
- Done: added user-task event wait wake and timeout regression coverage.
- Done: local `make BOARD=stm32f767` passes for the event wait continuation
  slice.
- Done: board-run validation passed for the event wait continuation slice.
- Done: added initial no-cap `channel_send_syscall()` and
  `channel_recv_syscall()` continuation paths.
- Done: added user-task channel recv wake, recv timeout, and send wake
  regression coverage.
- Done: local `make BOARD=stm32f767` passes for the channel continuation slice.
- Done: board-run validation passed for the channel continuation slice.
- Done: added user-task delete-wakeup regression coverage for semaphore, event,
  and channel recv sleepable syscalls.
- Done: `event_delete()` and `channel_delete()` now clear per-task syscall
  continuation scratch state while waking blocked user syscalls.
- Done: local `make BOARD=stm32f767` passes for the delete-wakeup regression
  slice.
- Done: board-run validation passed for the delete-wakeup regression slice.
- Done: added `CAP_OBJ_REPLY`, endpoint reply objects, and `sys_ep_take_reply()`
  so a server can receive a request, obtain a reply cap, and reply through that
  cap.
- Done: reply caps are invalidated on successful reply, endpoint deletion,
  sender cleanup, and stale/dead-client reply paths.
- Done: added user-task reply-cap regression coverage, including single-use
  reply behavior.
- Done: local `make BOARD=stm32f767` passes for the reply-cap first slice.
- Done: board-run validation passed for the reply-cap first slice.
- Done: added and board-validated reply-cap invalidation coverage for client
  timeout after the server has received a request.
- Done: added first sleepable cap-bearing endpoint send syscall path:
  `sys_ep_send_caps()` can queue user-supplied cap transfers, block through the
  syscall continuation path, and be received by a kernel server through
  `endpoint_recv_caps()`.
- Done: board-run validation passed for sleepable `sys_ep_send_caps()`.
- Done: fixed `sys_call4/5/6` SVC wrapper stack alignment so Cortex-M exception
  entry does not insert a padding word that shifts a4/a5/a6.
- Done: added first sleepable cap-bearing endpoint receive syscall path:
  `sys_ep_recv_caps()` can block with user output cap buffers and receive caps
  from a kernel `endpoint_send_caps()` caller.
- Done: board-run validation passed for sleepable `sys_ep_recv_caps()`.
- Done: cleaned temporary syscall diagnostic assertions after board validation.
- Done: added sleepable cap-bearing channel send/receive syscall paths:
  `sys_ch_send_caps()` can queue user cap transfers while blocked on a full
  channel slot, and `sys_ch_recv_caps()` can block with user output cap buffers.
- Done: added user-task channel cap-transfer regression coverage for
  user-to-kernel `sys_ch_send_caps()` and kernel-to-user `sys_ch_recv_caps()`.
- Done: local `make BOARD=stm32f767` passes for the channel cap-transfer
  syscall slice.
- Done: split capability delete and revoke semantics: `cap_delete()` and
  task-exit `cap_revoke_all()` now remove owned CSpace entries without
  cascading into IPC-copied child caps; explicit `cap_revoke()` still cascades.
- Done: added capability regression coverage that parent `cap_revoke()`
  invalidates children while parent `cap_delete()` preserves derived children.
- Done: fixed channel receive-side peer death handling: a message that has
  already been committed to a channel slot remains receivable even if the
  sender exits before the receiver resumes; send-side validation still requires
  a live peer and empty receives still report peer death.
- Done: added the first timer notification path: `endpoint_notify()` provides
  one-way endpoint delivery without waiting for a reply, and
  `timer_bind_endpoint()` sends a badge/timer-id notification when the timer
  expires.
- Done: added `SYSCALL_TIMER_BIND` and allowed user tasks to create no-callback
  timers, bind them to endpoint caps, start them, and receive expiry
  notifications through `sys_ep_recv()`; user-provided timer callbacks remain
  rejected.
- Done: added kernel and user regression coverage for timer endpoint
  notifications.
- Done: added first IRQ endpoint notification foundation: `irq_bind_endpoint()`
  records an IRQ-to-endpoint badge binding and `irq_notify()` delivers a
  one-way `{badge, irq}` message in task context.
- Done: added kernel regression coverage for IRQ endpoint notification and
  unbound IRQ rejection. Real hardware ISR mask/ack delivery remains a later
  Phase 5 slice.
- Done: added first BH endpoint notification foundation:
  notification-only BH objects can bind an endpoint and deliver a one-way
  `{badge, bh_id}` message when scheduled, while existing kernel callback BHs
  remain supported.
- Done: added kernel regression coverage for BH endpoint notification.
- Done: notification bindings now validate endpoint existence at bind time for
  timer, IRQ, and BH paths; added negative coverage for invalid endpoint binds.
- Done: started Phase 6 memory object foundation by changing `CAP_OBJ_MEMBLOCK`
  from a raw backing pointer to a small memory-object descriptor with recorded
  base and size.
- Done: added `kmem_get_bounds()` and regression coverage for memory cap bounds
  metadata, invalid bounds arguments, and cleanup of both descriptor and backing
  allocation.
- Done: added `kmem_get_range()` for capability-checked bounded pointer
  resolution with offset/length validation; added regression coverage for
  valid ranges, overflow, zero length, and NULL output arguments.
- Done: converted memory allocation syscalls to the memblock descriptor model:
  `sys_mem_alloc()` returns a memory capability, `sys_mem_free()` releases it
  through `kmem_free_cap()`, and `SYSCALL_MEM_SIZE` exposes descriptor size to
  user tasks without mapping the backing memory.
- Done: added user-task regression coverage for memory cap allocation, size
  query, cleanup, and invalid cap rejection.
- Done: added local regression coverage for copying a memory capability from a
  kernel endpoint sender to a user `sys_ep_recv_caps()` service; the service
  validates the received cap through `sys_mem_size()` and replies over the same
  endpoint.
- Done: added first MMIO capability skeleton with `CAP_OBJ_MMIO` and kernel-only
  `kmmio_create_cap()` / `kmmio_get_bounds()` / `kmmio_delete_cap()` APIs.
- Done: MMIO caps now strictly reject Flash, SRAM, heap pointers, invalid
  width/alignment, zero size, and peripheral-window overflow; no user mapping
  syscall is exposed in this slice.
- Done: added local regression coverage for MMIO strict rejection and invalid
  MMIO cap lookup/delete rejection. Valid MMIO cap lifecycle coverage is left
  for an earlier, controlled capability test because the late `mem` module may
  run after the global cap pool is under pressure.
- Done: added controlled `CAP_OBJ_MMIO` lifecycle coverage in the capability
  module: create a valid peripheral-window MMIO cap, read back base/size/width,
  delete it, verify stale lookup fails, and verify metadata allocation is
  cleaned up.
- Done: changed MMIO descriptors from dynamic heap allocations to a small
  static kernel descriptor pool, matching MMIO's fixed hardware-resource
  nature and avoiding heap dependency in capability lifecycle tests.
- Done: hardened capability slot accounting after MMIO testing exposed
  `cap_free_count()==128` while `cap_create_for()` could still fail. Free slots
  with invalid generation are now normalized before allocation, and
  `cap_free_count()` reports only allocatable free slots.
- Done: added first shared-memory object skeleton with `CAP_OBJ_SHM` and
  kernel-only `kshm_create_cap()` / `kshm_get_bounds()` /
  `kshm_get_range()` / `kshm_delete_cap()` APIs.
- Done: added local SHM regression coverage for descriptor/backing allocation,
  bounds metadata, range validation, reduced-rights derived caps, parent revoke
  invalidating children, and heap cleanup.
- Done: changed SHM descriptors to a small static kernel descriptor pool while
  keeping SHM backing memory dynamically allocated; tests now assert only the
  backing allocation affects heap outstanding count.
- Done: added endpoint IPC regression coverage for copying SHM capabilities:
  a client sends a reduced read-only SHM cap to a server, the server validates
  bounds/range and write rejection, the client keeps its writable source cap,
  and root revoke invalidates copied caps while restoring heap usage.
- Done: added `docs/SHM_MAP_DESIGN.md`, defining the first SHM-to-user MPU
  mapping design, including fixed MPU region usage, rights-to-AP mapping,
  alignment rules, per-task mapping metadata, task-exit cleanup, failure codes,
  revocation constraints, and implementation/test order.
- Done: added per-module test resource diagnostics to the test framework:
  module pass/fail deltas, `cap_free_count()` before/after, and
  `mem_get_outstanding_allocs()` before/after are printed after each module so
  cap/heap leaks and order pollution can be traced to the responsible module.
- Done: fixed test-suite heap pollution found by resource diagnostics:
  `ramfs` unlink now frees file private data and buffers, and fault/VFS tests
  unlink temporary `/tmp` files and mount-test directories after use.
- Done: added first kernel-only SHM-to-task MPU mapping path:
  `kshm_create_aligned_cap()` creates MPU-compatible backing,
  `kshm_map_to_task()` installs read-only or read/write mappings into dynamic
  task MPU regions `3..7`, `kshm_unmap_from_task()` clears one mapping, and
  task cleanup calls `kshm_unmap_all_for_task()` before CSpace revocation.
- Done: added local SHM mapping regression coverage for mapping metadata,
  MPU AP/XN bits, duplicate map rejection, explicit unmap, task-delete cleanup,
  and heap outstanding restoration. User-visible SHM map/unmap syscalls remain
  intentionally deferred until live MPU reload and revoke-race semantics are
  finalized.
- Done: exposed user-visible SHM map/unmap for the current user task:
  `SYSCALL_SHM_MAP` / `SYSCALL_SHM_UNMAP` call the kernel mapper, reload the
  active MPU before returning to user mode, and allow a user service with a SHM
  cap to read/write the shared backing memory directly.
- Done: added user syscall coverage for SHM map/unmap: invalid cap rejection,
  unsupported rights rejection, read/write through mapped SHM, explicit unmap,
  double-unmap `KERN_ERR_NOEXIST`, and heap cleanup after root cap deletion.
- Done: added generic capability revoke hooks and wired SHM into them. Before a
  SHM cap slot is invalidated or backing memory is released, SHM now walks all
  tasks, clears matching per-task mapping metadata, disables the MPU region, and
  reloads the current task MPU if needed.
- Done: added kernel regression coverage that root SHM revoke clears a mapped
  child cap from a user task, disables the mapped MPU region, invalidates the
  child cap, and restores heap outstanding count.
- Done: added user-path MPU region exhaustion coverage for SHM maps. A user
  task receives six SHM caps through endpoint cap transfer; the first five
  consume MPU regions `3..7`, and the sixth `sys_shm_map()` returns
  `KERN_ERR_RESOURCE`.
- Done: added conservative `SYSCALL_SHM_CREATE` policy. Kernel/privileged
  callers can create aligned SHM caps with validated rights and clean them up,
  while ordinary user tasks are rejected with `KERN_ERR_PERM` until root/init or
  a dedicated allocator service owns user-visible memory-object creation.
- Done: started Phase 7 root/init bootstrap with a conservative kernel-side
  bootstrap record. `root_bootstrap_prepare()` accepts only user tasks, installs
  an initial `CAP_OBJ_TASK` management cap into root/init's CSpace, records the
  initial cap set, rejects duplicate bootstrap, and clears bootstrap state when
  the root task is deleted.
- Done: added service-model regression coverage for invalid bootstrap inputs,
  privileged compatibility-task rejection, root/init initial cap lookup, duplicate
  prepare rejection, task-delete cleanup, and cap-pool restoration.
- Done: added `root_bootstrap_create()` so root/init user-task creation and
  initial authority installation are one rollback-safe kernel operation instead
  of scattered `task_create_user()` plus manual bootstrap calls. Tests now cover
  NULL entry rejection, duplicate root creation rejection, returned root task id,
  and cleanup without cap leakage.
- Done: root/init bootstrap now creates and records an initial endpoint cap for
  root/init. The endpoint is installed in root/init's CSpace with `CAP_FULL`,
  `root_bootstrap_info_t` records its endpoint id, and task cleanup deletes the
  endpoint without relying on a global endpoint-cap cleanup hook.
- Done: added explicit `root_bootstrap_start()` for starting the prepared
  root/init task without automatically taking over the existing compatibility
  boot path. Tests cover missing-root start rejection, not-started/started
  state reporting, successful start, duplicate start rejection, and cleanup
  after deleting a started root task.
- Done: aligned root/init initial task cap with the syscall task-cap ABI by
  storing `task_id + 1` as the cap object, matching `sys_task_start/delete`.
- Done: added `root_bootstrap_create_service()` as the first service creation
  foundation. It requires an active root/init, creates a user service task, and
  installs a full task-management cap for that service into root/init's CSpace.
  Tests cover missing-root rejection, NULL service entry rejection, returned
  service task id/cap, root-side cap lookup, and cleanup without cap leakage.
- Done: added `root_bootstrap_start_service()` so root/init can start a service
  through the service task cap it owns, not through a raw task id. Tests cover
  missing-root rejection, invalid-cap rejection, root self-cap rejection,
  successful service start, duplicate start rejection, ready-state observation,
  and cleanup without cap leakage.
- Done: added `root_bootstrap_create_service_endpoint()` as the first service
  endpoint bootstrap path. Root validates a service task cap, creates a service
  endpoint, receives a `CAP_FULL` endpoint cap, and the service receives a
  reduced `CAP_READ | CAP_WRITE` endpoint cap. Bootstrap tracks service
  endpoints and deletes them when the service task or root task is cleaned up.
- Done: added first root-created user-service IPC smoke test. Bootstrap patches
  the not-yet-started service's initial R0 with its endpoint cap, root starts the
  service through the service task cap, the kernel test sends one request over
  the service endpoint, the user service receives through `sys_ep_recv()` and
  replies through `sys_ep_reply()`, and the test joins the service and verifies
  cleanup.
