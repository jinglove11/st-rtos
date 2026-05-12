# My-RTOS P1 Microkernel Core Plan

Scope: STM32F767 mainline only. P1 builds on the completed P0 stability base. Do not change the default `make` workflow, target board, linker script defaults, or test harness entry unless a specific P1 task explicitly requires it.

## Goal

P1 turns the current "RTOS with user/kernel separation" into a credible microkernel core. The focus is not moving every service to user space yet. The focus is defining the kernel object model, IPC semantics, capability authority, and file/device object lifetime well enough that later services can depend on them.

P1 includes:

- Endpoint and channel IPC semantics
- Capability system redesign
- VFS/file descriptor lifecycle and directory traversal

P1 does not include:

- Full driver framework migration
- Shell feature expansion
- Timer/IRQ/BH service hardening beyond interface dependencies
- Dynamic user process loading
- Multi-board configuration work

## Design Rules

1. Kernel object access from user mode must go through capability lookup.
2. User pointers must be copied or mapped explicitly. IPC/VFS/device paths must not dereference unchecked user pointers.
3. Blocking operations must have deterministic timeout and cancellation cleanup.
4. Object deletion must wake all waiters with a stable error code.
5. Task death must revoke or cancel object state owned by that task.
6. Fast paths can stay static-pool based. Correct ownership matters more than dynamic allocation.
7. Existing tests must keep reaching the shell. New P1 tests should be narrow and regression-oriented.

## P1-1: Capability System Redesign

Status: foundation pass complete; object lifetime and IPC transfer rules remain.

### Current State

Files:

- `src/kernel/cap/capability.c`
- `src/kernel/cap/capability.h`
- `src/kernel/include/kernel_types.h`
- `src/kernel/syscall/syscall.c`

Current capability state is a global random-token table. It checks object type, rights, and owner task id. This is useful as a P0 guard, but it is not yet a microkernel CSpace:

- Cap tokens are global rather than per-task slots.
- Slot reuse is protected only by random token uniqueness, not generation.
- Derived caps are not linked to parent caps.
- Revoke does not cascade to descendants.
- Object lifetime and cap lifetime are loosely coupled.
- IPC transfer changes owner in place instead of defining copy/move/derive semantics.

### Target Model

Each task owns a CSpace: a fixed-size table of capability slots. A cap id is local to the current task and encodes:

- slot index
- generation
- optional flags/badge field if size permits

Each CSpace slot stores:

- object pointer or object id
- object type
- rights
- generation
- parent slot reference
- first child / sibling links or equivalent revoke tracking
- object reference ownership state
- `in_use`

Object-level metadata stores:

- object type
- reference count
- deleted/zombie flag
- optional cleanup callback

### Required Changes

1. Add fixed CSpace storage to `tcb_t`.
2. Replace global token lookup with current-task CSpace lookup for user syscalls.
3. Keep an explicit privileged/internal helper for kernel-created bootstrap caps.
4. Add generation-based stale cap rejection.
5. Track parent/child relationships for derived caps.
6. Implement cascading revoke:
   - revoke target slot
   - revoke all descendants
   - decrement object references
   - wake or fail pending operations if the last authoritative reference disappears
7. Define transfer semantics:
   - `copy`: duplicate cap into target CSpace with same or reduced rights
   - `move`: install into target CSpace and revoke source slot
   - `derive`: create child with subset rights in same CSpace
8. Add object type checking for every syscall cap lookup.
9. Add cap cleanup on task exit/fault/delete.

### Proposed API

```c
typedef uint16_t cap_id_t;

kern_err_t cap_space_init(tcb_t *task);
void       cap_space_destroy(tcb_t *task);

cap_id_t cap_alloc(tcb_t *owner, void *object, uint8_t type, uint8_t rights);
void    *cap_lookup(tcb_t *owner, cap_id_t cap, uint8_t type, uint8_t rights);
kern_err_t cap_delete_local(tcb_t *owner, cap_id_t cap);

cap_id_t cap_derive_local(tcb_t *owner, cap_id_t src, uint8_t rights);
cap_id_t cap_copy_to(tcb_t *src_task, cap_id_t src,
                     tcb_t *dst_task, uint8_t rights);
kern_err_t cap_move_to(tcb_t *src_task, cap_id_t src,
                       tcb_t *dst_task, uint8_t rights,
                       cap_id_t *out_dst);
kern_err_t cap_revoke_tree(tcb_t *owner, cap_id_t root);
```

Compatibility wrappers may keep old names temporarily, but syscall paths should move to the new explicit task-aware API.

### Rights Model

Use the existing rights as a base:

- `CAP_READ`: receive, read, wait
- `CAP_WRITE`: send, write, signal
- `CAP_MANAGE`: delete, configure, bind
- `CAP_TRANSFER`: move/copy cap through IPC
- `CAP_GRANT`: derive reduced caps

Add object-specific interpretation in docs and tests. Do not let raw rights bypass object type checks.

### Acceptance

- Task A cannot use Task B's local cap id.
- Reusing a CSpace slot does not make stale cap ids valid.
- Revoking a parent cap invalidates all derived children.
- Deleting a task revokes all its caps and decrements object references.
- Syscalls fail with `KERN_ERR_CAP` for wrong type or insufficient rights.
- Existing privileged tests can still create initial objects and caps.

### Tests

Add or extend `src/tests/test_capability.c`:

- stale generation rejection
- cross-task cap rejection
- derive subset rights
- reject derive with rights outside parent
- cascading revoke
- task cleanup revokes caps
- wrong object type rejection

### Progress

- Done: replace opaque random tokens with slot+generation cap ids.
- Done: reject stale cap ids after slot reuse.
- Done: add parent/child links for derived caps.
- Done: make parent revoke/delete cascade to derived children.
- Done: enable per-task CSpace storage in `tcb_t` under `CAP_ENABLE`.
- Done: require user-task lookups to pass both owner and local CSpace membership checks.
- Done: install `cap_create_for()` results into the owner task CSpace.
- Done: remove revoked/deleted/transferred caps from the previous owner task CSpace.
- Done: keep compatibility wrappers for existing `cap_create()`, `cap_resolve()`, `cap_delete()`, `cap_derive()`, `cap_transfer()`, and `cap_revoke()` call sites.
- Done: add explicit task-aware helper entry points (`cap_create_for`, `cap_lookup_for`, `cap_derive_for`, `cap_revoke_for`) for later syscall/IPC migration.
- Done: add observable object reference counter (`cap_object_refcount`) and regression coverage for create/derive/copy/revoke cascade.
- Done: add object cleanup callbacks keyed by cap object type and invoke them when the last cap for an object is removed.
- P1 decision: cap ids remain slot+generation global handles guarded by per-task CSpace membership. Fully local per-task numeric handles are an ABI-breaking CSpace redesign and are deferred beyond P1.

## P1-2: Endpoint IPC Call/Reply Semantics

Status: first hardening pass in progress.

### Current State

Files:

- `src/kernel/ipc/endpoint.c`
- `src/kernel/ipc/endpoint.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/user_api.h`

Endpoint currently implements a basic many-client/server queue. It uses:

- endpoint-global `current_sender`
- sender queue slots
- one client reply buffer pointer per task
- a shared reply wait queue

This is not enough for microkernel IPC. Multiple clients, timeout, server-side delay, endpoint deletion, and task death can break reply binding or leave stale state.

### Target Semantics

Endpoint supports these operations:

- `send`: enqueue one-way message, optionally blocking for queue space
- `recv`: server receives a request and receives a reply capability for call-style messages
- `reply`: server replies using a reply capability
- `call`: client sends request and blocks for reply atomically

Reply binding is per request, not per endpoint. A server must reply through a kernel-created reply object/cap that names exactly one pending client call.

### Request Slot

Each pending endpoint request stores:

```c
typedef struct {
    uint8_t    in_use;
    uint8_t    needs_reply;
    uint8_t    replied;
    task_id_t  sender_id;
    tcb_t     *sender;
    uint8_t    badge;
    uint16_t   len;
    uint8_t    msg[KERN_EP_MSG_SIZE];
    cap_id_t   reply_cap;
    uint8_t    cap_count;
    cap_id_t   transferred_caps[IPC_CAPS_MAX];
} ep_request_t;
```

The exact fields can be adjusted for static memory limits, but the request slot must own reply state.

### Reply Object

Add a kernel object type `CAP_OBJ_REPLY` or equivalent internal reply handle. A reply cap:

- is created when server receives a call request
- is single-use
- references the request slot/client waiter
- is invalidated on client timeout, client death, endpoint delete, or successful reply
- cannot be forged or reused

### Required Changes

1. Replace `current_sender` with request slots and reply caps.
2. Split endpoint send/call/recv/reply semantics.
3. Add syscall ids or repurpose existing ids carefully:
   - `SYSCALL_EP_SEND`
   - `SYSCALL_EP_CALL`
   - `SYSCALL_EP_RECV`
   - `SYSCALL_EP_REPLY`
4. Make `endpoint_recv()` return:
   - message length/status
   - optional badge
   - reply cap if request expects reply
5. Make `endpoint_reply()` require a reply cap, not endpoint id.
6. On timeout:
   - remove queued request if not received
   - invalidate reply cap if already received
   - wake client with `KERN_ERR_TIMEOUT`
7. On endpoint delete:
   - fail queued senders/callers
   - fail waiting receivers
   - invalidate outstanding reply caps
8. On task death:
   - if client dies: cancel queued/pending request and reply cap
   - if server dies: revoke its reply caps; clients time out or receive `KERN_ERR_NOEXIST`
9. Add usercopy for message buffers and cap arrays.
10. Enforce cap rights:
    - endpoint send/call requires `CAP_WRITE`
    - endpoint recv requires `CAP_READ`
    - endpoint delete/config requires `CAP_MANAGE`
    - reply requires valid `CAP_OBJ_REPLY`

### Acceptance

- Two clients can call the same endpoint concurrently and receive the correct replies.
- Replying twice with the same reply cap fails.
- Client timeout before server recv removes queued request.
- Client timeout after server recv invalidates reply cap.
- Endpoint delete wakes all senders/receivers/callers.
- User message buffers are copied through checked usercopy.
- Capability transfer through IPC only transfers caps with `CAP_TRANSFER`.

### Tests

Add or extend `src/tests/test_ipc_upgrade.c`:

- single call/reply
- two-client call/reply ordering
- delayed server reply
- reply cap single-use
- client timeout before recv
- client timeout after recv
- endpoint delete wakes recv waiter
- endpoint delete wakes call waiter
- wrong cap rights rejected
- bad user message pointer rejected

### Progress

- Done: split endpoint send-slot waiters from server recv waiters.
- Done: bind replies per endpoint and per server task instead of one endpoint-global sender.
- Done: cancel timed-out/deleted clients from the pending request queue, reply wait queue, send wait queue, and server reply bindings.
- Done: reject stale replies when the original client is no longer blocked on that endpoint.
- Done: add server-visible client death notification: reply after client deletion returns `KERN_ERR_NOEXIST`.
- Done: add regression coverage for client death between server receive and reply.
- Done: endpoint request slots are explicit ring-buffer slots carrying sender/message/cap metadata.
- Done: reply bindings are single-use per endpoint/server task; successful reply, stale reply, delete, timeout, and client death clear the binding.
- Done: endpoint syscall paths validate user payload buffers before entering endpoint send/recv/reply.

## P1-3: Channel IPC Semantics

Status: first hardening pass complete; shared memory capability mapping remains.

### Current State

Files:

- `src/kernel/ipc/channel.c`
- `src/kernel/ipc/channel.h`

Channel currently models a two-peer object with one message buffer per direction and optional shared memory. It has receive wait queues, but send-side waiting is incomplete and peer authority is weak.

Known issues:

- Non-peer tasks can fall into the B-side path.
- No explicit send wait queues.
- Channel delete only wakes recv waiters.
- Shared memory is exposed as a raw pointer.
- Timeout cancellation depends on generic task cleanup more than channel-owned state.

### Target Semantics

Channel is a two-party IPC object:

- exactly two connected endpoints: peer A and peer B
- each direction has one bounded message slot or a small ring
- send blocks when the direction is full
- recv blocks when the direction is empty
- delete/task death wakes all senders and receivers
- shared memory access must be mediated by memory/capability mapping

### Required Changes

1. Add explicit wait queues:
   - `a_send_waiters`
   - `b_send_waiters`
   - `a_recv_waiters`
   - `b_recv_waiters`
2. Validate current task is exactly `peer_a` or `peer_b`.
3. Reject send/recv before both peers are connected.
4. Wake opposite recv queue on successful send.
5. Wake corresponding send queue on successful recv.
6. Delete wakes all four queues with `KERN_ERR_NOEXIST`.
7. Task cleanup removes the task from all four queues and disconnects peer state.
8. Replace raw `channel_get_shm()` user return with:
   - memory object cap
   - explicit shared mapping region
   - checked bounds and rights
9. Enforce cap rights:
   - connect requires `CAP_MANAGE`
   - send requires `CAP_WRITE`
   - recv requires `CAP_READ`
   - shm map requires memory/map-specific rights

### Acceptance

- Non-peer send/recv returns `KERN_ERR_CAP` or `KERN_ERR_PARAM`.
- Send blocks when the direction slot is full and wakes after peer recv.
- Send timeout removes the sender from the send wait queue.
- Channel delete wakes send and recv waiters.
- Peer task death disconnects or invalidates the channel cleanly.
- Shared memory cannot expose arbitrary kernel SRAM to user tasks.

### Tests

Add or extend `src/tests/test_ipc_upgrade.c`:

- A-to-B and B-to-A send/recv
- send blocks while slot full
- send timeout cleanup
- recv timeout cleanup
- non-peer rejection
- delete wakes blocked sender
- delete wakes blocked receiver
- peer death cleanup
- shm cap/mapping boundary rejection

### Progress

- Done: add explicit `a_send_waiters` and `b_send_waiters`.
- Done: validate current task is exactly `peer_a` or `peer_b`.
- Done: reject send/recv before both peers are connected.
- Done: wake direction-specific recv queues on send and send queues on recv.
- Done: delete wakes all four channel wait queues.
- Done: task cleanup removes task from all four queues.
- Done: add regression coverage for unconnected channel and non-peer send/recv rejection.
- Done: add delete-wakes-blocked-sender regression.
- Done: send/recv detect dead connected peers and return `KERN_ERR_NOEXIST`.
- Done: add channel peer death regression coverage.
- P1 decision: channel shared memory remains a kernel API pointer for in-kernel tests; user-facing memory object/cap mapping is deferred to the memory-object work in P2/P3.

## P1-4: IPC Capability Transfer Rules

Status: capability helper foundation in progress.

### Current State

IPC and capability code are separate. Endpoint/channel messages carry bytes, but not a well-defined cap transfer envelope.

### Target Model

IPC message has a header:

```c
typedef struct {
    uint16_t len;
    uint8_t  flags;
    uint8_t  cap_count;
    cap_id_t caps[IPC_CAPS_MAX];
} ipc_msg_hdr_t;
```

Transfer rules:

- Sender must hold `CAP_TRANSFER` on every cap being sent.
- Receiver gets a new local cap slot.
- Rights may be reduced by the sender or policy.
- Move transfer invalidates sender cap.
- Copy transfer preserves sender cap.
- Reply caps are not transferable unless explicitly allowed.

### Required Changes

1. Define IPC message header ABI.
2. Add kernel internal transfer helper:
   - validate sender caps
   - allocate receiver caps
   - copy message payload
   - roll back all allocations if any step fails
3. Ensure caps are installed only when the message is actually delivered.
4. Define behavior on receiver full CSpace:
   - fail send/call with `KERN_ERR_RESOURCE`
5. Add syscall wrappers for cap-bearing IPC.

### Acceptance

- IPC transfer never leaks partially installed caps.
- Receiver cannot get more rights than sender had.
- Sender cannot transfer caps without `CAP_TRANSFER`.
- Receiver CSpace full fails cleanly.

### Progress

- Done: add `cap_copy_to(src, cap, dst, rights)` helper.
- Done: add `cap_move_to(src, cap, dst, out_dst)` helper.
- Done: define initial `ipc_msg_hdr_t` and `ipc_cap_xfer_t` envelope.
- Done: add `ipc_transfer_caps()` helper with copy/move support.
- Done: keep legacy `cap_transfer()` as a move wrapper.
- Done: copied caps are linked as children, so source revoke cascades to transferred copies.
- Done: copy rejects rights outside sender cap and caps without `CAP_TRANSFER`.
- Done: move removes the cap from source CSpace and installs it in destination CSpace.
- Done: add capability copy/move regression tests.
- Done: add rollback coverage for multi-cap transfer failure.
- Done: make IPC-layer move transactional by staging destination caps before revoking source caps.
- Done: add move rollback coverage so failed multi-cap transfer leaves source CSpace intact.
- Done: add endpoint cap-bearing send/recv wrappers (`endpoint_send_caps`, `endpoint_recv_caps`).
- Done: wire endpoint cap-bearing receive path through `ipc_transfer_caps()`.
- Done: add endpoint cap transfer regression coverage.
- Done: add channel cap-bearing send/recv wrappers (`channel_send_caps`, `channel_recv_caps`).
- Done: wire channel cap-bearing send path through `ipc_transfer_caps()`, so receiver CSpace exhaustion is returned to the sender before the message is queued.
- Done: IPC MOVE transfer uses staged destination caps plus source revoke, rather than direct owner mutation, so failed multi-cap moves leave source caps intact.
- Done: define initial channel receiver-full behavior: cap-bearing messages stay queued and `channel_recv()`/failed `channel_recv_caps()` returns an error without consuming payload.
- Done: add channel cap transfer regression coverage.
- Done: add syscall wrappers for cap-bearing endpoint/channel IPC.
- Done: queued copied caps are defined as derived authority; source revoke/death may revoke queued copies by cascade. Sender-visible channel transfer failures are reported before queueing.

## P1-5: VFS Directory, Path, Mount, and FD Semantics

Status: first directory/path pass in progress.

### Current State

Files:

- `src/kernel/vfs/vfs.c`
- `src/kernel/vfs/ramfs.c`
- `src/kernel/vfs/devfs.c`
- `src/kernel/vfs/inode.c`
- `src/kernel/vfs/vfs.h`

The shell already exposes a symptom: `ls: readdir not supported`. VFS has inode trees and file operations, but directory iteration, path normalization, mount behavior, and fd lifecycle are incomplete.

### Target Semantics

VFS should be good enough to support:

- `/`, `/dev`, `/tmp`
- `ls /`, `ls /dev`, `ls /tmp`
- open/read/write/close/ioctl with per-task fd table
- directory open + readdir
- path normalization for `.`, `..`, repeated slashes, trailing slash
- mount table redirect
- cap-backed file descriptors
- task exit closes fds

### Directory API

Add or complete:

```c
typedef struct {
    char     name[INODE_NAME_LEN];
    uint8_t  type;
    uint32_t size;
} vfs_dirent_t;

int vfs_readdir(int fd, vfs_dirent_t *out);
int vfs_rewinddir(int fd);
```

`fd_entry_t` already has offset; use it as directory cursor for simple static directory trees.

### Path Normalization

Add `vfs_normalize_path()`:

- input: user/kernel path string
- output: canonical absolute path
- collapse duplicate slashes
- resolve `.`
- resolve `..` without escaping root
- reject empty component names where invalid
- reject paths longer than fixed buffer

Examples:

- `/dev//uart0` -> `/dev/uart0`
- `/tmp/./a` -> `/tmp/a`
- `/tmp/a/../b` -> `/tmp/b`
- `../../x` from root -> `/x` or reject; choose and document one rule

For P1, prefer requiring absolute paths from syscalls and shell commands.

### Mount Semantics

Current `mount_table` exists but is not a complete filesystem abstraction. P1 should define the minimum:

- mount point must be an existing directory inode
- mounted root replaces lookup below that point
- unmount fails when refs/fds exist
- root `/`, `/dev`, `/tmp` are bootstrapped mounts or built-in mount points

### FD Lifecycle

Required behavior:

- fd allocated per current task
- fd close decrements inode reference
- task exit/fault/delete closes all fds
- cap delete for file closes or invalidates fd according to ownership model
- fd operations verify file cap type and rights
- duplicate close returns stable error

### Device File Semantics

`devfs` should expose devices through the same VFS operations:

- `read`
- `write`
- `ioctl`
- optional nonblocking behavior via flags

Do not add a full driver framework in P1. P1 only defines that `devfs` nodes must behave like VFS character devices.

### Required Changes

1. Add `readdir` support to root inode tree, ramfs, and devfs.
2. Add `vfs_normalize_path()`.
3. Route `vfs_lookup()` through normalized absolute paths.
4. Complete mount table matching behavior.
5. Add `vfs_readdir()` and shell `ls` integration if the shell call path is trivial.
6. Add `task_close_fds(tcb)` and call it from task cleanup.
7. Ensure file caps are per-task CSpace caps after P1-1.
8. Enforce read/write/ioctl rights through caps.

### Acceptance

- `ls /`, `ls /dev`, and `ls /tmp` work.
- `open("/dev/uart0")`, read/write/ioctl use the same fd/cap path.
- Repeated slash and `.` path components resolve correctly.
- `..` does not escape root.
- Closing a file twice fails cleanly.
- Task exit closes fds and releases inode refs.
- File cap from another task is rejected.

### Tests

Add or extend `src/tests/test_vfs.c` and `src/tests/test_shell.c`:

- root readdir includes `dev` and `tmp`
- devfs readdir lists registered devices
- ramfs readdir lists created files
- normalize duplicate slash
- normalize `.`
- normalize `..`
- open/read/write after normalization
- close twice
- task exit closes fd
- wrong file cap rights rejected

### Progress

- Done: add generic inode-tree directory lookup/readdir ops for root-style directories.
- Done: attach readdir-capable dir ops to `/` and `/dev`, so shell `ls /` and `ls /dev` no longer depend on ramfs-only dir ops.
- Done: route `vfs_lookup()` through `dir_ops->lookup`, enabling `.`, `..`, repeated slash, and trailing slash handling across root/dev/tmp.
- Done: reject path components longer than `INODE_NAME_LEN - 1` instead of silently truncating.
- Done: add VFS regression coverage for root/dev readdir and normalized lookup (`//tmp/../dev//./null`).
- Done: add `vfs_close_task_fds(tcb)` and call it from task cleanup before cap revoke.
- Done: add regression coverage that task deletion releases fd-held inode refs.
- Done: add public `vfs_readdir(fd)`/`vfs_rewinddir(fd)` API backed by directory fd offsets.
- Done: route shell `ls` through `vfs_open()` + `vfs_readdir()` so it exercises fd/cap-backed directory iteration.
- Done: add fd-based readdir/rewinddir regression coverage.
- Done: add internal path normalization for `vfs_lookup()` and `vfs_mount()`.
- Done: make mount table match normalized mount-point paths and redirect lookup below the mount point.
- Done: validate mount point/root are directories and reject duplicate mounts.
- Done: add mount redirect regression coverage.
- Done: add `vfs_unmount()` with normalized path lookup and mounted-root ref-busy checks.
- Done: add unmount busy/success/noexist regression coverage.
- Done: add recursive subtree busy checks for open descendants under mounted roots.

## P1 Execution Order

1. Capability CSpace foundation.
2. Convert syscall object lookup to task-aware cap lookup.
3. Endpoint request/reply slot model.
4. Endpoint timeout/delete/task-death cleanup.
5. Channel wait queue and peer validation model.
6. IPC cap transfer envelope.
7. VFS path normalization and readdir.
8. VFS fd lifecycle and task cleanup.

This order keeps authority model first. Endpoint/channel/VFS should not be hardened on top of the old global cap table if that can be avoided.

## P1 Integration Checkpoints

### Checkpoint A: Capability Base

- `make clean && make` passes.
- Existing cap tests pass.
- New CSpace tests pass.
- Existing IPC/VFS syscalls still work through compatibility wrappers.

### Checkpoint B: Endpoint Core

- Endpoint tests pass under multi-client call/reply.
- Timeout and delete cleanup leave no wait queue warnings.
- No user buffer direct dereferences remain in endpoint syscall paths.

### Checkpoint C: Channel Core

- Channel peer validation and four-wait-queue tests pass.
- Delete wakes all waiters.
- Shared memory is no longer returned as unchecked raw kernel pointer to user tasks.

### Checkpoint D: VFS Core

- `ls /`, `ls /dev`, `ls /tmp` work.
- VFS tests cover readdir/path normalization/fd cleanup.
- File caps are local to task CSpace.

## P1 Completion Criteria

P1 is complete when:

- User-visible kernel objects are accessed through task-local capabilities.
- Endpoint IPC has request-bound replies and deterministic timeout/delete cleanup.
- Channel IPC validates peers and cleans all blocking states.
- IPC has documented and tested capability transfer rules.
- VFS supports directory iteration, normalized paths, stable fd lifecycle, and file cap checks.
- A clean build passes.
- STM32F767 test suite reaches shell with zero failures.

## P2 Interface Notes

These are not P1 implementation tasks, but P1 should leave interfaces that P2 can use:

- Timer service can receive requests through endpoint call/reply.
- IRQ threaded handlers can signal user services through endpoint notifications.
- BH service can become a user or kernel service with bounded queues.
- Driver model should expose devices as devfs nodes and grant file/device caps.
- Memory manager should create cap-backed memory objects for shared IPC mappings.
- Shell should use VFS and diagnostic syscalls only, not direct object internals.
- Trace/stats should add structured event classes for IPC/cap/VFS lifecycle.
