# `INIT_PROCESS`

**User-mode init task**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 2 — Fault-Tolerant Infrastructure` |

## Dependencies

- `[SUPERVISOR](SUPERVISOR.md)`

## Help

```
bootstrap 后第一个用户任务,负责启动 supervisor/driver servers/shell。
实现:Phase 2.3。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
