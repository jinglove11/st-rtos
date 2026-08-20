# 内核部件完整度开发 Backlog(基线 77%,加权口径)

> 分支:`kernel-completeness`(从不直接合 master,板上测试全绿后由维护者合并)
> 工作流:每次闲时任务运行完成 **一项**,双板构建绿才算 done,更新本表状态并提交。
> 优先级:S = 快赢(半天内)/ M = 中 / L = 大(可分多次运行,每次推进一个可验证切片)

## 状态说明

- [ ] todo — 未开始
- [~] in-progress — 已有部分提交,继续推进
- [x] done — 代码落地 + 双板构建绿(待板上回归)
- [P] parked — 被阻塞(注明原因)

## P0 快赢批(S 级,先清完)

- [x] P0-1 (B3) 移除 `obj_generation=0` 校验豁免后门(capability.c:288);同步修 test_capability.c 的临时对象用法
  - 实现:cap_create/cap_create_for 自动从对象 header 取 generation;cap_create_for_gen_badge 拒绝非空对象+gen0创建;cap_get_entry 豁免移除。测试侧 51+ 处假对象迁 test_cap_obj_t(带真 header),2 处 (sup_id+1) 假任务指针改 task_obj_for_cap,15 处真对象显式 gen0 改 cap_create_for。双板构建绿(2026-08-20)
- [x] P0-2 (B5) `sys_mem_alloc` 按 rights 参数发放,不再默认全权 RW|MANAGE|TRANSFER
  - 实现:a2=rights 显式参数(合法集 RW|MANAGE|TRANSFER,无 GRANT,非空子集校验);
    ABI 1.0→1.1(树内无外部消费者,按兼容扩展);fs_server 给 RW|MANAGE,测试同步。双板构建绿(2026-08-20)
- [x] P0-3 (C8) sync 四族用户 syscall 取舍评估落文档(结论可以是"保留",但要写清 why)
  - 结论:短期保留(PI/死锁检测价值真实 + sync_server 阻塞锁未完成),中期按 P2-4 分族降级;
    三个降级触发条件见 docs/design/SYNC_SYSCALL_RETENTION.md §4
- [x] P0-4 (D1) CMake 弃用 GLOB_RECURSE,改显式 source manifest(对齐 Makefile)
  - 实现:三处 GLOB(GLOB_RECURSE ×1 + GLOB ×4)全部替换为显式 set() 清单
    (kernel 36 / user 13 / tests 44 文件);新文件需手动登记。双板构建绿(2026-08-20)
- [x] P0-5 (D2) 补齐 test/dev/release 三 profile(dev = 无测试有 shell 的开发镜像)
  - 实现:Kconfig DEV_PROFILE(depends !TEST_ENABLE && SHELL_ENABLE)+ main.c release 分支起 shell
    + configs/rp2350_dev_defconfig。顺带修两个存量 TEST-off 断裂:cap_test_* 钩子
    guard 失配(内核侧 TEST_ENABLE vs 测试侧 TEST_MODULE_CAP,双侧对齐后者)、
    main.c release 分支缺 system_init.h。dev 镜像构建绿(test_runner 符号缺席验证)
- [ ] P0-6 (C7) 清理 10 个 sys_nosys VFS 死槽(保留 ABI 编号,注释明确 reserved)
- [ ] P0-7 (D5) kernel_config.h 移出 git 跟踪,改为纯生成物(.gitignore + 构建依赖修正)

## P1 M4 批(结构性,L 级,每次一个可验证切片)

- [ ] P1-1 (A4) 独立 notification 对象(CAP_OBJ_NOTIFICATION,单字 badge,聚合 word)— M4 前置
- [ ] P1-2 (C2) `mpu_domain_t`/`address_space_t` 从 TCB 分离 mapping policy
- [ ] P1-3 (C3) MPU region 动态分配器(突破每任务 5 映射上限)
- [ ] P1-4 (C2) 用户任务私有 data/heap 域(code/rodata/data/stack region 布局)
- [ ] P1-5 (C1) ELF loader:完整段校验 + 溢出检查 + W^X 强制
- [ ] P1-6 (C1) ELF loader:重定位(R_ARM_ABS32/MOVW)+ backing 跟踪与退出回收
- [ ] P1-7 验收:两个用户"进程"同名全局变量互不可见 + ELF 万次加载无泄漏

## P2 边界收敛批

- [ ] P2-1 (A5) 封 `sys_task_create` 用户直呼:init 全面迁 factory cap 路径后加 PERM 门
- [ ] P2-2 (A2) timer 通知化:去内核回调路径,全部走 notification
- [ ] P2-3 (A2) BH 服务任务去内核化或并入 IRQ 通知路径
- [ ] P2-4 (A1) sync 四族降级用户态库(保留内核兼容层,Kconfig 切换)
- [ ] P2-5 (A3) shell 用户态化(kernel-task 类收敛到 idle/必需)

## P3 硬化批

- [ ] P3-1 (B1) cap pin 铺设:endpoint/channel/task 的 syscall 路径全面 pin/unpin
- [ ] P3-2 (B2) 对象池 per-task 配额(factory cap 掩码即配额)
- [ ] P3-3 (B4) SHM/MMIO unmap 与 in-flight syscall 的竞态审计 + quiesce 收口
- [ ] P3-4 (B6) test 7 残余窗口归零(RA 陷阱触发后按投递者地址定位)

## P4 功能补全批

- [ ] P4-1 (C4) Channel 连接生命周期语义收口(多客户端/disconnect 竞态)
- [ ] P4-2 (C5) 调度器 tickless idle
- [ ] P4-3 (C5) SCHED_FIFO/RR RT class
- [ ] P4-4 (C5) CPU 利用率核算(shell top 数据源)
- [ ] P4-5 (C6) abi_header_t 铺满全部带结构 syscall
- [ ] P4-6 (D3) 8h soak 留档(需要板子接好时跑)
- [ ] P4-7 (D4) CI 矩阵纳入 stress preset 与 SYS 层断言

## 规则(每次运行必读)

1. 工作区不干净 → 直接退出(用户可能正在工作),不打扰
2. 一次只做一项;L 级做"一个可验证切片"(编译过 + 有最小验证手段)
3. 完成判据:RP2350(`cmake --build build/rp2350-pico-sdk -j4`)与
   STM32(`make BOARD=stm32f767 -j4`,结束后 `git checkout -- .config &&
   python3 scripts/menuconfig.py genconfig` 恢复)双绿
4. 提交到本分支,信息格式:`[Pn-m] 标题(状态:done|slice k/N)`
5. 更新本表勾选状态,随代码一起提交
6. **绝不 merge/rebase 到 master**;绝不 force-push
7. 若板子在位(串口设备存在),可选跑 `scripts/regression.sh`,结果记入本表备注
