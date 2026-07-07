# `MPU_REGION_COUNT`

**MPU regions per task**

| Property | Value |
|---|---|
| Type | `int` |
| Default | `5` |
| Range | `3 8` |
| Menu path | `Microkernel Configuration` |

## Dependencies

- `[MPU_ENABLE](MPU_ENABLE.md)`

## Help

```
每个用户任务使用的 MPU region 数量。
典型: 0=代码, 1=数据, 2=栈, 3-4=共享/外设。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../planning/MICROKERNEL_OS_ROADMAP.md)
