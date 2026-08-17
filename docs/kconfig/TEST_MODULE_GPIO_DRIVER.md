# `TEST_MODULE_GPIO_DRIVER`

**GPIO driver (user-mode MMIO) tests**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `y` |
| Menu path | `Test Configuration` |

## Dependencies

- `[TEST_ENABLE](TEST_ENABLE.md) && [MPU_ENABLE](MPU_ENABLE.md) && [CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
核心补齐 #3:USER 任务通过 sys_mmio_request/map 读真实 GPIO 寄存器,
验证用户态驱动端到端(不 fault)。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
