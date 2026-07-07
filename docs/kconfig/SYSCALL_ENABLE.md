# `SYSCALL_ENABLE`

**Enable syscall interface**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Microkernel Configuration` |

## Dependencies

- `[MPU_ENABLE](MPU_ENABLE.md)`

## Help

```
启用系统调用接口 (SVC #1)。
用户任务通过 svc 指令请求内核服务。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
