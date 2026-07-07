# `KERN_TASK_STATS`

**Enable task CPU usage statistics**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Kernel Configuration` |

_No dependencies._

## Help

```
启用任务 CPU 使用率统计。
每个 TCB 记录上下文切换次数和 CPU 使用率百分比。
shell `top` 命令可显示各任务 CPU 占用。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
