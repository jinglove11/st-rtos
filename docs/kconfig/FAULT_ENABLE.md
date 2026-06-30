# `FAULT_ENABLE`

**Enable fault handlers**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Fault Handler Configuration` |

_No dependencies._

## Help

```
启用完整的 Fault 异常处理。
MemManage/BusFault/UsageFault → 终止用户任务。
HardFault → crash dump + panic。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
