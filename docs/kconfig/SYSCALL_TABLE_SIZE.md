# `SYSCALL_TABLE_SIZE`

**Syscall table size**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `96` |
| Range | `16 128` |
| Menu path | `Microkernel Configuration` |

## Dependencies

- `[SYSCALL_ENABLE](SYSCALL_ENABLE.md)`

## Help

```
系统调用分发表大小。必须 ≥ 最高 syscall 编号 + 1
(当前 SYSCALL_ABI_VERSION=86，所以 default=96)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
