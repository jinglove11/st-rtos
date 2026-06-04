# Phase 4: User Service Supervisor Test Matrix

> Board: STM32F767ZI Nucleo
> Scope: shell-managed user driver and FS services
> Status: board validated through manual serial tests

## Service Registry

| Service | Type | Backing stack | Probe command |
|---|---|---|---|
| `dev.uart0` | driver | user UART server + driver name-server | `svc probe dev.uart0` |
| `fs.ramfs` | fs | user RAMFS server + FS name-server | `svc probe fs.ramfs` |

## Control Surface

| Command | Purpose | Expected state effect |
|---|---|---|
| `svc` / `svc status` | Print service registry status | Read-only |
| `svc stats` | Print aggregate services/running/unhealthy/restarts/recovers/faults | Read-only |
| `svc start <service>` | Start a stopped service stack | Starts task and lookup path, does not increment counters |
| `svc stop <service>` | Stop a service stack | Alias of `svc down` |
| `svc down <service>` | Stop a service stack | Clears service task, lookup path, and health to state |
| `svc recover <service>` | Manual health-driven recovery | Increments recover counter |
| `svc restart <service>` | Manual down/up restart | Increments restart counter |
| `svc health <service>` | Ping health through registry dispatch | Updates last health |
| `svc probe <service>` | Functional service probe | Runs existing driver/FS probe sequence |
| `svc policy <service> manual` | Disable auto restart | Clears max restart limit |
| `svc policy <service> auto <max>` | Enable auto restart | Stores max restart limit |
| `svc supervise [service]` | Manual supervisor tick | Restarts unhealthy auto services under limit |
| `svc fault <service>` | Controlled fault injection | Unregisters service, deletes service task/endpoint, records fault |
| `svc clear <service>` | Clear counters only | Preserves policy and health |
| `svc reset <service>` | Reset supervisor metadata | Clears counters and policy, refreshes health |
| `svc stress <service> <loops>` | Bounded fault/supervise stress | Runs 1..10 fault + supervise loops, then probe/stats |

## Validated Sequences

### Manual Start And Probe

```text
svc start dev.uart0
svc start fs.ramfs
svc probe dev.uart0
svc probe fs.ramfs
svc stats
```

Expected:
- `running: 2`
- `unhealthy: 0`
- both probes report `ping: ok`

### Manual Fault Without Auto Policy

```text
svc start dev.uart0
svc fault dev.uart0
svc supervise dev.uart0
svc
```

Expected:
- fault counter increments
- health becomes `noexist` after supervise health check
- service is not restarted while policy is `manual`

### Auto Restart Limit

```text
svc policy dev.uart0 auto 1
svc supervise
svc stop dev.uart0
svc supervise
svc
```

Expected:
- first unhealthy pass restarts the service
- later pass reports `restart limit`
- `restarts: 1` and service remains stopped after limit is reached

### Targeted Recovery

```text
svc start dev.uart0
svc start fs.ramfs
svc policy dev.uart0 auto 2
svc policy fs.ramfs auto 2
svc fault dev.uart0
svc fault fs.ramfs
svc supervise dev.uart0
svc
svc supervise fs.ramfs
svc
```

Expected:
- first supervise only recovers `dev.uart0`
- `fs.ramfs` remains faulted until its targeted supervise command
- both services are healthy after the second targeted supervise

### Counter Clear

```text
svc policy dev.uart0 auto 2
svc fault dev.uart0
svc supervise dev.uart0
svc clear dev.uart0
svc
```

Expected:
- `restarts`, `recovers`, and `faults` clear to zero
- policy remains `auto`
- max restarts remains `2`
- health remains `ok`

### Stress Loop

```text
svc start dev.uart0
svc policy dev.uart0 auto 5
svc clear dev.uart0
svc stress dev.uart0 3
svc
```

Expected:
- `restarts: 3`
- `faults: 3`
- `health: ok`
- final probe reports all driver operations ok

## Error Semantics

| Command | Invalid input | Expected output |
|---|---|---|
| `svc fault missing` | Unknown service | `svc fault: service not found: missing` |
| `svc supervise missing` | Unknown service | `svc supervise: service not found: missing` |
| `svc clear missing` | Unknown service | `svc clear: service not found: missing` |
| `svc stress missing 1` | Unknown service | `svc stress: service not found: missing` |
| `svc stress dev.uart0 0` | Loop count too small | `svc stress: loops must be 1..10` |
| `svc stress dev.uart0 11` | Loop count too large | `svc stress: loops must be 1..10` |

## Current Limits

- `svc supervise` is a manual tick, not a background supervisor thread.
- Fault injection is controlled service-task termination, not a CPU exception.
- `svc stress` is bounded to 10 loops to avoid accidental long serial runs.
- Driver and FS recovery currently reuse their existing stack restart paths.
