# My-RTOS P0 Stability Optimization Plan

Scope: STM32F767 mainline only. Do not change the default `make` workflow, board target, or configuration files as part of this plan.

## Goal

P0 is about making the current kernel stable enough to support later microkernel work. The priority is not adding features. The priority is removing race windows, undefined task lifecycle behavior, weak syscall boundaries, and poor fault evidence.

## P0-1: Task Lifecycle and Scheduler Consistency

Status: complete for current P0 scope; hardware test-suite-to-shell runs passed on STM32F767.

### Current Risks

- `task_used_bitmap` can become visible before a TCB is fully initialized.
- TCBs can be cleared while another path is scanning task IDs.
- Some task APIs update TCB state, ready lists, and bitmap under different locking assumptions.
- `task_get_tcb()` reads TCB state before proving the slot is currently owned.
- Reclaim, delete, join, and fault termination do not yet share one cleanup path.

### Required Changes

1. Define a strict TCB ownership rule:
   - A task slot is externally visible only when `task_used_bitmap` has the bit set.
   - The bit must be set only after the TCB and initial stack frame are valid.
   - The bit must be cleared before the TCB is wiped.
2. Protect create/delete/reclaim state transitions with critical sections.
3. Make task scanning use bitmap snapshots instead of live bitmap iteration.
4. Avoid clearing TCB fields that a joiner still needs before retval is retained.
5. Keep ready queue operations internally consistent:
   - no duplicate insertion
   - no removal unless node belongs to the expected list
   - no stale cleared TCB in ready queues
6. Add lightweight debug checks for state, stack metadata, and queue membership.

### Acceptance

- Test suite repeatedly reaches shell without random HardFault.
- `ps` stack values remain sane after tests.
- Creating/deleting user tasks while SysTick runs does not fault.
- `task_join()` still receives retval after terminated task reclaim.

### Progress

- Done: publish `task_used_bitmap` only after the TCB and initial stack frame are valid.
- Done: clear bitmap visibility before wiping a TCB.
- Done: make `task_get_tcb()` validate bitmap ownership before reading TCB state.
- Done: make SysTick timeout scanning use a bitmap snapshot.
- Done: route task exit and user fault termination through a shared task cleanup helper.
- Done: safely unlink blocked tasks from IPC wait queues before delete/suspend/fault cleanup.
- Done: make shared `wait_queue_remove()` ignore non-members instead of corrupting queue links on double-remove races.
- Done: remove raw task pool scans from kernel task iteration and stats update paths.
- Done: add safe wait-queue unlink support and task cleanup hooks for sem/mutex/mqueue/event/endpoint/channel blocked tasks.
- Done: add a regression test for deleting a task blocked on an IPC wait queue.
- Done: add ready/wait queue invariant diagnostics for debug builds.
- Done: flash and repeated test-suite-to-shell runs on STM32F767 passed without scheduler switch faults.

## P0-2: Syscall and SVC Boundary

Status: complete for current P0 scope.

### Current Risks

- SVC context and task context have different exit semantics.
- Blocking syscall paths need a clear return model.
- User task return must always exit through SVC, never directly through a privileged helper.
- User pointers are still used as kernel pointers in several syscall paths.

### Required Changes

1. Keep `task_exit_request()` as the non-blocking SVC-safe exit request.
2. Keep `task_exit()` as the noreturn task-mode wrapper.
3. Define one syscall trap-frame layout and document argument offsets.
4. Add user pointer checks before syscall handlers dereference user buffers.
5. Ensure syscall handlers return error codes instead of causing kernel faults.

### Acceptance

- A user task that returns from its entry function is reclaimed normally.
- A user task calling `SYSCALL_TASK_EXIT` does not hang in SVC.
- Bad user pointers return an error or user fault; they do not panic the kernel.

### Progress

- Done: keep `task_exit_request()` as the SVC-safe path and `task_exit()` as the noreturn task-mode wrapper.
- Done: copy syscall task/endpoint/timer/VFS path strings into bounded kernel buffers before use.
- Done: reject obvious bad syscall buffer pointers before mqueue/endpoint/channel/VFS read/write paths dereference them.
- Done: add regression coverage for bad syscall string pointers returning `KERN_ERR_PARAM`.
- Done: replace broad user SRAM range checks with current-task MPU-region-aware syscall buffer validation.
- Done: document the SVC trap-frame and a4-a6 stack offsets next to the assembly handler.

## P0-3: MPU and User Fault Containment

Status: complete for current P0 scope; explicit shared/user data regions are deferred to later microkernel work.

### Current Risks

- User SRAM mapping is still too broad for a real microkernel boundary.
- Fault containment depends on correct EXC_RETURN and CONTROL interpretation.
- Fault cleanup does not yet remove the task from every wait queue/resource owner.

### Required Changes

1. Keep EXC_RETURN-based Thread/Handler detection.
2. Restrict user task MPU mappings in stages:
   - current stack
   - allowed user data
   - explicit shared buffers
3. Add `task_cleanup_resources(tcb)` and call it from exit/delete/fault paths.
4. Ensure kernel faults always panic with a complete crash dump.

### Acceptance

- User task fault terminates only that user task.
- Kernel fault prints PC/LR/CFSR/HFSR/MMFAR/BFAR and current task id.
- User task cannot write TCBs or scheduler data once MPU tightening is enabled.

### Progress

- Done: keep EXC_RETURN-based Thread/Handler detection for fault containment.
- Done: route task exit, delete, and user fault termination through shared task cleanup/resource revocation.
- Done: keep blocked-task unlink cleanup for sem/mutex/mqueue/event/endpoint/channel waits.
- Done: remove the broad user RW SRAM mapping from user task MPU setup.
- Done: keep user Flash RO+execute and current task stack RW as the default user address space.
- Done: align static task stacks to MPU region requirements and configure stack regions from the effective TCB stack size.
- Done: make syscall user buffer validation follow the current user task MPU regions, including stack guard subregions.
- Deferred: explicit shared buffers/user data regions beyond the current stack-only user SRAM window.

## P0-4: Fault Evidence and Debuggability

Status: complete for current P0 scope.

### Current Risks

- HardFault output can be incomplete on UART.
- PC alone is useful but insufficient for imprecise bus faults.
- No automatic recent trace is printed during panic.

### Required Changes

1. Keep panic output interrupt-disabled.
2. Print R0-R3/R12/LR/PC/xPSR/CFSR/HFSR/MMFAR/BFAR/MSP/PSP/CONTROL/EXC_RETURN.
3. Add optional panic-time trace dump:
   - recent task switches
   - recent syscall ids
   - recent IRQ/fault events
4. Add an addr2line workflow note to docs.

### Acceptance

- Any remaining panic can be mapped to a function and line from UART output.
- Crash output does not interleave with shell prompt or SysTick output.

### Progress

- Done: kernel fault path disables interrupts before UART fault output and panic.
- Done: crash output includes R0-R3/R12/LR/PC/xPSR/CFSR/HFSR/MMFAR/BFAR/MSP/PSP/CONTROL/EXC_RETURN and task id.
- Done: panic-time fault output dumps the recent trace tail when tracing is enabled.
- Done: diagnostic guide includes an `addr2line` workflow for PC/LR mapping.

## Execution Order

1. Close TCB visibility races in task create/get/delete/reclaim.
2. Add scheduler/task invariants where low risk.
3. Stabilize SVC/task exit semantics.
4. Improve crash dump reliability.
5. Start usercopy and MPU tightening only after task lifecycle stops faulting.
