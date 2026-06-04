# My-RTOS P4 Microkernel Refactor Plan

Status: Phase 10 FS server foundation and shell-managed service lifecycle are
board-validated. Phases 0-10 core slices have board test coverage; Phase 11
supervisor work has started as a shell-managed health/restart/recover path and
still needs a reusable root/init-owned supervisor service.

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

Status: Design starting. See `docs/NAME_SERVER_DESIGN.md`.

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
- Done: completed Phase 7 core acceptance coverage. Board tests now cover
  controlled root/init creation, initial task and endpoint caps, cap-based
  service task creation/start, service endpoint creation with reduced service
  rights, endpoint lifetime cleanup, and one root-created user service IPC round
  trip using a full `KERN_EP_MSG_SIZE` receive buffer.
- Remaining Phase 7 limits: the default boot still uses the compatibility path,
  there is no production `src/user/init/init.c` policy loop yet, root/init crash
  behavior is still test-scoped, and service endpoint delivery still uses the
  initial-R0 bootstrap helper until the user runtime/name-server handoff exists.
- Done: started Phase 8 name-server implementation with a stable
  `src/user/nameserver/nameserver.h` IPC ABI and service-model coverage for
  root-created user name-server `PING`.
- Done: added name-server `REGISTER` coverage over real user IPC cap transfer:
  a user client transfers a service endpoint cap with `CAP_TRANSFER`, the
  user-space name-server receives it through `sys_ep_recv_caps()`, records it,
  replies through endpoint IPC, and all cap resources are restored.
- Done: fixed a sleepable endpoint send/cap-transfer race found by the
  name-server tests. Syscall send now establishes the client reply-wait state
  before waking a waiting server, and direct delivery failure rolls back the
  client syscall state instead of falsely waking the receiver.
- Done: added name-server `LOOKUP` happy-path coverage. A second user client
  looks up the registered service name, provides an inbox endpoint cap, receives
  a derived service endpoint cap from the name-server over cap-bearing IPC, and
  verifies the returned cap is usable.
- Done: extracted the test-proven name-server logic into
  `src/user/nameserver/nameserver.c` as `nameserver_service_run()`, then updated
  service-model tests to call the real user-service implementation for `PING`,
  `REGISTER`, `LOOKUP`, missing lookup, and duplicate register.
- Done: added name-server `UNREG` coverage. A registered service can be removed
  through endpoint IPC, subsequent lookup returns `KERN_ERR_NOEXIST`, and
  service/root cleanup restores the cap pool.
- Done: added protocol error coverage for the user-space name-server. Bad magic,
  empty names, register-without-cap, and unregister-missing-service all return
  explicit errors, and the service continues to process a following `PING`.
- Done: tightened name-server cap lifecycle. The service now revokes unused
  transferred caps on error paths, revokes lookup inbox caps after use, revokes
  registered service caps on `UNREG`, and drains registered caps on exit. Tests
  now exercise repeated register/unregister cycles to catch long-lived CSpace
  leaks.
- Done: tightened name-server name validation. Named operations now require a
  non-empty, NUL-terminated name within `NS_NAME_MAX`, release any transferred
  caps on malformed requests, and tests cover unterminated-name rejection.
- Done: expanded per-task CSpace from the legacy 16-slot bitmap to 32 slots.
  This makes a 16-entry name-server registry viable for a long-lived user
  service that also holds its own endpoint caps. Capability tests now verify
  slots beyond 15, and service-model tests exercise registry-full behavior.
- Done: added first name-server ownership policy. `REGISTER` records an
  `owner_badge`, and `UNREG` now rejects mismatched owner badges with
  `KERN_ERR_PERM`. Service-model coverage verifies that a non-owner cannot
  unregister another service while the original owner still can.
- Done: closed stale endpoint-cap lifetime gap. Capability now has
  `cap_revoke_object()`, and `endpoint_delete()` revokes all caps that point to
  the deleted endpoint object before freeing the endpoint id. Tests verify root
  and service endpoint caps no longer resolve after endpoint cleanup, and the
  capability module directly covers object-wide revoke behavior.
- Done: added reusable user-side name-server client helpers for ping, register,
  unregister, and two-phase lookup. Service-model clients now use the helpers
  for the main happy paths while lower-level protocol tests still exercise raw
  messages and malformed requests.
- Done: added name-server helper validation coverage. The helper API now has
  regression tests for invalid caps, NULL/empty/unterminated names, NULL lookup
  output storage, and lookup output clearing before local validation failures.
- Done: started Phase 9 driver server foundation with
  `src/user/drivers/driver_proto.h` and `src/user/drivers/uart_server.c`.
  Driver tests now create a root-managed user UART server, send `PING` and
  `WRITE` requests over endpoint IPC, verify deterministic replies, and keep the
  existing kernel/debug UART path intact.
- Done: added first driver client helpers and protocol-error coverage.
  `driver_ping()` and `driver_write()` now centralize the user-side ABI shape,
  and driver tests verify local helper validation plus UART-server replies for
  bad magic, unknown opcode, unsupported read, and oversized write requests.
- Done: connected the UART driver-server prototype to service discovery.
  Driver tests now create a user name-server, register `dev.uart0` with a
  transferred endpoint cap, perform lookup from a second user client, and use
  the returned cap through `driver_ping()`. This validates the intended path:
  root-created driver service -> name-server registration -> client lookup ->
  driver IPC.
- Done: added initial driver session semantics. The UART server now supports
  `OPEN` and `CLOSE`, rejects duplicate open / close-before-open, and requires
  an open session before `WRITE`. Client helpers now expose `driver_open()` and
  `driver_close()`, and driver tests cover the ping -> open -> write -> close
  path plus close-before-open, write-before-open, and duplicate-open rejection.
- Done: added the first driver event ABI. `DRV_EVENT_*` bits and
  `driver_poll()` now let a user client query service state without depending
  on kernel `DEVICE_EVENT_*` definitions. The UART server reports writable
  after `OPEN`, and tests cover helper validation plus poll replies.
- Done: added a minimal driver `IOCTL` control path. `drv_msg_t` now carries an
  explicit command field, `driver_ioctl()` validates and clears caller output,
  and the UART server implements `DRV_IOCTL_GET_EVENTS` with unknown-command
  rejection. Driver tests cover both normal get-events replies and invalid
  IOCTL handling.
- Done: added the first driver `READ` path. `driver_read()` validates and
  clears caller buffers, copies returned payload bytes with overflow checks, and
  the UART server now supports a nonblocking empty read after `OPEN`. Tests
  cover helper validation, read-before-open rejection, oversized read rejection,
  and the open -> empty-read path.
- Done: upgraded the service-discovery driver client test from a health check
  to a full helper sequence. After resolving `dev.uart0` through the
  name-server, the user client now runs `PING`, `OPEN`, `POLL`,
  `IOCTL(GET_EVENTS)`, `READ`, `WRITE`, and `CLOSE` through the transferred
  endpoint cap.
- Done: added the first resource-cap attach path for user-space drivers.
  `DRV_OP_ATTACH` and `driver_attach_cap()` transfer one resource capability to
  the UART server using endpoint cap passing; the server validates receipt and
  revokes its received copy. Driver tests create an MMIO cap, copy it to a user
  client, send it to the server, and verify cleanup restores cap accounting.
- Done: added attach negative coverage. The UART server now has a regression
  test for `DRV_OP_ATTACH` without an attached cap and must return
  `KERN_ERR_CAP`, proving plain messages cannot impersonate resource grants.
- Done: added attach transfer-right coverage. A user client with only a
  read-only MMIO cap now attempts `driver_attach_cap()` and must fail before the
  server receives a request, while the server exits by receive timeout. This
  pins the rule that resource attach requires `CAP_TRANSFER`.
- Done: typed driver resource attach. `DRV_RESOURCE_MMIO` and
  `DRV_RESOURCE_IRQ` now define the resource class carried by `DRV_OP_ATTACH`;
  the default helper sends MMIO, while `driver_attach_resource()` can specify
  the type explicitly. The UART server rejects unknown resource types after
  releasing the received cap, and tests cover local helper validation plus a
  raw bad-type attach request.
- Done: recorded attached driver resources in the user-space UART server.
  Successful `DRV_OP_ATTACH` now updates MMIO/IRQ resource state, duplicate
  attaches are rejected as busy, and `DRV_IOCTL_GET_RESOURCES` returns a stable
  resource bitmask for diagnostics and later probe/remove policy.
- Done: pinned duplicate driver resource attach behavior with a regression
  test. Re-sending the same MMIO resource class to a running UART server now
  must return `KERN_ERR_BUSY` while still releasing the transferred cap copy and
  preserving cap accounting.
- Done: covered the IRQ resource attach protocol path. The UART driver server
  now has board-test coverage for `DRV_RESOURCE_IRQ` attach and verifies that
  `DRV_IOCTL_GET_RESOURCES` reports the IRQ bit independently from the MMIO bit.
- Done: started enforcing resource-dependent driver policy. Legacy no-resource
  health tests can still open the prototype server, but once a server has
  entered resource-managed mode with an IRQ resource, `OPEN` is rejected until
  an MMIO resource is also attached.
- Done: covered the positive resource-managed driver path. A UART server that
  receives an MMIO resource cap now accepts `OPEN`, handles `WRITE`, and
  closes cleanly, proving that attached resources are part of the usable driver
  session path rather than only diagnostics.
- Done: covered resource-managed event and resource lifetime. After MMIO attach
  and `OPEN`, `POLL` reports writable; after `CLOSE`, `POLL` clears writable
  while `DRV_IOCTL_GET_RESOURCES` still reports the MMIO resource owned by the
  server.
- Done: covered the full user-space resource-managed driver session. A user
  client now receives endpoint and MMIO caps, calls `driver_attach_cap()`,
  `driver_open()`, `driver_poll()`, `driver_write()`, and `driver_close()`
  through sleepable syscalls, and the server completes the session with clean
  cap accounting.
- Done: covered the user-space negative resource policy. A user client that
  attaches only an IRQ resource now reaches the driver server through the normal
  helper path, but `driver_open()` is rejected with `KERN_ERR_CAP` until an MMIO
  resource is available.
- Done: added a typed `driver_get_resources()` user helper. The full
  user-space resource-managed session now queries the server's resource bitmask
  after `driver_attach_cap()` and verifies the MMIO bit before opening.
- Done: added a typed `driver_get_events()` user helper. The full user-space
  resource-managed session now verifies writable state through the ioctl event
  helper, while the lower-level `driver_poll()` path remains separately tested.
- Done: connected IRQ endpoint notification to the UART driver-server event
  model. `irq_notify()` can now wake the UART server through a driver-specific
  badge, and the server reports the interrupt as `DRV_EVENT_READABLE` through
  the normal event query path.
- Done: added IRQ-readable consumption semantics to the UART driver server.
  A read after an IRQ notification now consumes one pending readable event and
  subsequent event queries clear `DRV_EVENT_READABLE`.
- Done: added user-client coverage for IRQ readable consumption. A user task
  now observes the IRQ-driven readable event through `driver_get_events()`,
  consumes it with `driver_read()`, and verifies the event bit clears through
  sleepable driver helpers.
- Done: added the first real IRQ capability object. `kirq_create_cap()`,
  `kirq_get_number()`, and `kirq_delete_cap()` now back `CAP_OBJ_IRQ` with a
  static descriptor pool, and UART driver IRQ-resource tests now transfer real
  IRQ caps instead of MMIO stand-ins.
- Done: connected IRQ caps to endpoint notification binding. `kirq_bind_endpoint()`
  now binds an IRQ cap to an endpoint using `CAP_WRITE`, and final IRQ cap
  cleanup clears the corresponding notification binding.
- Done: switched UART driver IRQ-notification tests to the cap-binding path.
  Driver IRQ events now flow through `kirq_create_cap()` and
  `kirq_bind_endpoint()` before `irq_notify()` wakes the UART server.
- Done: tightened IRQ cap revoke semantics. Endpoint notification bindings now
  remember the cap used to bind them, and the IRQ cap revoke hook clears the
  binding even if other derived caps still reference the same IRQ object.
- Done: tightened driver resource attach validation. User-space UART server now
  uses `sys_cap_type()` to verify that `DRV_RESOURCE_MMIO` carries a real
  `CAP_OBJ_MMIO` and `DRV_RESOURCE_IRQ` carries a real `CAP_OBJ_IRQ`; mismatched
  resource messages are rejected with `KERN_ERR_CAP`.
- Done: changed UART driver resource attach from symbolic state to real service
  ownership. Successful attach now keeps the copied resource cap in the driver
  server until the service exits, then revokes it; regression coverage checks
  MMIO and IRQ object refcounts while the server is alive and after cleanup.
- Done: tightened driver resource rights. `driver_attach_resource()` now copies
  `CAP_READ | CAP_WRITE` resource caps, and the UART server rejects attached
  MMIO/IRQ caps that do not carry the required rights even if their object type
  is correct.
- Done: added user-facing IRQ endpoint binding through `sys_irq_bind()`.
  User tasks can bind an IRQ cap to an endpoint cap without raw IRQ IDs, and
  UART driver-server IRQ tests now pass an IRQ resource cap to the server and
  let the user-space server bind its own notification endpoint. Negative
  coverage verifies read-only IRQ caps and read-only endpoint caps are rejected.
- Done: added a driver status ioctl. `DRV_IOCTL_GET_STATUS` reports open,
  MMIO-ready, IRQ-bound, pending-IRQ, and sticky-error bits separately from the
  resource inventory bitmask, giving shell/diagnostic code a stable view of
  user-space driver state.
- Done: added recoverable driver diagnostics. `DRV_IOCTL_CLEAR_STATUS` clears
  sticky-error and pending-IRQ diagnostic state without dropping open/resource
  ownership, so supervisor code can acknowledge transient faults explicitly.
- Done: added explicit driver resource detach. `DRV_OP_DETACH` lets a
  user-space driver release an attached MMIO or IRQ resource cap while closed,
  updates the resource inventory, and relies on cap revoke hooks to tear down
  IRQ endpoint bindings.
- Done: pinned active-session detach policy. Driver tests now verify resource
  detach is rejected while the UART server is open and that the existing
  attached resource remains usable for subsequent I/O.
- Done: covered IRQ resource detach end-to-end. Driver tests now verify IRQ
  detach releases the server-held IRQ cap, clears the resource bit, and removes
  the endpoint notification binding through the IRQ cap revoke hook.
- Done: covered MMIO reattach after detach. Driver tests now verify a long-lived
  UART server can release a resource, accept a fresh transferred MMIO cap, report
  the resource bit again, and release the reattached cap on service exit.
- Done: covered IRQ reattach after detach. Driver tests now verify a detached
  IRQ resource can be transferred back into the same running UART server,
  re-establish endpoint notification binding, and still clean up its cap on
  service exit.
- Done: covered user-client resource detach. The user resource-session test now
  drives `driver_detach_resource()` through the normal helper/SVC/IPC path,
  verifies the MMIO resource bit clears, and checks the server-held MMIO cap is
  released before the service exits.
- Done: covered user-client IRQ detach. The IRQ-only user-client test now
  detaches the IRQ resource through the helper path, verifies the resource bit
  clears, and checks the IRQ endpoint binding is gone before service exit.
- Done: covered user-client active detach rejection. The user resource-session
  test now attempts `driver_detach_resource()` while the UART server is open,
  verifies `KERN_ERR_BUSY`, and then proves the session can still perform I/O.
- Done: covered duplicate detach rejection. The MMIO detach regression now
  verifies a second detach after the resource is already gone returns
  `KERN_ERR_STATE`, while later reattach still succeeds.
- Done: covered duplicate IRQ detach rejection. The IRQ detach regression now
  verifies a second detach after binding cleanup returns `KERN_ERR_STATE`, while
  later reattach still restores notification delivery.
- Done: added a shell-visible user-driver ABI entry point. The `driver` command
  reports the user-space driver protocol, ioctl/resource/status bit names, and
  explicitly notes that the debug UART compatibility path remains active.
- Done: split the shell driver command into stable subcommands. `driver abi`
  keeps the protocol view, while `driver status` reports the runtime
  name-server/inbox binding state instead of implying a hidden static service
  session.
- Done: added a static user-driver registry foundation. `driver_registry`
  records the `dev.uart0` service descriptor, supported protocol operations,
  required MMIO/IRQ resources, and status bits; shell `driver status` now shows
  registered user-driver services independently from the live lookup state.
- Done: expanded shell driver registry diagnostics. `driver status` now prints
  per-service operation, resource, and status-bit descriptors from the registry,
  making the shell view useful even before a live name-server binding exists.
- Done: added service-filtered shell driver diagnostics. `driver status
  <service>` now looks up a single registry descriptor, while unknown services
  return an explicit not-found line.
- Done: added registry descriptor validation. Driver registry code now validates
  descriptor names, known operation/resource/status bit masks, and duplicate
  service names; tests cover both the valid `dev.uart0` entry and malformed
  descriptor rejection.
- Done: exposed registry validation in shell diagnostics. `driver status` now
  reports whether the full registry validates, and filtered service status
  reports whether the selected descriptor is valid.
- Done: split driver registry resource descriptors into required and optional
  sets. `dev.uart0` now records MMIO as required and IRQ as optional, shell
  diagnostics print both sets, and validation rejects out-of-set or overlapping
  resource declarations.
- Done: added ioctl capability descriptors to the driver registry. `dev.uart0`
  now records supported event/resource/status/clear-status ioctls, shell
  diagnostics print them, and validation rejects unknown ioctl bits or ioctl
  declarations without the ioctl operation.
- Done: linked registry ioctl descriptors to protocol command IDs. Registry
  helpers now convert supported ioctl bits to `DRV_IOCTL_*` commands and back,
  with tests covering valid mappings and invalid/null arguments.
- Done: linked registry operation descriptors to protocol opcodes. Registry
  helpers now convert supported operation bits to `DRV_OP_*` opcodes and back,
  with tests covering ping/write/attach/detach mappings plus invalid/null
  arguments.
- Done: linked registry resource descriptors to protocol resource types.
  Registry helpers now convert MMIO/IRQ resource bits to `DRV_RESOURCE_*` wire
  values and back, with tests covering valid mappings and invalid/null
  arguments.
- Done: centralized user-driver status-bit names in the registry. Shell driver
  diagnostics now use registry-provided names for open/MMIO/IRQ/pending/error
  state instead of carrying a separate status-name table.
- Done: centralized user-driver operation, ioctl, and resource names in the
  registry. Shell driver diagnostics now use registry-provided names for
  operation/ioctl/resource bitsets, keeping the user-driver metadata in one
  module as the framework grows beyond UART.
- Done: added capability-based driver registry lookup. Callers can now find a
  driver descriptor by required operation, ioctl, and resource bitsets instead
  of open-coding descriptor iteration, which prepares the next live service
  lookup/client selection step.
- Done: split driver capability matching from registry iteration.
  `driver_descriptor_supports()` now owns the operation/ioctl/resource subset
  check used by capability-based lookup, so future driver clients and shell
  paths can reuse the same matching semantics.
- Done: added diagnostic driver registry capability queries.
  `driver_registry_query_by_caps()` now returns `KERN_OK`, `KERN_ERR_NOEXIST`,
  or `KERN_ERR_PARAM` while clearing output on misses, giving future live lookup
  paths a reasoned API instead of a nullable pointer only.
- Done: added diagnostic driver registry name queries. `driver_registry_query()`
  now returns explicit status for service-name lookup while preserving the
  nullable `driver_registry_find()` compatibility helper.
- Done: added a user-driver discovery helper. `driver_lookup_service()` now
  validates the static driver descriptor, checks required operation/ioctl/resource
  capabilities, and then performs name-server lookup; the UART name-server test
  now exercises this driver-level lookup path instead of calling the name-server
  helper directly.
- Done: added a user-driver discovery release helper. `driver_release_service()`
  now acknowledges the name-server lookup inbox through a driver-level API;
  copied service endpoint caps remain owned by the client CSpace and are cleaned
  up by normal task/cap lifecycle.
- Done: added a UART-specific driver discovery helper. `driver_lookup_uart()`
  wraps the `dev.uart0` service name and baseline UART operation/ioctl/resource
  requirements so clients do not need to open-code the capability set.
- Done: added driver client error names. `driver_error_name()` maps common
  kernel/driver discovery return codes to stable strings so shell/live lookup
  diagnostics can report readable failure reasons.
- Done: wired driver error names into shell registry diagnostics. Filtered
  `driver status <service>` now uses `driver_registry_query()` and prints a
  readable failure reason when the requested service descriptor is missing.
- Done: added explicit driver name-server status probing. `driver_name_server_status()`
  reports unbound shell state as `KERN_ERR_STATE` and can ping a real name-server
  cap later; shell `driver status` now prints `service lookup: unbound (state)`
  instead of a hard-coded not-connected sentence.
- Done: added a driver runtime binding layer. `driver_runtime` now owns the
  shell-visible name-server endpoint cap slot, supports clear/bind/status
  operations, and keeps `driver status` routed through one future live-lookup
  entry point instead of hard-coding an invalid cap in the shell.
- Done: added runtime driver lookup helpers. `driver_runtime_lookup_service()`
  and `driver_runtime_lookup_uart()` hide the stored name-server cap from
  clients, clear outputs on unbound lookup attempts, and delegate to the
  existing driver client once a live name-server endpoint is bound.
- Done: exposed the driver runtime binding slot in shell diagnostics. `driver
  status` now reports the current name-server cap as `none` or an id, making
  future live binding visible without triggering an IPC lookup from the shell.
- Done: added a shell-side driver lookup diagnostic. `driver lookup <service>`
  validates the static descriptor and reports whether the runtime name-server
  binding is ready, giving the future live lookup path a stable user-visible
  command without inventing a temporary shell IPC inbox.
- Done: made driver runtime binding state explicit.
  `driver_runtime_name_server_bound()` centralizes the "has a name-server cap"
  check so shell diagnostics and runtime lookup helpers no longer open-code
  `cap <= 0` as binding policy.
- Done: added a controlled shell binding command for driver discovery.
  `driver bind-ns <cap|clear>` updates the runtime name-server endpoint slot
  without inventing a service or guessing bootstrap caps, making future
  bootstrap-provided cap handoff testable from the shell.
- Done: split driver runtime name-server state into unbound, bound, and live.
  Shell diagnostics now report a saved but invalid cap as `bound (<err>)`
  instead of conflating it with the no-cap `unbound` state.
- Done: consolidated shell driver name-server diagnostics. `driver status`,
  `driver lookup`, and `driver bind-ns` now share one status/cap rendering
  helper so future live lookup changes do not fork the shell output semantics.
- Done: added an explicit driver runtime inbox binding slot. Runtime lookup now
  requires both a name-server endpoint cap and an inbox endpoint cap, while shell
  diagnostics expose `driver bind-inbox <cap|clear>` without creating endpoint
  resources implicitly.
- Done: added a runtime lookup readiness check. `driver_runtime_lookup_ready()`
  reports whether both live name-server and inbox prerequisites are satisfied,
  and `driver lookup` now prints the concrete blocker before a real lookup is
  attempted.
- Done: tracked driver runtime inbox ownership. Manual shell bindings are now
  reported as external, while future auto-created inbox caps can be marked owned
  so cleanup code does not revoke user-supplied caps by mistake.
- Done: added shell-owned driver inbox creation. `driver bind-inbox auto` now
  creates a real endpoint cap for shell/runtime lookup use, marks it owned, and
  `driver bind-inbox clear` deletes only owned inbox endpoints while leaving
  external/manual caps untouched.
- Done: added a shell-managed user name-server prototype. `driver ns-start`
  creates a user-space name-server task plus endpoint caps and binds the shell
  side cap into driver runtime; `driver ns-stop` tears it down and clears the
  runtime binding. Infinite name-server mode now survives idle receive timeouts.
- Done: made shell driver service probes schedulable. Driver status/lookup now
  use a short bounded timeout instead of zero-timeout ping, allowing a freshly
  started user name-server task to run to its receive point before diagnostics
  classify it as unavailable.
- Done: added name-server task state to shell driver diagnostics. `driver
  status` now reports the managed name-server task id plus scheduler state, and
  `driver ns-start` yields once before probing so the service can reach its
  receive loop.
- Done: added shell-managed UART service registration. `driver uart-start`
  creates a user-space UART server endpoint/task and registers `dev.uart0` with
  the live name-server; `driver uart-stop` unregisters the service and tears the
  server down.
- Done: upgraded shell driver lookup to a real UART service lookup path.
  `driver lookup dev.uart0` now uses the runtime name-server/inbox bindings to
  receive a copied UART service cap, probes it with `driver_ping()`, and
  acknowledges the lookup inbox.
- Done: fixed UART server persistent mode. `uart_server_run(max_requests=0)`
  now treats idle receive timeouts as keepalive waits, matching the name-server
  behavior instead of exiting before a later shell lookup can ping it.
- Done: added live shell driver probing. `driver probe dev.uart0` performs a
  runtime lookup, pings the copied service cap, reads resource/status/poll
  state, and attempts open/close so the shell can validate the service protocol
  path beyond name-server discovery.
- Done: added a live MMIO delegation probe. `driver probe-mmio dev.uart0`
  creates a kernel MMIO cap, transfers it to the user-space UART service,
  validates resource/status/open/write/close behavior, reports byte-count
  write results, detaches the resource, and deletes the local cap so the shell
  can exercise driver resource handoff without leaving persistent state behind.
- Done: tightened shell-managed UART service shutdown. `driver uart-stop` now
  unregisters `dev.uart0` from the live name-server before deleting the service
  task/endpoint and reports the unregister result, keeping registry teardown
  ordered before endpoint cap revocation.
- Done: started Phase 10 FS server foundation. Added `src/user/fs/fs_proto.h`
  and `src/user/fs/fs_server.c` with a compact endpoint ABI for
  `ping/open/close/read/write/lseek/readdir`, service-local fd tokens, and a
  user-task service loop backed by existing VFS syscalls.
- Done: added first FS server service-model coverage. A user FS service task
  and a user client task now exercise `fs_ping()`, `fs_open()`, `fs_write()`,
  `fs_lseek()`, `fs_read()`, `fs_readdir()`, and `fs_close()` over endpoint IPC
  using `/tmp` through the compatibility VFS path.
- Done: added shell-managed FS service diagnostics. The `fs` command now reports
  the FS service ABI, starts/stops a user-space FS server task, and probes the
  live endpoint with `ping/open("/tmp")/readdir/close` without replacing the
  stable kernel `ls/cat` compatibility path.
- Done: connected the shell-managed FS service to name-server discovery.
  `fs ns-start`, `fs bind-inbox auto`, `fs start`, `fs lookup`, and `fs probe`
  can now register and resolve `fs.ramfs` through a user-space name-server,
  matching the driver service discovery pattern while preserving direct FS
  probe fallback when no name-server is active.
- Done: expanded the FS service ABI to cover `unlink`, `mkdir`, and `stat`.
  Kernel VFS/syscall compatibility hooks now expose those operations to the
  user-space FS server, and service-model plus shell probes validate file
  create/write/stat/unlink and directory mkdir/stat/unlink paths.
- Done: added shell FS data-plane commands through the user-space service:
  `fs ls`, `fs cat`, `fs write`, `fs rm`, `fs mkdir`, and `fs stat`. Manual
  board validation covers `/`, `/tmp`, `/dev/null`, file write/readback,
  deletion, directory creation/removal, and stat output.
- Done: added shell-managed FS lifecycle helpers. `fs up` creates an owned
  inbox endpoint, starts a user-space name-server, starts/registers the FS
  service, and reports status; `fs down` unregisters the service, deletes the
  service task/endpoint, stops the name-server, clears the owned inbox, and
  returns all shell-visible FS handles to `none`.
- Done: added first supervisor-style FS recovery commands. `fs restart` performs
  a full down/up cycle, `fs health` performs non-mutating lookup/ping/status
  diagnostics, and `fs recover` cold-starts a missing stack or restarts an
  unhealthy stack. Board validation covers cold recover, healthy recover,
  restart followed by probe, and down followed by recover.
- Done: started extracting FS service runtime state from shell-only code.
  `src/user/fs/fs_runtime.c` now owns the bound name-server cap, inbox cap,
  lookup, and lookup-ack helpers, matching the driver-runtime direction and
  preparing FS lifecycle policy for a later root/init supervisor.
- Done: tightened shell FS lookup diagnostics and temporary cap cleanup.
  `fs status` now reports discovery-path readiness separately from service
  registration, `fs registered` explicitly checks whether `fs.ramfs` is present
  in the name-server, and FS shell commands delete copied lookup caps after
  acknowledging the name-server inbox. The ack/delete sequence is centralized
  in `fs_runtime_release_service()` so future supervisor code can reuse the same
  temporary-cap cleanup path.
- Done: tightened shell driver lookup temporary cap cleanup. Live shell driver
  lookup/probe paths now acknowledge the lookup inbox and delete copied service
  endpoint caps from the shell's CSpace after use, while the user-facing
  `driver_release_service()` helper remains an ACK-only API suitable for user
  tasks. Driver tests now cover repeated lookup/release/delete cycles and assert
  that temporary capability slots are restored.
- Done: added shell-managed driver lifecycle commands. `driver up` creates an
  owned inbox, starts the user-space name-server, starts/registers the UART
  service, and reports the resulting stack state; `driver down`, `restart`,
  `health`, and `recover` mirror the FS supervisor-style commands so driver and
  FS service stacks now share the same manual lifecycle vocabulary.
- Board-validated: `driver health` reports stopped from a cold shell state;
  `driver recover` creates an owned inbox, starts the name-server, starts the
  UART service, and reaches a live lookup path; `driver probe dev.uart0` and
  `driver probe-mmio dev.uart0` succeed; `driver down` unregisters/stops/clears
  the stack; a second `driver recover` brings the stack back up cleanly.
- Done: added `driver registered [service]` to mirror `fs registered`. It
  reports whether the driver discovery path is ready, whether the named service
  is present in the live name-server, and validates registered services with a
  ping before releasing the copied lookup cap.
- Board-validated: `driver registered` reports `name-server: state` before
  recovery, reports `service registered: yes` plus `ping: ok` after `driver
  recover`, reports `service registered: no (noexist)` after `driver uart-stop`,
  and `driver down` clears the service, name-server, and owned inbox state.
- Done: reduced expected wait-queue cleanup noise. `wait_queue_remove_safe()`
  now performs a genuinely silent optional remove, while ordinary
  `wait_queue_remove()` still reports unexpected missing-task removals in debug
  builds. Board validation passed after the change.
- Done: added first shell-managed service health stats for Phase 11. Driver and
  FS status now report restart count, recover count, and last health result,
  giving the manual supervisor path persistent diagnostics without changing the
  kernel ABI.
- Done: raised the STM32F767 service-concurrency headroom for Phase 11. The
  task pool is now 24 tasks and the endpoint pool is now 8 endpoints, allowing
  shell-managed driver and FS service stacks to be online together. `fs up`
  also rolls back a just-created inbox/name-server on partial startup failure,
  and FS/driver `up` refreshes last-health before printing final status.
- Board-validated: `driver recover` followed by `fs recover` now leaves both
  user-space service stacks online at the same time. Driver uses tasks 5/6,
  FS uses tasks 7/8, both discovery paths are live, and both `driver status`
  and `fs status` report `last health: ok`.
- Board-validated: with both service stacks online, `driver probe dev.uart0`,
  `fs probe`, `driver probe-mmio dev.uart0`, and `fs ls /` all succeed. This
  validates concurrent name-server/inbox/service endpoint usage across the live
  driver and FS paths.
- Board-validated: independent teardown and recovery work. `fs down` tears down
  the FS stack while `driver probe dev.uart0` still succeeds; `driver down`
  tears down the driver stack; a later `fs recover`, `fs probe`, and `fs down`
  all succeed without depending on the driver stack.
- Done: `driver down` and `fs down` now reset last-health to `state` after full
  teardown, so stopped stacks no longer report a stale `last health: ok`.
- Board-validated: final `fs status` and `driver status` after teardown report
  `last health: state`, while recover paths still report `last health: ok` once
  the service stack is live.
- Done: started extracting reusable supervisor code. Added
  `src/user/supervisor/supervisor.c` and `.h` with shared restart/recover/health
  bookkeeping, switched shell-managed driver and FS lifecycle diagnostics to use
  it, and added service-model coverage for the reusable supervisor state API.
- Done: extended reusable supervisor state with pending-client bookkeeping.
  Driver and FS status now expose `pending clients`, currently zero for the
  shell-managed path, and service-model tests cover increment/decrement,
  explicit set, init reset, NULL safety, and underflow protection.
- Done: added reusable supervisor service identity and a `svc [status]` shell
  summary. Driver and FS supervisor instances are now named (`dev.uart0` and
  `fs.ramfs`), `svc` reports task state, lookup readiness, restart/recover
  counts, pending clients, and last health for both shell-managed services, and
  service-model tests cover named/unnamed supervisor initialization.
- Done: added a fixed-size supervisor service registry. Driver and FS now obtain
  their supervisor records from registry slots instead of static per-command
  state objects, `svc` iterates the registry, and service-model tests cover
  register/find/iterate/duplicate/full-table behavior with cleanup before shell
  startup.
- Done: moved driver/FS down-path health reset to the start of teardown, so
  intermediate status prints during `driver down` and `fs down` no longer show
  stale `last health: ok` while resources are being stopped.
- Done: added supervisor restart policy metadata. Service records now carry
  `manual`/`auto` policy and `max_restarts`; `svc` prints the policy, defaults
  remain manual with max 0 so behavior is unchanged, and service-model coverage
  verifies defaults, updates, invalid-policy fallback, init reset, and NULL
  safety.
- Done: added `svc policy <service> <manual|auto> [max]` as the first
  supervisor control-plane write path. The command updates registry metadata
  only, does not enable automatic restart behavior yet, reports missing services
  and invalid policies deterministically, and shares policy parsing with
  service-model tests.
- Done: added `svc recover <service>` as the first unified supervisor recovery
  command. It resolves the service through the supervisor registry and dispatches
  to the existing driver or FS recovery path, preserving current manual recovery
  semantics while moving the control entry point under `svc`.
- Done: added `svc restart <service>` to complete the manual supervisor control
  pair. The command resolves services through the same registry path and
  dispatches to the existing driver/FS restart routines, keeping restart counters
  and teardown/startup behavior unchanged.
- Done: added `svc health <service>` so health probing also flows through the
  supervisor registry. It dispatches to existing driver/FS health commands and
  preserves the same health-result bookkeeping while completing the manual
  `svc` control surface: status, health, recover, restart, and policy.
- Done: added `svc fault <service>` as a safe placeholder for later fault
  injection. It resolves services through the supervisor registry and returns
  deterministic `unsupported` for known services and `service not found` for
  missing ones, without killing or restarting service tasks yet.
- Done: added `svc down <service>` so supervisor service teardown is available
  through the unified control path. The command resolves services via the
  registry, dispatches to the existing driver/FS down routines, and preserves the
  original unregister, name-server stop, inbox cleanup, and health-state
  behavior.
- Done: added `svc start <service>` for explicit cold/manual startup through the
  supervisor control surface. It reuses the existing driver/FS `up` paths and
  intentionally does not increment restart or recover counters, keeping
  lifecycle counters reserved for actual recovery/restart operations.
- Done: added `svc stop <service>` as a supervisor-friendly alias for
  `svc down <service>`. Both commands share the same registry dispatch and
  teardown handlers, so stop/down behavior stays identical.
- Done: added `svc probe <service>` as a unified service validation command. It
  resolves the target through the supervisor registry and dispatches to the
  existing driver or FS probe routines, so functional IPC/file-operation checks
  can be run from the same `svc` control surface as health and lifecycle
  commands.
- Done: added `svc supervise` as a manual supervisor tick. It health-checks all
  registered services, honors `manual` versus `auto` restart policy, applies the
  configured restart limit through the reusable supervisor helper, and dispatches
  to the existing restart paths only when a service is unhealthy and eligible.
- Done: added reusable supervisor metadata reset plus `svc reset <service>`.
  Reset preserves the registered service name but clears restart/recover counts,
  pending clients, policy, and max restart limit. The shell command now refreshes
  health from the actual service path during reset, so running services remain
  reported as healthy while stopped services remain `state`.
- Done: upgraded `svc fault <service>` from placeholder to controlled service
  fault injection. Driver and FS fault paths unregister only the target service,
  delete its service task/endpoint, preserve name-server and inbox state, mark
  supervisor health as `fault`, and leave recovery to `svc supervise` or manual
  restart.
- Done: added supervisor fault accounting. Fault injection now increments a
  per-service fault counter, driver/FS status and `svc` status expose it next to
  restart/recover counts, and `svc reset` clears it with the rest of the
  supervisor metadata.
- Done: added `svc stats` as a read-only supervisor snapshot. It summarizes
  registered service count, currently running service tasks, unhealthy services,
  and aggregate restart/recover/fault counters without probing or mutating
  service state.
- Done: extended `svc supervise` to accept an optional service name. The command
  can now run the same health/policy/restart decision for either all registered
  services or one selected service, making targeted recovery tests possible
  after injecting faults into multiple services.
- Done: added reusable supervisor counter clearing plus `svc clear <service>`.
  Clear resets restart/recover/fault counters only, preserving restart policy,
  max restart limit, pending-client count, and current health so auto-restart
  stress tests can keep their policy while zeroing statistics.
- Done: added `svc stress <service> <loops>` for bounded board-side supervisor
  stress testing. It runs controlled fault injection plus targeted supervise for
  1..10 loops, then executes the existing service probe and prints aggregate
  service stats, reusing the same fault/probe/supervise dispatch paths as the
  manual commands.
- Done: documented the board-validated supervisor command matrix in
  `docs/phase4/SUPERVISOR_TEST_MATRIX.md`, including registry entries, command
  semantics, validated serial sequences, error behavior, and current limits.
- Done: added `docs/phase4/COMPLETION_REPORT.md` summarizing the implemented
  service stacks, supervisor control plane, board-validated behaviors, build
  baseline, current limits, and Phase 5 candidates.
