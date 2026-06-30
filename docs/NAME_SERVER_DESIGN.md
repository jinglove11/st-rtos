# Name Server Design

Status: Phase 8 core implementation complete and board-validated.

## Purpose

The name server is the first user-space service that turns the Phase 7
root/init bootstrap into a usable service model. Kernel code should not hand raw
endpoint ids to clients. A service registers a name and endpoint capability with
the name server, and clients ask the name server for a derived endpoint cap.

The first implementation is intentionally small: fixed registry size, fixed
name length, synchronous endpoint IPC, and explicit capability transfer. It must
prove the authority path before broader driver and FS service migration.

## Scope

Implemented in the Phase 8 core slice:

- root/init creates and starts the name server as a user task;
- root/init gives the name server an endpoint cap;
- a service registers one endpoint cap under one service name;
- a client looks up that name and receives a reduced endpoint cap;
- unregister removes a name and revokes or stops future derived transfers;
- name-server task death does not corrupt kernel state.

Still out of scope:

- persistent namespace;
- hierarchical paths;
- wildcard discovery;
- rich policy language;
- automatic restart supervision;
- public shell command surface.

## Objects And Authority

Initial authority comes from root/init:

| Object | Owner | Rights |
| --- | --- | --- |
| name-server task cap | root/init | `CAP_FULL` |
| name-server endpoint cap | root/init + name server | root gets `CAP_FULL`, server gets `CAP_READ | CAP_WRITE` |
| service endpoint cap | service + root/init | service gets `CAP_READ | CAP_WRITE`, root keeps management authority |
| derived client endpoint cap | client | usually `CAP_READ | CAP_WRITE`, no `CAP_MANAGE` |

The kernel remains responsible for safe cap copy/derive mechanics. The name
server decides only whether a caller may receive a cap for a registered name.

## Registry Model

Current fixed-size registry:

```c
#define NS_NAME_MAX        24
#define NS_REGISTRY_MAX    16

typedef struct {
    uint8_t in_use;
    char name[NS_NAME_MAX];
    cap_id_t endpoint_cap;
    uint8_t rights;
    uint32_t owner_badge;
} ns_entry_t;
```

Rules:

- names are ASCII, NUL-terminated, and cannot be empty;
- duplicate register is rejected with `KERN_ERR_BUSY`;
- register records the request `owner_badge`;
- unregister requires the same owner badge, with future root-authority override
  planned when kernel-provided sender badges exist;
- lookup returns `KERN_ERR_NOEXIST` for missing names;
- lookup never returns `CAP_MANAGE` to ordinary clients;
- registry entries store endpoint caps owned by the name-server task.

## IPC ABI

All requests use endpoint IPC with the existing message buffer size rules. The
payload begins with a fixed header:

```c
typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t flags;
    uint32_t seq;
    int32_t status;
} ns_msg_hdr_t;
```

Constants:

```c
#define NS_MAGIC       0x4E535256U
#define NS_OP_REGISTER 1U
#define NS_OP_LOOKUP   2U
#define NS_OP_UNREG    3U
#define NS_OP_PING     4U
```

Register request:

- input message: header + service name + owner badge;
- transferred caps: one endpoint cap;
- reply: `KERN_OK` or error.

Lookup request:

- input message: header + service name;
- transferred caps: one client inbox endpoint cap;
- direct reply message: status;
- if status is `KERN_OK`, the name server sends one derived service endpoint
  cap to the provided inbox endpoint.

Unregister request:

- input message: header + service name + owner badge;
- transferred caps: none;
- reply: `KERN_OK` or error.

Ping request:

- input message: header;
- reply: `KERN_OK`, same sequence number.

## Client Helpers

`src/user/nameserver/nameserver.c` also provides small client-side helpers for
service code:

- `nameserver_ping()`;
- `nameserver_register()`;
- `nameserver_unregister()`;
- `nameserver_lookup_begin()` and `nameserver_lookup_ack()`.

Lookup is deliberately split into begin/ack because the name server sends the
returned service cap over a client-provided inbox endpoint. The client may need
to validate or use the cap before acknowledging the inbox send; acknowledging
too early lets a short-lived name-server test exit and revoke registry caps.

## Capability Transfer Rules

Register:

1. User service sends its endpoint cap to the name server.
2. Kernel cap-transfer path copies or derives the cap into the name-server
   CSpace.
3. Name server validates the copied cap type is `CAP_OBJ_ENDPOINT`.
4. Name server records the cap and service name.

Lookup:

1. Client sends lookup request to name-server endpoint with an inbox endpoint
   cap.
2. Name server finds entry.
3. Name server replies to the request with status.
4. Name server sends a derived endpoint cap to the client's inbox endpoint.
5. Derived rights are `entry.rights & (CAP_READ | CAP_WRITE)`.

Unregister:

1. Name server deletes the registry entry.
2. Stored name-server endpoint cap is deleted from the name-server CSpace.
3. Existing client caps are not revoked by unregister. Later supervisor
   work can add cascading revoke by service generation.

## Failure Semantics

| Case | Result |
| --- | --- |
| bad magic/opcode | reply `KERN_ERR_PARAM` |
| name too long | reply `KERN_ERR_PARAM` |
| no transferred endpoint cap on register | reply `KERN_ERR_CAP` |
| registry full | reply `KERN_ERR_RESOURCE` |
| duplicate name | reply `KERN_ERR_BUSY` |
| lookup missing name | reply `KERN_ERR_NOEXIST` |
| unregister owner mismatch | reply `KERN_ERR_PERM` |
| helper invalid argument | return `KERN_ERR_PARAM` locally before syscall |
| name server dies | clients time out or receive endpoint-death error |
| registered service endpoint deleted | endpoint delete revokes caps for that endpoint; later lookup cannot transfer a valid service cap |
| registered service dies without endpoint deletion | future supervisor work should unregister or restart the service |

## Implemented Coverage

- `src/user/nameserver/nameserver.h` defines the stable Phase 8 ABI.
- `src/user/nameserver/nameserver.c` contains the service loop and client
  helpers.
- `test_service_model.c` validates root-created user name-server service
  startup, `PING`, `REGISTER`, `LOOKUP`, `UNREG`, duplicate/missing/error
  paths, owner badge enforcement, registry capacity, cap lifecycle recycling,
  helper argument validation, and cleanup accounting.
- Capability and endpoint tests cover object-wide endpoint-cap revocation on
  endpoint deletion.

## Acceptance Tests

- root/init can create and start name server;
- name server receives its endpoint cap through the Phase 7 bootstrap path;
- a service can register one endpoint cap by name;
- a client can look up the service and receive an endpoint cap;
- the client can call the service through the looked-up cap;
- duplicate registration and missing lookup return deterministic errors;
- malformed names and helper-local invalid arguments return deterministic
  errors;
- non-owner unregister returns `KERN_ERR_PERM`;
- registry full returns `KERN_ERR_RESOURCE`;
- cap and heap resource counters are unchanged after the module finishes.

## Later Extensions

- generation numbers so stale client caps can be detected;
- caller badges and per-service allowlists;
- service health integration with supervisor;
- root namespace entries for `/dev`, `/fs`, `/timer`, and `/irq`;
- shell diagnostics for registered services;
- ABI documentation freeze in Phase 12.
