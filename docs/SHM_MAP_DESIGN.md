# SHM Map Design

## 1. Scope

This document defines the first shared-memory mapping design for P4 Phase 6.
The current code has `CAP_OBJ_SHM`, kernel-only bounds/range helpers, IPC cap
transfer tests, a kernel-only SHM-to-task MPU mapping path, and user-visible
map/unmap syscalls for the current task.

Non-goals for the first implementation:

- no dynamic user virtual address allocator
- no remapping at arbitrary user virtual addresses
- no copy-on-write
- no file-backed mappings
- no user-mode MMIO mapping

The first version maps the SHM backing address directly into the target task's
MPU regions.

## 2. Current MPU Layout

User tasks currently use these MPU regions:

| Region | Purpose | State |
| --- | --- | --- |
| 0 | user Flash, read-only executable | fixed |
| 1 | disabled, previously full SRAM | reserved |
| 2 | user stack, read/write with guard subregion | fixed |
| 3-7 | disabled | available for explicit mappings |

The first SHM mapper must allocate only from regions `3..7`. It must not change
region `0`, `1`, or `2`.

## 3. API

Kernel-internal API:

```c
kern_err_t kshm_map_to_task(tcb_t *task, cap_id_t shm_cap,
                            uint8_t rights, void **out_addr);
kern_err_t kshm_unmap_from_task(tcb_t *task, cap_id_t shm_cap);
void       kshm_unmap_all_for_task(tcb_t *task);
```

Later syscall API:

```c
int sys_shm_create(int size, int rights);
int sys_shm_map(int shm_cap, int rights);
int sys_shm_unmap(int shm_cap);
```

`sys_shm_create()` is intentionally conservative in this slice: kernel or
privileged callers may create an aligned SHM cap, but ordinary user tasks get
`KERN_ERR_PERM`. This keeps heap/MPU resource creation policy in the future
root/init or allocator service instead of letting arbitrary user tasks allocate
global memory objects directly.

`sys_shm_map()` returns the mapped address as an integer on success, or a
negative `kern_err_t` on failure.

## 4. Rights

Mapping rights must be a subset of the caller's cap rights:

| Requested rights | Required cap rights | MPU AP |
| --- | --- | --- |
| `CAP_READ` | `CAP_READ` | privileged RW, user RO (`AP_PRW_URO`) |
| `CAP_READ | CAP_WRITE` | `CAP_READ | CAP_WRITE` | full RW (`AP_FULL`) |

Execute permission is never granted. SHM regions always set `XN_ENABLE`.

`CAP_MANAGE`, `CAP_TRANSFER`, and `CAP_GRANT` are not mapping rights. They must
not affect MPU permissions.

## 5. Alignment And Size

Cortex-M MPU regions require power-of-two size and aligned base. The first
implementation has two valid options:

1. Require SHM backing allocations to be MPU-compatible.
2. Round mapping size up and use subregion disable bits when possible.

The recommended first implementation is option 1:

- `kshm_create_cap()` should reject map-capable SHM sizes below 32 bytes.
- backing base must be aligned to the MPU region size
- size must be a power of two
- tests should begin with 256-byte SHM to allow subregion-safe future work

If dynamic heap cannot guarantee this, add `kshm_create_aligned_cap()` and use
`kmalloc_aligned()` for SHM backing.

## 6. Map State

Each task needs per-mapping metadata so cleanup is deterministic:

```c
typedef struct {
    uint8_t  in_use;
    uint8_t  region;
    uint8_t  rights;
    cap_id_t cap;
    void    *addr;
    size_t   size;
} shm_mapping_t;
```

Store this in `tcb_t`, sized to the available SHM MPU regions. First version:

```c
#define TASK_SHM_MAP_MAX 5
shm_mapping_t shm_maps[TASK_SHM_MAP_MAX];
```

The mapping slot and MPU region should have the same lifecycle.

## 7. Failure Codes

| Case | Error |
| --- | --- |
| NULL task / NULL output | `KERN_ERR_PARAM` |
| invalid cap / wrong type | `KERN_ERR_CAP` |
| requested rights not held | `KERN_ERR_CAP` |
| unsupported rights bits | `KERN_ERR_PARAM` |
| already mapped in task | `KERN_ERR_BUSY` |
| no free task mapping slot | `KERN_ERR_RESOURCE` |
| no free MPU region | `KERN_ERR_RESOURCE` |
| backing not MPU-alignable | `KERN_ERR_PARAM` |
| unmap cap not mapped | `KERN_ERR_NOEXIST` |

## 8. Task Exit Cleanup

Task deletion, normal exit, and fault exit must call:

```c
kshm_unmap_all_for_task(tcb);
```

This must happen before the task's capability set is revoked so the cleanup code
can still identify mapped caps and clear MPU regions. Cleanup must:

- disable each mapped MPU region in `tcb->mpu_regions`
- clear each `shm_mapping_t`
- not revoke the SHM cap by itself; cap cleanup remains capability-layer-owned

## 9. Revocation Semantics

`cap_revoke(parent)` invalidates derived SHM caps. For the first mapper, revoke
does not need to walk all task mappings immediately. Instead:

- task exit always unmaps
- `kshm_unmap_from_task()` must return `KERN_ERR_CAP` if the cap is already
  invalid
- before returning to user mode, stale mapped SHM is acceptable only if there is
  no public revoke syscall that can race with the mapped task

The capability layer exposes object-type revoke hooks. SHM registers a hook
that walks tasks and removes mappings for the cap being cleared before the cap
slot generation changes or backing memory can be released.

## 10. Tests

First implementation tests:

- map read-only SHM into a user task and verify the task can read through a
  syscall/usercopy helper
- map read/write SHM and verify write is accepted
- write request using a read-only cap is rejected
- mapping the same SHM twice returns `KERN_ERR_BUSY`
- exhausting regions returns `KERN_ERR_RESOURCE`
- unmap disables the MPU region and clears task metadata
- task delete/fault clears SHM mappings
- parent revoke invalidates derived caps; before public revoke syscall, verify
  explicit unmap after revoke returns `KERN_ERR_CAP`

## 11. Implementation Status

Done:

- `shm_mapping_t` metadata is stored in `tcb_t`.
- The mapper programs MPU regions `3..7` only.
- `kshm_create_aligned_cap()` provides MPU-compatible SHM backing.
- `kshm_map_to_task()`, `kshm_unmap_from_task()`, and
  `kshm_unmap_all_for_task()` are implemented.
- Task cleanup calls `kshm_unmap_all_for_task()` before capability revocation.
- Kernel tests cover read-only mapping metadata, read/write mapping metadata,
  duplicate map rejection, explicit unmap, task-delete cleanup, and heap
  outstanding restoration.
- `SYSCALL_SHM_MAP` and `SYSCALL_SHM_UNMAP` are exposed through
  `sys_shm_map()` / `sys_shm_unmap()` for the current user task.
- The syscall path reloads MPU regions before returning to user mode, so the
  mapped region is immediately accessible by the caller.
- User syscall tests cover invalid cap rejection, invalid rights rejection,
  read/write through mapped SHM, explicit unmap, double-unmap rejection, and
  heap cleanup after root cap deletion.
- Capability revoke hooks are implemented and SHM uses them to clear mapped
  MPU regions and per-task mapping metadata before a SHM cap is invalidated.
- Kernel tests cover root revoke invalidating a mapped child cap, clearing the
  task mapping, disabling the MPU region, and restoring heap outstanding count.
- User syscall tests cover MPU region exhaustion by receiving six SHM caps over
  endpoint cap transfer: the first five maps consume regions `3..7`, and the
  sixth map returns `KERN_ERR_RESOURCE`.
- `SYSCALL_SHM_CREATE` is exposed with a conservative policy: kernel/privileged
  callers can create and delete aligned SHM caps, while ordinary user tasks are
  rejected with `KERN_ERR_PERM`.

Remaining:

- decide whether a future root/init allocator service should grant SHM creation
  to selected user services through an explicit allocator capability
