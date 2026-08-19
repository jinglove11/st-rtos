# 测试框架使用指南

## 概述

My-RTOS 测试框架是模块化的板上测试框架,测试按**契约稳定性分三层**:

| 层 | 目录 | 验证内容 | 改内核时的预期 |
|---|---|---|---|
| **K** | `src/tests/k/` | 内核不变量(白盒,内核上下文断言) | 红 = 重构成本,改完就好 |
| **ABI** | `src/tests/abi/` | syscall 契约(用户任务发 SVC,黑盒) | 红 = **真回归,必须查** |
| **SYS** | `src/tests/sys/` | 启动健康(init/supervisor 接线断言) | 红 = 内核与用户态各自都对、**接缝断了** |

层间执行顺序固定为 **K → ABI → (启动 init/supervisor) → SYS**,与构建系统链接顺序无关。每层独立小计(`[TIER K] modules N pass +X fail +Y`)。

分层的目的:让"改一点内核一片红"变成有信息量的信号——ABI 层保持绿说明内核内部可以放心重构;SYS 层专抓接线断裂(如 supervisor 订阅失败导致的静默睡死)。

## 文件结构

```
src/tests/
├── test_framework.h/.c   # 框架本体(tier 字段 + 三遍扫描)
├── test_example.c        # 模板(默认不编译)
├── k/                    # K 层:内核不变量(白盒)
├── abi/                  # ABI 层:syscall 契约(用户任务黑盒)
└── sys/                  # SYS 层:启动健康
```

## 快速开始

### 1. 创建测试模块(按层选择注册宏)

```c
#include "test_framework.h"

static void test_my_feature(void) {
    test_section("My Feature Test");
    TEST_ASSERT_EQ(1, my_func(), "my_func returns 1");
}

static void test_my_module(void) {
    test_my_feature();
}

TEST_K_MODULE(my_module, test_my_module);     /* 或 TEST_ABI_MODULE / TEST_SYS_MODULE */
```

`TEST_MODULE_REGISTER` 保留为 K 层的兼容别名,新代码不要用。

### 2. 启用测试模块

新文件放入对应层目录后:

- CMake(RP2350):各层目录已 GLOB,自动拾取
- Makefile(STM32):`TEST_SOURCES` 分层列表中加一行

### 3. 构建和运行

```bash
make                      # RP2350 (CMake)
make BOARD=stm32f767      # STM32F767
make flash                # 烧录后串口看输出
```

### 4. ABI 层用户任务用例助手

用户任务写不了内核计数器(内核 SRAM 不可达),断言必须回到内核上下文:

```c
/* 用户子任务内:聚合结果,一次回传 */
static void my_user_case(void *arg) {
    int ok = (sys_ep_create("t", 8, 2) > 0);
    sys_task_exit((void *)(intptr_t)(ok ? KERN_OK : KERN_ERR_STATE));
}

/* 内核侧:spawn + join + 断言retval */
test_user_case("user can create ep", my_user_case, KERN_OK, 2000);
```

多断言场景自行 `task_create_user` + `sys_task_exit` 回传 + `task_join` 后逐条 `TEST_ASSERT_EQ`(样板见 `abi/test_syscall_user.c`)。

## 运行时重跑(shell)

测试跑完进入 shell 后,可用 `test` 命令不重刷定位失败:

```
test k          # 重跑 K 层
test abi        # 重跑 ABI 层
test sys        # 重跑 SYS 层
test all        # 三层全跑
test <模块名>    # 重跑单个模块(如 test syscall_user)
```

注意:重跑发生在系统已运行 init/supervisor 之后,fault 类用例可能触发 supervisor 重启逻辑,属预期交互。

## 模块注册机制

注册宏把 `test_module_t { name, func, tier }` 放进链接段 `.test_modules`
(链接脚本 `KEEP` 收集),`test_run_tier()` 按层过滤线性遍历。层内顺序
= 链接顺序(不保证跨构建系统一致,不要依赖)。

## 分类规则

- 验证**内核内部不变量**的用例归 K(无论从哪个上下文调用;用户任务只当被测对象/负载发生器也算 K)
- 验证**用户任务经 SVC 可观察行为**的用例归 ABI(用户任务是 syscall 调用者)
- 验证**启动编排接线**(init/supervisor/服务拉起后的系统状态)归 SYS
- 混合模块按用例拆成两个文件(如 `k/test_syscall.c` + `abi/test_syscall_user.c`)

## 测试统计

`test_pass`/`test_fail` 全局计数,`[MODULE]` 行打印每模块增量与资源
(cap/mem/fault)快照,`[RESOURCE]` 行为泄漏警告(不 fail)。汇总在
`test_summary()`。
