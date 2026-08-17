# `KERN_TASK_CAP_SLOTS`

**Capability slots per task**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `64` |
| Range | `8 64` |
| Menu path | `Capability Configuration` |

## Dependencies

- `[CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
每个任务 TCB 内的 capability slot 数量。
M2-#4: 默认 64 (历史 32→64),由 capabilities uint64 位图驱动。
必须与 tcb_t.cap_set[] 的静态大小保持一致 (静态 assert 强制)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
