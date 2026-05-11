# 软件定时器设计文档

## 1. 设计目标

- **目的**：提供灵活的软件定时器机制，支持单次触发和周期触发
- **架构**：最小堆 + 命令队列 + 定时器服务任务
- **安全性**：回调在任务上下文执行，可调用阻塞 API
- **复杂度**：插入/删除 O(log n)，获取最近到期 O(1)

---

## 2. 架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                      用户 API 层                             │
│  timer_create() timer_start() timer_stop() timer_delete()   │
│  timer_reset() timer_change_period()                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      命令队列层                              │
│  - 接收来自任务/中断的定时器操作请求                          │
│  - 保证线程安全（任意上下文可调用）                           │
│  - 使用消息队列实现                                          │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   定时器服务任务                             │
│  - 从命令队列取出请求并执行                                  │
│  - 管理最小堆，处理定时器到期                                 │
│  - 执行回调函数（任务上下文，可阻塞）                         │
│  - 优先级可配置（menuconfig）                                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      最小堆层                                │
│  - 按 expire 时间组织活跃定时器                              │
│  - 插入/删除 O(log n)                                        │
│  - 获取最近到期 O(1)                                         │
│  - 堆大小由 menuconfig 配置                                  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 工作流程

```
用户任务调用 timer_start()
         │
         ▼
┌─────────────────────────┐
│ 构造命令消息             │
│ cmd = TIMER_CMD_START   │
│ timer_id = x            │
│ period = 100            │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│ 发送到命令队列           │
│ (消息队列，线程安全)      │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│ 定时器服务任务被唤醒     │
│ (从队列读取消息)         │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│ 执行命令                 │
│ - 插入最小堆             │
│ - 更新 expire 时间       │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│ 等待最近定时器到期       │
│ 或新命令到达             │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│ 定时器到期，执行回调     │
│ (任务上下文，可阻塞)      │
└─────────────────────────┘
```

---

## 3. 数据结构

### 3.1 定时器结构体

```c
/**
 * @brief 定时器状态
 */
typedef enum {
    TIMER_STATE_IDLE = 0,      // 空闲（未启动）
    TIMER_STATE_ACTIVE,        // 活跃（在堆中）
    TIMER_STATE_RUNNING,       // 回调执行中
    TIMER_STATE_DELETED        // 已删除
} timer_state_t;

/**
 * @brief 定时器控制块
 */
typedef struct {
    // --- 基本信息 ---
    char            name[KERN_TIMER_NAME_LEN];  // 定时器名称
    timer_id_t      id;                         // 定时器 ID
    timer_state_t   state;                      // 当前状态

    // --- 时间参数 ---
    uint32_t        period;                     // 周期（ticks），0 表示单次
    uint32_t        expire;                     // 到期时间（ticks）

    // --- 回调信息 ---
    timer_callback_t callback;                  // 回调函数
    void           *arg;                        // 回调参数

    // --- 堆索引 ---
    int             heap_index;                 // 在最小堆中的索引，-1 表示不在堆中

    // --- 标志 ---
    uint8_t         one_shot;                   // 单次触发标志
    uint8_t         in_use;                     // 使用标志
} timer_t;
```

### 3.2 最小堆结构

```c
/**
 * @brief 定时器最小堆
 */
typedef struct {
    timer_t        *timers[KERN_TIMER_MAX];     // 定时器指针数组
    int             size;                       // 当前大小
} timer_heap_t;
```

### 3.3 命令结构体

```c
/**
 * @brief 定时器命令类型
 */
typedef enum {
    TIMER_CMD_START,        // 启动定时器
    TIMER_CMD_STOP,         // 停止定时器
    TIMER_CMD_RESET,        // 重置定时器
    TIMER_CMD_CHANGE_PERIOD,// 修改周期
    TIMER_CMD_DELETE        // 删除定时器
} timer_cmd_type_t;

/**
 * @brief 定时器命令消息
 */
typedef struct {
    timer_cmd_type_t    type;       // 命令类型
    timer_id_t          timer_id;   // 目标定时器
    uint32_t            param;      // 参数（周期、超时等）
} timer_cmd_t;
```

---

## 4. 最小堆算法

### 4.1 堆的性质

- **最小堆**：父节点的 expire 值总是小于等于子节点
- **根节点**：expire 值最小的定时器（最近到期）
- **数组表示**：使用数组存储完全二叉树

### 4.2 索引关系

```
对于索引 i 的节点：
- 父节点索引：(i - 1) / 2
- 左子节点索引：2 * i + 1
- 右子节点索引：2 * i + 2
```

### 4.3 核心操作

#### 插入（上浮）

```c
/**
 * @brief 插入定时器到堆
 * @param heap 堆指针
 * @param timer 定时器指针
 * @return 0 成功，-1 堆已满
 */
static int heap_insert(timer_heap_t *heap, timer_t *timer) {
    if (heap->size >= KERN_TIMER_MAX) {
        return -1;  // 堆已满
    }

    // 插入到末尾
    int i = heap->size++;
    heap->timers[i] = timer;
    timer->heap_index = i;

    // 上浮调整
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (heap->timers[parent]->expire <= timer->expire) {
            break;  // 满足堆性质
        }
        // 交换
        heap->timers[i] = heap->timers[parent];
        heap->timers[i]->heap_index = i;
        heap->timers[parent] = timer;
        timer->heap_index = parent;
        i = parent;
    }

    return 0;
}
```

#### 删除最小值（下沉）

```c
/**
 * @brief 弹出堆顶定时器（expire 最小）
 * @param heap 堆指针
 * @return 堆顶定时器指针，NULL 表示堆为空
 */
static timer_t *heap_pop(timer_heap_t *heap) {
    if (heap->size == 0) {
        return NULL;
    }

    timer_t *min = heap->timers[0];
    min->heap_index = -1;

    // 用最后一个元素替换根
    timer_t *last = heap->timers[--heap->size];
    if (heap->size > 0) {
        heap->timers[0] = last;
        last->heap_index = 0;

        // 下沉调整
        int i = 0;
        while (1) {
            int smallest = i;
            int left = 2 * i + 1;
            int right = 2 * i + 2;

            if (left < heap->size &&
                heap->timers[left]->expire < heap->timers[smallest]->expire) {
                smallest = left;
            }
            if (right < heap->size &&
                heap->timers[right]->expire < heap->timers[smallest]->expire) {
                smallest = right;
            }

            if (smallest == i) {
                break;  // 满足堆性质
            }

            // 交换
            heap->timers[i] = heap->timers[smallest];
            heap->timers[i]->heap_index = i;
            heap->timers[smallest] = last;
            last->heap_index = smallest;
            i = smallest;
        }
    }

    return min;
}
```

#### 删除任意节点

```c
/**
 * @brief 从堆中移除指定定时器
 * @param heap 堆指针
 * @param timer 定时器指针
 */
static void heap_remove(timer_heap_t *heap, timer_t *timer) {
    int i = timer->heap_index;
    if (i < 0 || i >= heap->size) {
        return;  // 不在堆中
    }

    timer->heap_index = -1;

    // 用最后一个元素替换
    timer_t *last = heap->timers[--heap->size];
    if (heap->size > 0 && i < heap->size) {
        heap->timers[i] = last;
        last->heap_index = i;

        // 可能需要上浮或下沉
        // 先尝试上浮
        int parent = (i - 1) / 2;
        if (i > 0 && heap->timers[parent]->expire > last->expire) {
            // 上浮
            while (i > 0) {
                parent = (i - 1) / 2;
                if (heap->timers[parent]->expire <= last->expire) {
                    break;
                }
                heap->timers[i] = heap->timers[parent];
                heap->timers[i]->heap_index = i;
                heap->timers[parent] = last;
                last->heap_index = parent;
                i = parent;
            }
        } else {
            // 下沉
            while (1) {
                int smallest = i;
                int left = 2 * i + 1;
                int right = 2 * i + 2;

                if (left < heap->size &&
                    heap->timers[left]->expire < heap->timers[smallest]->expire) {
                    smallest = left;
                }
                if (right < heap->size &&
                    heap->timers[right]->expire < heap->timers[smallest]->expire) {
                    smallest = right;
                }

                if (smallest == i) {
                    break;
                }

                heap->timers[i] = heap->timers[smallest];
                heap->timers[i]->heap_index = i;
                heap->timers[smallest] = last;
                last->heap_index = smallest;
                i = smallest;
            }
        }
    }
}
```

---

## 5. 定时器服务任务

### 5.1 任务职责

1. 从命令队列读取命令并执行
2. 等待最近定时器到期
3. 执行到期定时器的回调函数
4. 处理周期定时器的重新插入

### 5.2 任务实现

```c
/**
 * @brief 定时器服务任务函数
 */
static void timer_service_task(void *arg) {
    (void)arg;
    timer_cmd_t cmd;
    uint32_t timeout;

    while (1) {
        // 计算等待时间
        if (timer_heap.size > 0) {
            uint32_t now = sched_get_tick_count();
            uint32_t expire = timer_heap.timers[0]->expire;
            if (expire > now) {
                timeout = expire - now;
            } else {
                timeout = 0;  // 已到期
            }
        } else {
            timeout = 0xFFFFFFFF;  // 无限等待
        }

        // 等待命令或超时
        kern_err_t err = mqueue_receive(cmd_queue, &cmd, timeout);

        if (err == KERN_OK) {
            // 处理命令
            process_command(&cmd);
        } else if (err == KERN_ERR_TIMEOUT) {
            // 定时器到期，处理到期定时器
            process_expired_timers();
        }
    }
}

/**
 * @brief 处理到期的定时器
 */
static void process_expired_timers(void) {
    uint32_t now = sched_get_tick_count();

    while (timer_heap.size > 0) {
        timer_t *timer = timer_heap.timers[0];

        if (timer->expire > now) {
            break;  // 最近的定时器还未到期
        }

        // 从堆中移除
        heap_pop(&timer_heap);
        timer->state = TIMER_STATE_RUNNING;

        // 执行回调
        if (timer->callback) {
            timer->callback(timer->arg);
        }

        // 周期定时器重新插入
        if (!timer->one_shot && timer->state != TIMER_STATE_DELETED) {
            timer->expire = now + timer->period;
            timer->state = TIMER_STATE_ACTIVE;
            heap_insert(&timer_heap, timer);
        } else {
            timer->state = TIMER_STATE_IDLE;
        }
    }
}
```

---

## 6. API 设计

### 6.1 用户 API

```c
/**
 * @brief 创建定时器
 * @param name     定时器名称
 * @param callback 回调函数
 * @param arg      回调参数
 * @param period   周期（ticks），0 表示单次
 * @return 定时器 ID，失败返回 KERN_INVALID_ID
 */
timer_id_t timer_create(const char *name, timer_callback_t callback,
                        void *arg, uint32_t period);

/**
 * @brief 启动定时器
 * @param timer_id 定时器 ID
 * @param delay    首次触发的延迟（ticks），0 表示立即开始
 * @return KERN_OK 成功，其他失败
 */
kern_err_t timer_start(timer_id_t timer_id, uint32_t delay);

/**
 * @brief 停止定时器
 * @param timer_id 定时器 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t timer_stop(timer_id_t timer_id);

/**
 * @brief 重置定时器（重新开始计时）
 * @param timer_id 定时器 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t timer_reset(timer_id_t timer_id);

/**
 * @brief 修改定时器周期
 * @param timer_id 定时器 ID
 * @param new_period 新周期（ticks）
 * @return KERN_OK 成功，其他失败
 */
kern_err_t timer_change_period(timer_id_t timer_id, uint32_t new_period);

/**
 * @brief 删除定时器
 * @param timer_id 定时器 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t timer_delete(timer_id_t timer_id);

/**
 * @brief 获取定时器状态
 * @param timer_id 定时器 ID
 * @return 定时器状态
 */
timer_state_t timer_get_state(timer_id_t timer_id);

/**
 * @brief 获取定时器剩余时间
 * @param timer_id 定时器 ID
 * @return 剩余 ticks，-1 表示失败
 */
int32_t timer_get_remaining(timer_id_t timer_id);
```

### 6.2 初始化 API

```c
/**
 * @brief 初始化定时器模块
 * @note 由 kern_init() 内部调用
 */
void timer_init(void);

/**
 * @brief 启动定时器服务
 * @note 由 kern_start() 内部调用
 */
void timer_service_start(void);
```

---

## 7. 配置选项

### 7.1 Kconfig 新增选项

```kconfig
menu "Timer Configuration"

config TIMER_ENABLE
    bool "Enable software timer"
    default y
    help
      启用软件定时器支持。

config TIMER_MAX
    int "Maximum timers"
    depends on TIMER_ENABLE
    default 16
    range 4 64
    help
      最大定时器数量。

config TIMER_CMD_QUEUE_SIZE
    int "Timer command queue size"
    depends on TIMER_ENABLE
    default 8
    range 4 32
    help
      定时器命令队列大小。

config TIMER_TASK_PRIORITY
    int "Timer service task priority"
    depends on TIMER_ENABLE
    default 1
    range 0 31
    help
      定时器服务任务优先级。
      数值越小优先级越高。
      建议设为较高优先级以确保定时器响应及时。

config TIMER_TASK_STACK_SIZE
    int "Timer service task stack size"
    depends on TIMER_ENABLE
    default 512
    range 256 2048
    help
      定时器服务任务栈大小（字节）。

config TIMER_NAME_LEN
    int "Timer name length"
    depends on TIMER_ENABLE
    default 16
    range 8 32
    help
      定时器名称最大长度。

endmenu
```

### 7.2 生成的宏定义

```c
/* 定时器配置 */
#define TIMER_ENABLE            1
#define KERN_TIMER_MAX          16
#define KERN_TIMER_CMD_QUEUE    8
#define KERN_TIMER_TASK_PRIO    1
#define KERN_TIMER_STACK_SIZE   512
#define KERN_TIMER_NAME_LEN     16
```

---

## 8. 文件结构

```
src/kernel/
├── timer/
│   ├── timer.h            # 定时器接口定义
│   └── timer.c            # 定时器实现
└── include/
    └── kernel_types.h     # 已有 timer_t 定义
```

---

## 9. 实现步骤

### 第一阶段：基础框架

| Step | 内容 | 文件 |
|------|------|------|
| 1 | 添加 Kconfig 配置选项 | Kconfig |
| 2 | 更新 kernel_types.h 中的 timer_t | kernel_types.h |
| 3 | 创建 timer.h 接口定义 | timer.h |

### 第二阶段：最小堆实现

| Step | 内容 | 文件 |
|------|------|------|
| 4 | 实现堆插入、删除、弹出 | timer.c |
| 5 | 编写堆操作单元测试 | test_timer.c |

### 第三阶段：命令队列与服务任务

| Step | 内容 | 文件 |
|------|------|------|
| 6 | 实现命令发送函数 | timer.c |
| 7 | 实现定时器服务任务 | timer.c |
| 8 | 实现命令处理函数 | timer.c |

### 第四阶段：用户 API

| Step | 内容 | 文件 |
|------|------|------|
| 9 | 实现 timer_create/delete | timer.c |
| 10 | 实现 timer_start/stop/reset | timer.c |
| 11 | 实现 timer_change_period | timer.c |

### 第五阶段：集成与测试

| Step | 内容 | 文件 |
|------|------|------|
| 12 | 集成到 kern_init/kern_start | kernel.c |
| 13 | 编写完整测试用例 | test_timer.c |

---

## 10. 测试用例

### 10.1 基础测试

```c
// 测试 1：单次定时器
void test_one_shot_timer(void) {
    static int flag = 0;

    void callback(void *arg) {
        flag = 1;
    }

    timer_id_t tid = timer_create("test1", callback, NULL, 0);
    TEST_ASSERT(tid >= 0);

    timer_start(tid, 10);  // 10 ticks 后触发
    task_delay(15);

    TEST_ASSERT(flag == 1);
    timer_delete(tid);
}

// 测试 2：周期定时器
void test_periodic_timer(void) {
    static int count = 0;

    void callback(void *arg) {
        count++;
    }

    timer_id_t tid = timer_create("test2", callback, NULL, 5);  // 周期 5 ticks
    TEST_ASSERT(tid >= 0);

    timer_start(tid, 0);
    task_delay(20);

    TEST_ASSERT(count >= 3);  // 至少触发 3 次

    timer_stop(tid);
    timer_delete(tid);
}

// 测试 3：定时器停止
void test_timer_stop(void) {
    static int count = 0;

    void callback(void *arg) {
        count++;
    }

    timer_id_t tid = timer_create("test3", callback, NULL, 5);
    timer_start(tid, 0);

    task_delay(12);
    timer_stop(tid);

    int count_before = count;
    task_delay(20);

    TEST_ASSERT(count == count_before);  // 停止后不再触发
    timer_delete(tid);
}
```

### 10.2 压力测试

```c
// 测试 4：多定时器并发
void test_multiple_timers(void) {
    #define TIMER_COUNT 8
    static int flags[TIMER_COUNT] = {0};

    void callback(void *arg) {
        int idx = (int)arg;
        flags[idx]++;
    }

    timer_id_t timers[TIMER_COUNT];

    // 创建并启动多个定时器
    for (int i = 0; i < TIMER_COUNT; i++) {
        timers[i] = timer_create("multi", callback, (void *)i, 10);
        timer_start(timers[i], i * 5);  // 错开启动时间
    }

    task_delay(100);

    // 验证每个定时器都触发了
    for (int i = 0; i < TIMER_COUNT; i++) {
        TEST_ASSERT(flags[i] > 0);
        timer_delete(timers[i]);
    }
}

// 测试 5：从中断启动定时器
void test_timer_from_isr(void) {
    static int flag = 0;

    void callback(void *arg) {
        flag = 1;
    }

    void isr_callback(void) {
        timer_id_t tid = timer_create("isr_test", callback, NULL, 0);
        timer_start(tid, 5);
    }

    // 模拟中断触发
    isr_callback();

    task_delay(10);
    TEST_ASSERT(flag == 1);
}
```

---

## 11. 注意事项

### 11.1 回调函数限制

虽然回调在任务上下文执行，但仍需注意：

1. **避免长时间阻塞**：回调中不要调用长时间阻塞的 API
2. **避免死锁**：注意回调中获取锁的顺序
3. **重入问题**：周期定时器的回调可能在上一次未完成时再次触发

### 11.2 性能考虑

1. **堆操作复杂度**：O(log n)，适合定时器数量 < 64 的场景
2. **命令队列大小**：根据实际并发需求配置
3. **服务任务优先级**：设为较高优先级确保及时响应

### 11.3 内存使用

| 项目 | 大小 |
|------|------|
| 定时器池 | `sizeof(timer_t) * KERN_TIMER_MAX` |
| 命令队列 | `sizeof(timer_cmd_t) * KERN_TIMER_CMD_QUEUE` |
| 服务任务栈 | `KERN_TIMER_STACK_SIZE` |
| 堆指针数组 | `sizeof(timer_t*) * KERN_TIMER_MAX` |

---

## 12. 与现有模块的关系

```
┌─────────────────────────────────────────────────────────────┐
│                       kernel.c                               │
│  kern_init() → timer_init()                                 │
│  kern_start() → timer_service_start()                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                       timer.c                                │
│  依赖：scheduler.h, mqueue.h, kernel_config.h               │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │scheduler │   │ mqueue   │   │   hal    │
        └──────────┘   └──────────┘   └──────────┘
```

---

## 13. 扩展方向

1. **定时器组**：支持多个定时器同步启停
2. **高精度定时器**：使用硬件定时器实现微秒级精度
3. **定时器统计**：记录触发次数、最大延迟等
4. **看门狗集成**：定时器超时自动喂狗
