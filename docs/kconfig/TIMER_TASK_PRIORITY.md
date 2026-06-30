# `TIMER_TASK_PRIORITY`

**Timer service task priority**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `1` |
| Range | `0 31` |
| Menu path | `Timer Configuration` |

## Dependencies

- `[TIMER_ENABLE](TIMER_ENABLE.md)`

## Help

```
定时器服务任务优先级。
数值越小优先级越高。
建议设为较高优先级以确保定时器响应及时。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
