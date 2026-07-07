# `MUTEX_DEADLOCK_DETECT`

**Enable deadlock detection**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `IPC Configuration` |

## Dependencies

- `[IPC_MUTEX](IPC_MUTEX.md)`

## Help

```
启用互斥锁死锁检测。
在获取锁前检测等待图环，防止死锁形成。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
