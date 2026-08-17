# `SMP_STRESS_ITERATIONS`

**SMP ping-pong iterations**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `10000` |
| Range | `100 1000000` |
| Menu path | `Phase 6 — SMP (Cortex-M33 Core 1)` |

## Dependencies

- `[TEST_MODULE_SMP](TEST_MODULE_SMP.md)`

## Help

```
SMP smoke profile uses 10000.  The dedicated RP2350 SMP acceptance
profile sets 1000000 for the M1 cross-core semaphore and endpoint gates.
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
