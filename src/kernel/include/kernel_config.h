/**
 * @file kernel_config.h
 * @brief 内核配置 - 微内核架构 v1.0
 *
 * 所有内核对象静态分配，配置在编译时确定
 */

#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

/*============================================================================
 * 基础配置
 *============================================================================*/

// 内核版本
#define KERN_VERSION_MAJOR      0
#define KERN_VERSION_MINOR      1
#define KERN_VERSION_PATCH      0

// 内核名称
#define KERN_NAME               "My-RTOS Microkernel"

/*============================================================================
 * 任务配置
 *============================================================================*/

// 最大任务数 (静态分配)
#define KERN_MAX_TASKS          4

// 优先级数量 (0 最高, KERN_MAX_PRIORITY-1 最低)
#define KERN_MAX_PRIORITY       128

// 空闲任务优先级
#define KERN_IDLE_PRIORITY      (KERN_MAX_PRIORITY - 1)

// 默认任务栈大小 (字节)
#define KERN_DEFAULT_STACK_SIZE 1024

// 空闲任务栈大小 (字节)
#define KERN_IDLE_STACK_SIZE    256

// 任务名称最大长度
#define KERN_TASK_NAME_LEN      16

/*============================================================================
 * 调度配置
 *============================================================================*/

// 使能抢占式调度
#define KERN_PREEMPTIVE         1

// 使能时间片轮转
#define KERN_TIME_SLICE         1

// 默认时间片 (ticks)
#define KERN_DEFAULT_TIME_SLICE 100

// 使能 SMP 支持
#define KERN_SMP_ENABLE         0

// CPU 核心数 (SMP)
#define KERN_CPU_COUNT          1

/*============================================================================
 * 定时器配置
 *============================================================================*/

// 系统滴答频率 (Hz)
// 常用值: 100(10ms), 1000(1ms), 10000(100us)
#define KERN_TICK_RATE_HZ       1000

// 计算每 tick 微秒数
#define KERN_TICK_US            (1000000UL / KERN_TICK_RATE_HZ)

// 软件定时器最大数量
#define KERN_MAX_TIMERS         8

// 定时器任务栈大小
#define KERN_TIMER_STACK_SIZE   512

/*============================================================================
 * 内存管理配置
 *============================================================================*/

// 使能动态内存管理
#define KERN_ENABLE_HEAP        1

// 堆大小 (字节)
#define KERN_HEAP_SIZE          (32 * 1024)

// 使能内存池
#define KERN_ENABLE_MEMPOOL     1

// 最大内存池数量
#define KERN_MAX_MEMPOOLS       8

/*============================================================================
 * IPC 配置
 *============================================================================*/

// 使能消息队列
#define KERN_ENABLE_MQUEUE      1

// 最大消息队列数量
#define KERN_MAX_MQUEUES        8

// 消息队列默认深度
#define KERN_MQUEUE_DEPTH       8

// 单条消息最大大小 (字节)
#define KERN_MSG_MAX_SIZE       64

// 使能信号量
#define KERN_ENABLE_SEMAPHORE   1

// 最大信号量数量
#define KERN_MAX_SEMAPHORES     16

// 使能互斥锁
#define KERN_ENABLE_MUTEX       1

// 最大互斥锁数量
#define KERN_MAX_MUTEXES        8

// 使能优先级继承 (互斥锁)
#define KERN_MUTEX_PI           1

// 使能事件标志组
#define KERN_ENABLE_EVENT       1

// 最大事件标志组数量
#define KERN_MAX_EVENTS         8

// 使能条件变量
#define KERN_ENABLE_CONDVAR     0

// 最大条件变量数量
#define KERN_MAX_CONDVARS       4

/*============================================================================
 * 内存配置
 *============================================================================*/

// 使能内存池
#define KERN_ENABLE_MEMPOOL     1

// 内存池块大小配置
#define KERN_MEMPOOL_SMALL      32
#define KERN_MEMPOOL_MEDIUM     128
#define KERN_MEMPOOL_LARGE      512

// 每种块的数量
#define KERN_MEMPOOL_SMALL_COUNT    32
#define KERN_MEMPOOL_MEDIUM_COUNT   16
#define KERN_MEMPOOL_LARGE_COUNT    8

/*============================================================================
 * 中断配置
 *============================================================================*/

// 使能中断线程化
#define KERN_IRQ_THREADED       1

// 中断线程栈大小
#define KERN_IRQ_THREAD_STACK   512

// 底半部队列深度
#define KERN_BH_QUEUE_DEPTH     16

// 使能中断嵌套
#define KERN_IRQ_NESTING        1

// 最大嵌套深度
#define KERN_IRQ_NEST_DEPTH     4

/*============================================================================
 * 能力模型配置
 *============================================================================*/

// 使能能力模型
#define KERN_ENABLE_CAPABILITY  1

// 能力令牌位数
#define KERN_CAP_TOKEN_BITS     16

// 能力类型
#define KERN_CAP_IPC            (1 << 0)    // IPC 能力
#define KERN_CAP_MEMORY         (1 << 1)    // 内存访问能力
#define KERN_CAP_IRQ            (1 << 2)    // 中断能力
#define KERN_CAP_HW             (1 << 3)    // 硬件访问能力

/*============================================================================
 * 调试配置
 *============================================================================*/

// 使能调试日志
#define KERN_DEBUG_ENABLE       1

// 日志级别: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=trace
#define KERN_LOG_LEVEL          3

// 使能断言
#define KERN_ASSERT_ENABLE      1

// 使能性能计数器
#define KERN_PERF_COUNTER       1

// 使能栈溢出检测
#define KERN_STACK_CHECK        1

// 使能任务统计
#define KERN_TASK_STATS         1

/*============================================================================
 * 电源管理配置
 *============================================================================*/

// 使能空闲休眠
#define KERN_IDLE_SLEEP         1

// 使能动态调频 (需要硬件支持)
#define KERN_DVFS_ENABLE        0

// 低功耗模式阈值 (无任务运行的 tick 数)
#define KERN_IDLE_THRESHOLD     10

/*============================================================================
 * 容错配置
 *============================================================================*/

// 使能看门狗
#define KERN_WATCHDOG_ENABLE    1

// 看门狗超时 (ms)
#define KERN_WATCHDOG_TIMEOUT   5000

// 使能进程监控
#define KERN_PROC_MONITOR       1

// 任务心跳超时 (ticks)
#define KERN_HEARTBEAT_TIMEOUT  1000

/*============================================================================
 * 移植配置
 *============================================================================*/

// 使能 FPU (浮点单元)
#define KERN_FPU_ENABLE         0

// 使能 MPU (内存保护单元)
#define KERN_MPU_ENABLE         0

// 栈对齐要求 (字节)
#define KERN_STACK_ALIGN        8

/*============================================================================
 * 验证配置合法性
 *============================================================================*/

#if KERN_MAX_PRIORITY < 2
#error "KERN_MAX_PRIORITY must be at least 2"
#endif

#if KERN_TICK_RATE_HZ < 1
#error "KERN_TICK_RATE_HZ must be at least 1"
#endif

#if KERN_MAX_TASKS < 1
#error "KERN_MAX_TASKS must be at least 1"
#endif

#endif // KERNEL_CONFIG_H
