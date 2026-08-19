# 已知问题记录:SMP 偶发测试失败族(阻塞用户任务的唤醒竞态)

记录日期:2026-08-18 | 更新:2026-08-19 | 状态:**主体已解决**(根因修复+测试竞态修复),剩 2 项 smp 压测偶发

## 0. 根因与解决 (2026-08-19 更新)

**根因 (取证闭环)**:`syscall_cont_commit` 的 phase 与 state 转换非原子 —
旧顺序 [CAS phase ARMING→BLOCKED] → [写 state=BLOCKED]。跨核 waker 在
两步之间看到 phase==BLOCKED → complete 全套记账 → sched_wakeup 的
state CAS 撞上 RUNNING → 静默失败 (零扰动 [CASFAIL] 计数器实测
RUNNING 桶=1);任务随后自写 state=BLOCKED 并出队 → 带已完成
continuation 阻塞进虚空,唤醒永久丢失。现场特征 (4/4 确定性):
`state=BLOCKED phase=IDLE active=0 R0=已投递 不在任何队列`。

**修复**:commit 改为 [出队+state=BLOCKED (临界区)] → [phase CAS],
不变量 `phase==BLOCKED ⟹ state==BLOCKED`;lose 路径恢复 RUNNING +
重新入队。配套:sched_wakeup CAS 失败零扰动计数器 (冷路径单存储)。

**测试竞态修复**:sem/event/channel delete-wake 测试的 `task_delay(1)`
在 SMP 负载下不足以保证 waiter 先阻塞 → 删除先于 wait → waiter 的
SVC 命中已回收 cap 返回 PARAM(-2) 而非 NOEXIST(-9)。改为轮询等待
waiter 真正 BLOCKED 再删除。

**验证**:1M 压力 preset 3 轮 — mqueue sender 0 复现 (原 4/4 必现);
UP 真单核 3197/3197 满分 (commit 路径影响全部阻塞 syscall,零回归)。

## 1. 复现操作(用户实测)

1. `make flash` 烧板,套件自动跑完 → **2 个失败**
2. shell 输入 `reset`(`cmd_reset` → `hal_system_reset` → `watchdog_reboot`,**热复位路径**,非刷写复位)→ 套件重跑 → **4 个失败**

失败数随 boot 波动(1~4 个/轮,历次观测见 §3),同一测试偶红偶绿 —
**是 flaky 竞态族,热复位改变启动时序从而改变命中窗口,"reset 后变 4"不是确定性因果**。

## 2. 失败清单(2026-08-18 两轮实测)

### 第 1 轮(make flash 后,2 个)

| 模块 | 断言 | 现象 |
|---|---|---|
| smp | `task reuse core1 joined` | expected 0, actual **-9**(NOEXIST)|
| syscall_user | `mqueue sender joined OK` | expected 0, actual **-3**(TIMEOUT)|

### 第 2 轮(shell reset 后,4 个)

| 模块 | 断言 | 现象 |
|---|---|---|
| smp | `task reuse core1 joined` | expected 0, actual -9(与第 1 轮相同,**每轮必现**,最稳定的一个)|
| syscall_user | `sleepable sem wait returned noexist after delete` | actual 值被终端 80 列截断,**待捕获** |
| syscall_user | `sleepable event wait returned noexist after delete` | 同上 |
| syscall_user | `sleepable channel recv returned noexist after delete` | 同上 |

注:第 1 轮失败的 mqueue 用例第 2 轮反而通过 — 失败集合逐轮变化。

## 3. 历次观测汇总(跨 boot 波动)

| 来源 | boot 方式 | 失败 |
|---|---|---|
| 本记录第 1 轮 | make flash(刷写复位) | task-reuse -9、mqueue join -3 |
| 本记录第 2 轮 | shell reset(watchdog 热复位) | task-reuse -9、×3 delete-wake |
| 回归 tier2 轮 | 刷写复位 | task-reuse -9、randomized interleavings actual=5、mqueue join -3 |
| 回归 tfix 轮 | 刷写复位 | randomized interleavings actual=1(+汇总区 UART 花屏 1 条)|

共同特征:**全部属于"线程态内核上下文唤醒 SVC 阻塞的用户任务"这一族**
(sem/event/channel/mqueue 的 delete-wake、send-wake、join)。UP 构建同套
用例 100% 通过 → SMP 特有,指向跨核唤醒与派发/超时的窗口。

## 4. 已知测试解剖(sem 变体,Test 18)

```
waiter = 用户任务: sys_sem_wait(sem_cap, 1000 ticks)  ← 超时 1s
测试核任务:      task_delay(1) → sem_delete(sem)
期望:           waiter 退出码 = NOEXIST(-9)
join:           task_join(waiter, 1000)
```

关键事实:第 2 轮 3 个失败中 **join 断言通过、仅退出码断言失败** — 任务
正常结束了,但带回了错误码。waiter 自身超时(1s)与删除点(1 tick)相距
悬殊,合法 TIMEOUT 竞态几乎不可能;指向 delete-wake 的**结果码投递错误**
或 waiter 经由其他路径提前退出。

## 5. 嫌疑方向(按优先级)

1. **delete-wake 结果投递的跨核竞态**:`sem/event/channel delete` 在核 A
   (线程态)唤醒派发在核 B 的 waiter,continuation complete 的 result 写入
   与 waiter 恢复执行之间的窗口;
2. **task 退出记录/join 竞态**(task-reuse -9 每轮必现,可能是同一根因
   的最稳定表象:退出记录被槽位复用冲掉 → join 读到 NOEXIST);
3. affinity/steal 违反(此前已单独标记:`ping task 0 stayed on core0`)。

## 6. 剩余未决 (2026-08-19 二次更新)

**test 6 (task reuse -9) 已根除**: 根因为 task_exit_record_store 盲用
next++ 覆盖存活记录 — worker b 先退出后,对核继续产生 >256 次子任务
退出把 b 的记录冲掉 → join(b) NOEXIST。修复: store 优先复用 invalid
槽。修复后 ~15 轮 1M 压力零复现。

**test 7 (randomized interleavings) 大幅收敛**: 从每轮 100% 多项失败
降至约 0-10/1000 次 join (近 14 轮中 11 轮全零)。残余特征已钉死:
`del_join = KERN_ERR_PARAM(-2)`,join 路径面包屑 path=8 (阻塞后被
waker 以 -2 唤醒),del 本身 TERMINATED。属超窄时序窗口 (任何代码
变动即隐藏 — 探针效应),已在 sched_wakeup 布 RA 陷阱
(sched_wake_param_join_hits/ra,冷路径零扰动): 下次触发 RACEDBG
将直接打印投递者地址,addr2line 即可定罪。

**附带 (CI 工具,非运行问题)**: verify_pico2w_build.py 的 UF2 元数据
校验在 default/release (偶发 full) 上 flaky — picotool 解析镜像时把
"cap " 字符串误当指针 ("failed to read memory at 0x20706163")。
ELF 构建正常、板上运行正常,与 picotool 版本相关,待单独排查。

## 7. 附带观测(非失败,一并记录)

- smp Test 7 期间 `[WAITQ] remove ignored: task not in queue` 连刷数十条
  (每轮出现,疑为交错测试的良性双删保护触发,但属同一竞态族信号);
- gpio_driver 的 MMIO 读值热/冷复位不同(0x2000 vs 0x04022200),断言不
  涉及,仅记录;
- 套件汇总区在 SUMMARY+shell banner 并发打印时 DAPLink 串口会丢字节花屏
  (`Passeiled` 交错),判读结果请以 `[TIER ...]` 行为准。
