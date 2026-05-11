# 测试框架使用指南

## 概述

My-RTOS 测试框架是一个模块化的单元测试框架，支持：

- **模块化注册**：每个测试模块独立注册，自动收集
- **断言宏**：提供多种断言宏简化测试编写
- **测试统计**：自动统计通过/失败数量
- **格式化输出**：支持数字、十六进制输出
- **职责分离**：初始化与测试分离

## 文件结构

```
src/
├── kernel/
│   ├── system_init.h    # 系统初始化接口
│   └── system_init.c    # 系统初始化实现
├── tests/
│   ├── test_framework.h # 测试框架接口
│   ├── test_framework.c # 测试框架实现
│   ├── test_scheduler.c # 调度器测试模块
│   └── test_example.c   # 示例测试模块（模板）
└── app/
    └── main.c           # 入口（调用测试框架）
```

## 快速开始

### 1. 创建测试模块

复制 `test_example.c` 作为模板：

```c
#include "test_framework.h"

/* 测试用例 */
static void test_my_feature(void) {
    test_section("My Feature Test");

    TEST_ASSERT(1 == 1, "Basic check");
    TEST_ASSERT_EQ(5, 5, "Equality check");
}

/* 模块入口 */
static void test_my_module(void) {
    test_my_feature();
}

/* 注册模块 */
TEST_MODULE_REGISTER(my_module, test_my_module);
```

### 2. 启用测试模块

在 `Makefile` 中添加测试文件：

```makefile
TEST_SOURCES += src/tests/test_my_module.c
```

### 3. 构建和运行

```bash
make BOARD=stm32f767
make flash
```

## 断言宏

| 宏 | 说明 |
|---|---|
| `TEST_ASSERT(cond, name)` | 断言条件为真 |
| `TEST_ASSERT_EQ(exp, act, name)` | 断言相等 |
| `TEST_ASSERT_NE(exp, act, name)` | 断言不相等 |
| `TEST_ASSERT_RANGE(val, min, max, name)` | 断言在范围内 |
| `TEST_ASSERT_NOT_NULL(ptr, name)` | 断言指针非空 |
| `TEST_ASSERT_NULL(ptr, name)` | 断言指针为空 |

## 输出函数

| 函数 | 说明 |
|---|---|
| `test_print(msg)` | 打印字符串 |
| `test_print_num(label, num)` | 打印数字 |
| `test_print_hex(label, num)` | 打印十六进制 |
| `test_section(name)` | 打印分节标题 |
| `test_pass(name)` | 打印通过 |
| `test_fail(name)` | 打印失败 |
| `test_summary()` | 打印测试摘要 |

## 模块注册机制

### 工作原理

1. `TEST_MODULE_REGISTER()` 宏将模块信息放入 `.test_modules` 段
2. 链接器将所有模块收集到连续内存区域
3. `test_run_all_modules()` 遍历执行所有模块

### 链接器配置

在链接脚本中添加：

```ld
.test_modules :
{
    . = ALIGN(4);
    __test_modules_start = .;
    KEEP(*(.test_modules))
    __test_modules_end = .;
    . = ALIGN(4);
} > FLASH
```

## 主函数

`main.c` 只需调用 `test_runner_start()`：

```c
#include "test_framework.h"

int main(void) {
    test_runner_start();
    while (1);
    return 0;
}
```

## 输出示例

```
****************************************
    My-RTOS Test Suite v2.0
****************************************

[Module] scheduler
----------------------------------------

========================================
  Test 1: Task Create & Schedule
========================================
[PASS] Create tasks with valid params
[PASS] Task name set correctly
[PASS] Task priority set correctly
...

========================================
         TEST SUMMARY
========================================
Passed: 50
Failed: 0
Total:  50
========================================
All tests PASSED!
```

## 添加新测试模块

1. 创建 `src/tests/test_xxx.c`
2. 实现测试函数
3. 使用 `TEST_MODULE_REGISTER()` 注册
4. 在 Makefile 中添加源文件
5. 构建并测试
