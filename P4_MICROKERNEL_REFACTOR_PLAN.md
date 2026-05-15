# My-RTOS P4 Microkernel Refactor Plan

Status: Phase 0/1/2 first slice in progress.

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
- Pending: board-run validation for the mqueue continuation slice.
