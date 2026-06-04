# Phase 4 Completion Report

> Board: STM32F767ZI Nucleo
> Status: implementation and board validation substantially complete
> Validation style: shell command tests over serial plus `make BOARD=stm32f767`

## Summary

Phase 4 now provides a usable user-space service layer on top of the earlier
microkernel primitives. The board-validated surface covers driver and FS service
startup, service registration, name-server lookup, service probing, manual
lifecycle control, supervisor metadata, policy-controlled restart, controlled
fault injection, counter management, and bounded stress testing.

The supervisor is intentionally still a manual shell-driven control plane. The
current `svc supervise` command is a deterministic supervisor tick rather than a
background daemon. This keeps Phase 4 validation explicit and repeatable while
leaving a clear path for Phase 5 to move the same policy logic into a periodic
or event-driven supervisor task.

## Implemented Service Stacks

| Stack | Service | Capabilities |
|---|---|---|
| User driver | `dev.uart0` | descriptor lookup, ping, open/close, status, resources, poll, MMIO attach/detach probe |
| User FS | `fs.ramfs` | ping, open, readdir, create, write, stat, unlink, mkdir |

## Supervisor Control Plane

| Area | Commands |
|---|---|
| Status | `svc`, `svc status`, `svc stats` |
| Lifecycle | `svc start`, `svc stop`, `svc down`, `svc recover`, `svc restart` |
| Validation | `svc health`, `svc probe` |
| Policy | `svc policy <service> manual`, `svc policy <service> auto <max>` |
| Supervision | `svc supervise`, `svc supervise <service>` |
| Faults | `svc fault <service>` |
| Counters | `svc clear <service>`, `svc reset <service>` |
| Stress | `svc stress <service> <loops>` |

## Board-Validated Behaviors

| Behavior | Result |
|---|---|
| Cold `svc` registry view | Both services visible, stopped, manual policy |
| `svc start` | Starts target service without incrementing restart/recover counters |
| `svc stop` / `svc down` | Stops target service and marks health state |
| `svc probe` | Driver and FS functional probes pass after startup |
| `svc policy` | Manual/auto policy and max restart limit update correctly |
| `svc supervise` manual policy | Detects unhealthy services but does not restart |
| `svc supervise` auto policy | Restarts unhealthy services while under max limit |
| Restart limit | Stops auto restart after configured max is reached |
| Targeted supervise | Can recover one faulted service without recovering the other |
| `svc fault` | Unregisters service, deletes service task/endpoint, records fault |
| Fault counters | Fault count increments and is visible in status/stats |
| `svc clear` | Clears restart/recover/fault counters while preserving policy and health |
| `svc reset` | Clears metadata and policy while refreshing current health |
| `svc stress` | Runs bounded fault/supervise loops and final probe successfully |

## Recent Build Baseline

```text
make BOARD=stm32f767
FLASH: 331168 B / 2 MB
SRAM:   74880 B / 384 KB
```

## Current Limits

- `svc supervise` is manual, not a background task.
- `svc fault` injects controlled service loss, not a CPU exception inside the
  user service.
- Fault recovery reuses existing stack restart paths, so a faulted service may
  restart its name-server and inbox as part of the existing down/up sequence.
- The debug UART compatibility path remains active while the user UART service
  path is validated.
- `svc stress` is bounded to 10 loops to keep serial-driven tests safe.

## Phase 5 Candidates

1. Move `svc supervise` policy logic into a real supervisor task or periodic
   event tick.
2. Add service dependency metadata so FS and driver startup can be ordered and
   reported by the supervisor.
3. Add event-driven fault notification instead of relying only on health probes.
4. Split controlled service-loss fault injection from real user-task CPU fault
   injection.
5. Reduce restart verbosity once the lifecycle is stable enough for normal use.

## References

- `docs/phase4/SUPERVISOR_TEST_MATRIX.md`
- `P4_MICROKERNEL_REFACTOR_PLAN.md`
