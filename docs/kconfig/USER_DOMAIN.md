# `USER_DOMAIN`

**Per-task private data/heap domain (P1-4, region 1)**

| Property | Value |
|---|---|
| Type | `bool` |
| Default | `n` |
| Menu path | `Microkernel Configuration` |

## Dependencies

- `[MPU_ENABLE](MPU_ENABLE.md) && [CAP_ENABLE](CAP_ENABLE.md)`

## Help

```
用户任务可显式附加私有 data/heap 域:kuser_domain_attach() 从 Frame
池分配 MPU 合规内存,映射到静态 region 1(RW+XN),frame cap 归任务
持有(可显式流转)。任务退出经 cap 吊销自动回收。P1-7(双进程同名
全局互不可见)的地基;默认关闭。
```

---

- Back to [INDEX](INDEX.md)
- Top-level [Kconfig](../../Kconfig)
- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)
