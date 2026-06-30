# `SMP_WORK_STEALING`

**Idle CPU steals work from busy CPU**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Phase 6 — SMP (Cortex-M33 Core 1)` |

## Dependencies

- `[SMP](SMP.md)`

## Help

```
idle CPU 从其他 CPU 的 run queue 末尾偷任务。
关闭则任务严格按 affinity 跑。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
