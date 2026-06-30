# Phase 5: Service Runtime Supervisor

> Date: 2026-06-09
> Scope: post-P4 service lifecycle runtime
> Note: existing `docs/phase5/DESIGN.md` and `CHECKLIST.md` describe an older IPC-upgrade phase. This file tracks the P4 follow-up service runtime work.

## Goal

Move service supervision from a purely manual shell workflow to a small background runtime that can observe registered user services, apply restart policy, and later support boot manifests and dependency ordering.

## Current baseline

Phase 4 completed the service control plane:

- Service registry for `dev.uart0` and `fs.ramfs`
- Manual start, stop, restart, recover, probe, health, stats
- Restart policy: `manual` / `auto` with max restart budget
- Fault injection, targeted supervision, counter clear/reset
- Stress command for repeated fault/restart validation

## Step 1: Background runtime task

Status: in progress

Implemented first:

- `svc runtime status`
- `svc runtime start [period-ticks] [tick|health|auto]`
- `svc runtime stop`
- Dedicated `svc.rt` task
- Runtime tick counter
- Configurable tick period, range `1..10000`
- Initial mode is `tick-only`

This first cut intentionally does not mutate service state. It proves that the shell can manage a long-lived supervisor task without changing the validated P4 recovery semantics.

Board validation checklist:

- `svc runtime status` before start shows `task: none`
- `svc runtime start 50` creates `svc.rt`
- Repeated `svc runtime status` shows `runtime ticks` increasing
- `svc runtime stop` deletes the runtime task
- Invalid periods return an error:
  - `svc runtime start 0`
  - `svc runtime start 10001`
- Starting twice reports `already running`
- Stopping twice reports `already stopped`

Automated serial smoke test:

```sh
python3 scripts/serial_svc_runtime_test.py --port /dev/ttyACM0
```

The script validates prompt access, runtime start/stop/status, tick growth, duplicate start/stop handling, and invalid period errors.
It also covers:

- `health-sweep` counters
- `auto-restart` safe path under manual policy
- `dev.uart0` fault injection under `policy auto`
- runtime `actions` incrementing after auto restart
- service `restarts` incrementing after auto restart

Boot self-test without shell commands:

- `src/tests/test_svc_runtime.c`
- Runs automatically in `My-RTOS Test Suite`
- Calls `shell_svc_runtime_selftest()` directly
- Validates tick-only task progress
- Validates health-sweep counters without typing `svc ...`
- Validates auto-restart mode is safe under manual policies
- Expected boot line:
  - `[MODULE] svc_runtime pass +1 fail +0 ...`

## Step 2: Runtime health sweep

Status: implemented, pending board validation

Implemented:

- `svc runtime start <period> health` enables periodic health sweeps
- `svc runtime start <period> tick` keeps tick-only behavior
- Runtime health sweep checks all registered services
- Existing manual `svc supervise` output path remains unchanged
- Runtime task does not print from the background path
- Runtime records shell-visible status:
  - `sweeps`
  - `checks`
  - `last service`
  - `last health`
- Boot self-test validates runtime tick and health modes without shell input

Board validation checklist:

- `svc runtime start 5 health`
- Repeated `svc runtime status` shows `mode: health-sweep`
- `sweeps` increases above 0
- `checks` increases above 0
- Services are not restarted by health-sweep mode
- `svc runtime start 5 unknown` returns `mode must be tick or health`

## Step 3: Runtime auto-restart

Status: initial implementation, pending fault/restart board validation

- `svc runtime start <period> auto` enables auto-restart mode
- Runtime still performs periodic health sweeps
- A failed service is restarted only when:
  - service policy is `auto`
  - `supervisor_should_auto_restart()` allows another restart
- Manual-policy services are observed but not restarted
- Runtime records:
  - `actions`
  - `last action`
- Existing service-specific restart handlers are reused

Validation still needed:

- below limit: restart
- at limit: leave stopped/unhealthy
- manual policy: observe only

## Step 4: Boot manifest

Planned:

- Define static manifest entries for user services
- Add boot-time or shell-triggered autostart
- Track desired state separately from observed state
- Support disabled/manual service entries

## Step 5: Dependencies

Planned:

- Add dependency metadata, initially static
- Start dependencies before dependents
- Stop dependents before dependencies
- Report blocked services with dependency reason

## Step 6: Event path

Planned:

- Add service event records:
  - started
  - stopped
  - faulted
  - restarted
  - restart-limit
- Keep a small ring buffer for `svc events`
- Later connect events to endpoint/channel clients.

## Progress

Service runtime Phase 5 is at the first implementation slice:

- Background runtime shell surface: started
- Background runtime task: started
- Health sweep: implemented, pending board validation
- Auto-restart from runtime: initial implementation, pending fault board tests
- Boot manifest: not started
- Dependencies: not started
- Event path: not started

Estimated completion for this Phase 5 track after Step 2 is board-validated: about 25%.
