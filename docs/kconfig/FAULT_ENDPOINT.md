# `FAULT_ENDPOINT`

**Fault reporting endpoint**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Phase 2 — Fault-Tolerant Infrastructure` |

## Dependencies

- `[FAULT_ENABLE](FAULT_ENABLE.md) && [IPC_ENDPOINT](IPC_ENDPOINT.md)`

## Help

```
内核保留 endpoint,用户任务 crash 时把 fault_info 打包发送。
Supervisor 通过 sys_fault_subscribe() 订阅。
实现:Phase 2.1。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
