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
  - 验证补漏(2026-08-20 复核):dev 镜像此前仍链入 11 个无 TEST 门控的测试模块
    (注册宏无总门,service_model 的注册还落在文件级守卫之外)→ 11 个测试文件统一
    `#if TEST_ENABLE` 全文件包裹,dev 镜像测试符号清零(text 404K→169K);dev
    defconfig 残留 TEST_MODULE_*=y 一并清除。另修两处 master 存量断裂:release
    (device.c device_current_task_id 在 TRACE off 下 unused)与 tiny
    (IPC_EP_MSG_SIZE=32 装不下 ns_name_msg_t=44,nameserver.h 加 _Static_assert
    钉死契约,tiny 提到 48)。verify_pico2w_build.py 弃用 picotool info(镜像内
    ASCII 误读 bug,见 KNOWN_ISSUES)改 UF2 直解,ci_local 恢复 full 校验。
    全矩阵绿:rp2350 tiny/default/release/full + stm32 + docs(kconfig 文档补生成
    DEV_PROFILE 页)。
- [x] P0-6 (C7) 清理 10 个 sys_nosys VFS 死槽(保留 ABI 编号,注释明确 reserved)
  - 实现:sys_nosys → sys_reserved_vfs,10 个槽位(OPEN..LSEEK 32-37、READDIR..STAT
    67-70)移出 #if VFS_ENABLE 无条件占表恒 NOSYS——此前条目藏在 VFS_ENABLE 内而
    所有 preset 均关,raw 调用落表洞返回 PARAM,与 user_api.h 承诺的 NOSYS 矛盾
    (行为随配置摇摆)。syscall.h 编号注释 reserved 永不复用;test_syscall 断言
    去 VFS 门控并铺满全部 10 编号。附带:test_smp.c 补 SMP_STRESS_ITERATIONS
    #ifndef 回退(genconfig 不物化 Kconfig 默认值,P0-8 缺口,陈旧 .o 曾掩盖)。
    双板构建绿(测试对象全量重编验证,2026-08-20)
- [x] P0-7 (D5) kernel_config.h 移出 git 跟踪,改为纯生成物(.gitignore + 构建依赖修正)
  - 实现:git rm --cached + .gitignore;CMake 补 add_custom_command(menuconfig.py
    genconfig,DEPENDS .config/Kconfig/menuconfig.py)+ gen_kernel_config target,
    gen_tcb_offsets 顺序依赖之;Makefile 原有 $(CONFIG_HEADER) 规则即可。验证:
    删除头文件后 RP2350(cmake)与 STM32(make)均从零重生成并构建绿(2026-08-20)
- [x] P0-8 (D6) genconfig 输出不执行 depends 收缩(Kconfig 依赖仅在交互菜单 UI 生效,
  defconfig 携带违依赖符号会被原样写进 kernel_config.h——P0-5 验证中 dev 镜像链入
  测试代码的根因;顺手评估 range 下限同样不收缩的问题)
  - 实现:menuconfig.py 新增 deps_satisfied/single_dep_ok(含 !SYMBOL 支持,UI
    _check_depends 复用)+ collapse_depends(不动点迭代收缩违依赖符号,stderr 告警)
    + clamp_ranges(int 越界钳到 range 边界,告警),genconfig 输出前执行。配套修 4 处
    陈旧 Kconfig 依赖(kernel VFS 已删但依赖未摘):SHELL_ENABLE/DRIVER_ENABLE 摘
    VFS_ENABLE、FS_PERSISTENT 改仅 BLOCK_DEVICE、IPC_ENDPOINT_MAX range 1..16→1..64
    (.config 实际用 32)。验证:提交 .config 再生头与旧生效头仅差 3 个合法丢弃
    (CAP_RCU@UP、VFS_MAX_*@VFS-off);违依赖/range 越界负向用例通过;全矩阵绿
    (rp2350×4 + stm32 + smp preset 抽验 + dev 模块残留 0,2026-08-20)

## P1 M4 批(结构性,L 级,每次一个可验证切片)

- [x] P1-1 (A4) 独立 notification 对象(CAP_OBJ_NOTIFICATION,单字 badge,聚合 word)— M4 前置
  - slice 1(2c53252):内核对象 + cap 类型 + syscall 家族 + K 白盒 8 组。
    notification.c(seL4 语义:signal 只 `word|=badge`(badge 来自 mint 的 signal cap,
    CAP_WRITE),wait/poll 整字消费,word 非零唤醒至多一个等待者并锁内移交 +
    copy_to_user,ISR 安全 signal);CAP_OBJ_NOTIFICATION=19;BLOCK_REASON_NOTIFICATION=13
    (timeout/fault 摘除接入 task_unlink_blocked);syscall 87-90
    (NTFN_CREATE/SIGNAL/WAIT/POLL,user_api 内联);factory 两分派接入;Kconfig
    IPC_NOTIFICATION(默认 n,opt-in)+ IPC_NOTIFICATION_MAX;ABI 1.2。
  - slice 2:补内核线程式阻塞变体 notification_wait(手动出队+置 BLOCKED,消除
    sched_block 解锁窗口竞态;移交字经 cont.u.ntfn.word——修正 slice 1 中 K 白盒
    waiter 直呼 SVC 路径的运行时隐患,-128 哨兵只对 SVC 汇编入口有意义);
    补 SYSCALL_NTFN_DELETE=91(CAP_MANAGE);abi/test_ntfn_user.c(模块 ntfn_user)
    5 组用户契约:单任务全链(无徽章 signal=no-op/徽章字位/消费/delete 后 cap 吊销)、
    rights(WRITE 才能 signal,READ 才能 wait/poll)、SVC 超时、SVC 阻塞被内核
    signal 唤醒(用户内存 copyout)、用户→用户跨任务 signal(字=signaler 徽章)。
    验证:双板 + CI 全矩阵 6/6 绿 + 板上真机(工作配置 3351/3351 含 ntfn 63/63 +
    ntfn_user 24/24;SMP preset 3386/3386,2026-08-20)。板上修复:signal 唤醒改
    直写 msg_buf(阻塞时已按等待者上下文校验;copy_to_user 按当前=signal 方的
    MPU 区校验,跨任务等待者栈必被拒——endpoint/mqueue 同款 memcpy 范式);
    fuzz 跳过清单补 NTFN_CREATE/DELETE/WAIT。irq/timer 消费者接入归 P2-2/P2-3。
- [~] P1-2 (C2) `mpu_domain_t`/`address_space_t` 从 TCB 分离 mapping policy
  - slice 1(2026-08-20):行为保持重构。新增 `address_space_t`(regions[8][2],
    mpu.h)+ KERNEL_MAX_TASKS 池(mpu_aspace_acquire/release_task);TCB 的内嵌
    `mpu_regions[8][2]` 改为 `struct address_space *aspace` 指针(内核任务 NULL,
    上下文切换清全部区)。48 触点全量迁移:mpu.c(池+加载经指针)、task.c
    (create_user 获取,池耗尽走正式 delete 回收任务槽;cleanup_resources 在
    kshm/kmmio unmap 之后归还)、mem.c(map 三路径加 NULL 守卫+扫描/编码/
    unmap 清零迁指针)、usercopy(MPU 校验 NULL 早退)、fault(区转储 NULL 安全)、
    elf_loader(region 3)、白盒测试 3 文件;gen_tcb_offsets.c 同步(off_mpu_regions
    → off_aspace,asm 无直接引用)。验证:双板 + CI 全矩阵 6/6 绿 + 板上真机(工作配置/SMP 双 preset 全绿)。
  - 剩余 slice 2:aspace cap 化/共享(多任务同 address_space,M4 进程语义)、
    与 P1-3 动态区分配器衔接
- [x] P1-3 (C3) MPU region 动态分配器(突破每任务 5 映射上限)
  - 实现(2026-08-20):软映射表 mpu_map_t[MPU_MAP_MAX](Kconfig,default 16)进入
    address_space;硬件运行时槽(3..MPU_REGION_COUNT-1)降级为表的 LRU 驻留缓存
    (slot_owner 双向归属 + lru_seq)。mpu_map_add/remove/slot_of/demand_load API;
    槽满不拒映射(表满才 RESOURCE),MemManage(MMARVALID)fault 钩子按需 LRU 换入
    (写本核硬件 + 镜像,正常异常返回重试;已驻留仍违例=真实故障放行)。mem.c
    kshm/kmmio 三 map 三 unmap 全量迁移(槽满仍成功);mmio_map_t 补 addr 字段
    (unmap 时 cap 可能已吊销,不能靠 cap 反查);shm_mapping_t.region 降为顾问值。
    mpu.h MPU_MAP_MAX #ifndef 回退(genconfig 不物化默认值,P0-8 缺口,与
    SMP_STRESS_ITERATIONS 同款)。k/test_mpu_aspace.c(模块 mpu_aspace):容量
    超槽/LRU 逐出恰一/驻留重入 0/未覆盖 -1/remove 清槽归属/表满/重复 base/无
    aspace 拒绝。验证:双板 + CI 全矩阵绿;用户任务真实 fault 换入端到端归板上
    回归。
- [~] P1-4 (C2) 用户任务私有 data/heap 域(code/rodata/data/stack region 布局)
  - slice 1(2026-08-20):Kconfig USER_DOMAIN(默认 n,opt-in)+ kuser_domain_attach/
    detach(mem.c):静态 region 1 显式附加私有域 —— kframe_create_cap_for 分配 MPU
    合规 frame(cap 归任务,可流转),encode RW+XN 进 region 1 镜像;aspace 记
    domain_base/size/cap;detach 清区+吊销 cap(frame 经吊销钩子回收);重复附加
    BUSY/未附加 detach NOEXIST。白盒:test_mpu_aspace Test 3(attach/BUSY/编码
    RW+XN/detach 清区/复用)。设计取舍:不做 create_user 自动附加(全部用户任务
    强制吃 frame 会打断现有测试的内存预算),由 P1-5 ELF loader 按段显式调用。
    验证:双板 + CI 全矩阵绿(工作配置 + full 启用)。
  - 剩余 slice 2:code/rodata/data 段级私有布局(依赖 P1-5 ELF loader 段校验,
    加载时按段 attach 多区);P1-7 双进程同名全局隔离在此之上验收
- [x] P1-5 (C1) ELF loader:完整段校验 + 溢出检查 + W^X 强制
  - 实现(2026-08-20):elf_load 增加 image_size 参数;头校验补 class32/LSB/
    phentsize==sizeof(Phdr)/ph 表界内(溢出安全减法);段校验 W^X 拒绝、
    p_memsz>=p_filesz、文件范围不越镜像界;强制恰一 X 段且 e_entry 落其
    link 范围。可写段改 kframe 分配(cap 归任务,退出自动回收 —— 修旧
    kmalloc 无跟踪泄漏)+ kframe_info_for 公共访问器 + mpu_map_add 登记
    (修旧 regions[3] 直写绕过 P1-3 记账、demand-load 会踩掉数据段的隐患)。
    负向测试 8 组(构造最小 ELF 变体):W^X/越界/memsz<filesz/无 X 段/
    entry 越界/截断/phentsize/双 X 段。验证:双板 + CI + 板上真机
    3360/3360(elf 模块 20/20,正向加载执行 exit=0)。
- [x] P1-6 (C1) ELF loader:重定位(R_ARM_ABS32/MOVW)+ backing 跟踪与退出回收
  - 实现(2026-08-20):--emit-relocs 静态重定位引擎 —— R_ARM_ABS32(bias 精确恢复
    addend:new = old + (S'-S_link))、R_ARM_MOVW/MOVT_ABS(A32)与 R_ARM_THM_MOVW/
    MOVT_ABS(T32,半字视图访问 + 实测编码位域 imm4/i/imm3/imm8)、R_ARM_V4BX 跳过;
    不支持类型/段外目标/未定义符号一律 PARAM。镜像含 SHT_REL → text 落 RAM 模式
    (RX 帧:AP_PRW_URO 无 XN,W^X 保持;entry = 帧基址 + 偏移;帧先以创建者名义
    分配含 TRANSFER 权,建任务后 cap_move_to 移交,退出自动回收)。XIP 模式
    (无重定位)行为不变。节表/symtab/REL 段全部镜像界内校验(溢出安全)。
    user_task_exit_handler 入口 R0→R1 转发(C 返回值 = 退出状态,修返回式 ELF
    应用 retval 丢失)。test_elf_rel_app(.data RAM link VMA + ABS32 + 显式
    #:lower16:/:upper16:)+ Test 4 五轮加载-执行-回收。板上修复三处:
    THM 半字索引(uint32_t[1] 误取下一字)、TRANSFER 权、exit R1 转发。
    验证:板上 3387/3387(elf 47/47,5 轮零 fault 零 cap 泄漏)+ CI 全矩阵绿。
- [x] P1-7 验收:两个用户"进程"同名全局变量互不可见 + ELF 万次加载无泄漏
  - 实现(2026-08-20):rel app 增隔离自检(iso_counter:双实例并发各 50 轮
    +2/yield,隔离正确终值恒 1100,共享则趋 1200 → verdict bit64);
    test_elf Test 5(双实例并发 join,双双 verdict 0)+ Test 6(万次
    load/start/join/delete,cap 池 128→128 每 2500 轮抽检 + 终检)。
    **soak 挖出并修复 M3 遗留微窗死锁**:cap_deferred_poll 的
    "spin_trylock 成功 → owner_cpu 发布"窗口内任务被抢占时,后来者见
    "锁被占 + owner=NONE" 永久自旋(UP 持有者永不回升,SysTick 饿死);
    修复 = trylock 与 owner 发布同置关中断窗口(本核再入必为真嵌套,
    他核持有才忙等)。零扰动 RAM 探针(openocd 不挂起读 tick/TCB/栈)
    定罪。验证:工作配置 3395/3395 + SMP 3430/3430 + CI 全矩阵 7/7,
    soak 万次零泄漏。

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
