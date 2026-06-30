# Phase 4: User-Service Supervisor Checklist

> Board: STM32F767ZI Nucleo
> Status: complete for the Phase 4 boundary
> Validation: serial command tests plus `make BOARD=stm32f767`
> Current baseline: FLASH 331168 B, SRAM 74880 B

## Completion Criteria

Phase 4 is considered complete when the system can run shell-managed user-space
driver and FS services, register them in a common supervisor registry, validate
them through IPC/name-server paths, inject controlled service faults, recover
them through policy-aware supervisor commands, and document the test matrix.

## Service Registry

| # | Item | Status |
|---|---|:---:|
| 1.1 | Register `dev.uart0` as a supervisor service | done |
| 1.2 | Register `fs.ramfs` as a supervisor service | done |
| 1.3 | Preserve restart/recover/fault counters per service | done |
| 1.4 | Track pending client count placeholder per service | done |
| 1.5 | Track last health per service | done |
| 1.6 | Track restart policy and max restart limit per service | done |

## User Driver Service

| # | Item | Status |
|---|---|:---:|
| 2.1 | Start user driver inbox | done |
| 2.2 | Start driver name-server | done |
| 2.3 | Start UART user service task | done |
| 2.4 | Register `dev.uart0` in the name-server | done |
| 2.5 | Validate descriptor lookup | done |
| 2.6 | Validate ping/open/close/status/resources/poll | done |
| 2.7 | Validate MMIO attach/write/detach probe path | done |
| 2.8 | Validate controlled UART service fault injection | done |
| 2.9 | Validate restart after fault through supervisor policy | done |

## User FS Service

| # | Item | Status |
|---|---|:---:|
| 3.1 | Start FS inbox | done |
| 3.2 | Start FS name-server | done |
| 3.3 | Start RAMFS user service task | done |
| 3.4 | Register `fs.ramfs` in the name-server | done |
| 3.5 | Validate ping/open/readdir/close | done |
| 3.6 | Validate create/write/stat/unlink | done |
| 3.7 | Validate mkdir/stat/unlink directory path | done |
| 3.8 | Validate controlled FS service fault injection | done |
| 3.9 | Validate restart after fault through supervisor policy | done |

## Supervisor Shell Commands

| # | Command | Status |
|---|---|:---:|
| 4.1 | `svc` / `svc status` | done |
| 4.2 | `svc stats` | done |
| 4.3 | `svc start <service>` | done |
| 4.4 | `svc stop <service>` | done |
| 4.5 | `svc down <service>` | done |
| 4.6 | `svc recover <service>` | done |
| 4.7 | `svc restart <service>` | done |
| 4.8 | `svc health <service>` | done |
| 4.9 | `svc probe <service>` | done |
| 4.10 | `svc policy <service> manual` | done |
| 4.11 | `svc policy <service> auto <max>` | done |
| 4.12 | `svc supervise` | done |
| 4.13 | `svc supervise <service>` | done |
| 4.14 | `svc fault <service>` | done |
| 4.15 | `svc clear <service>` | done |
| 4.16 | `svc reset <service>` | done |
| 4.17 | `svc stress <service> <loops>` | done |

## Validated Control Loops

| # | Scenario | Status |
|---|---|:---:|
| 5.1 | Cold `svc` shows both services stopped/manual | done |
| 5.2 | Manual `svc start` starts driver and FS without incrementing counters | done |
| 5.3 | `svc probe` validates both services after start | done |
| 5.4 | `svc stop` returns services to stopped/state | done |
| 5.5 | `svc policy auto <max>` updates policy and limit | done |
| 5.6 | Manual policy prevents restart during `svc supervise` | done |
| 5.7 | Auto policy restarts unhealthy services under limit | done |
| 5.8 | Restart limit prevents further restarts | done |
| 5.9 | Targeted supervise recovers only the selected service | done |
| 5.10 | Fault counters increment on `svc fault` | done |
| 5.11 | `svc clear` clears counters while preserving policy and health | done |
| 5.12 | `svc reset` clears metadata and refreshes health | done |
| 5.13 | `svc stress dev.uart0 3` completes fault/supervise/probe loop | done |

## Error Semantics

| # | Case | Status |
|---|---|:---:|
| 6.1 | Unknown service for `svc policy` reports not found | done |
| 6.2 | Invalid policy reports invalid policy | done |
| 6.3 | Unknown service for `svc recover` reports not found | done |
| 6.4 | Unknown service for `svc restart` reports not found | done |
| 6.5 | Unknown service for `svc health` reports not found | done |
| 6.6 | Unknown service for `svc probe` reports not found | done |
| 6.7 | Unknown service for `svc fault` reports not found | done |
| 6.8 | Unknown service for `svc clear` reports not found | done |
| 6.9 | Unknown service for `svc stress` reports not found | done |
| 6.10 | `svc stress` rejects loop counts outside `1..10` | done |

## Documentation

| # | Document | Status |
|---|---|:---:|
| 7.1 | `P4_MICROKERNEL_REFACTOR_PLAN.md` updated with completed work | done |
| 7.2 | `docs/phase4/SUPERVISOR_TEST_MATRIX.md` added | done |
| 7.3 | `docs/phase4/COMPLETION_REPORT.md` added | done |
| 7.4 | `docs/phase4/CHECKLIST.md` updated to current P4 scope | done |

## Deferred To Phase 5

| # | Item | Reason |
|---|---|---|
| 8.1 | Background supervisor task | P4 keeps supervision as deterministic manual tick |
| 8.2 | Boot service manifest | Needs service dependency model and startup policy |
| 8.3 | Real CPU-fault injection inside user services | Current P4 validates controlled service-loss fault injection |
| 8.4 | Dependency-aware recovery ordering | Requires dependency metadata |
| 8.5 | Generic resource manager for MMIO/IRQ | Current P4 validates UART MMIO attach path only |

## Summary

| Area | Total | Done | Completion |
|---|---:|---:|---:|
| Service registry | 6 | 6 | 100% |
| User driver service | 9 | 9 | 100% |
| User FS service | 9 | 9 | 100% |
| Supervisor shell commands | 17 | 17 | 100% |
| Validated control loops | 13 | 13 | 100% |
| Error semantics | 10 | 10 | 100% |
| Documentation | 4 | 4 | 100% |
| **Phase 4 boundary** | **68** | **68** | **100%** |

Phase 4 is complete at the current boundary. The remaining work belongs to
Phase 5: turning the manual supervisor control plane into background service
management and boot-time service orchestration.
