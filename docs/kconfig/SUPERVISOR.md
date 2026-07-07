# `SUPERVISOR`

**User-mode supervisor task**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 2 — Fault-Tolerant Infrastructure` |

## Dependencies

- `[FAULT_ENDPOINT](FAULT_ENDPOINT.md)`

## Help

```
监控任务,接收 fault_info,按策略重启或 kill 故障任务。
Rate limit: 每服务每 5s 最多 1 次重启,3 次后永久 kill。
实现:Phase 2.2。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
