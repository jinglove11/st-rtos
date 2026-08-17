# `TEST_MODULE_MMIO`

**MMIO mapping tests**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Test Configuration` |

## Dependencies

- `[TEST_ENABLE](TEST_ENABLE.md) && [MPU_ENABLE](MPU_ENABLE.md) && [CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
核心补齐 #2:MMIO cap→MPU 映射测试(region 编程、unmap 清除、
权限/坏 cap 拒绝)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
