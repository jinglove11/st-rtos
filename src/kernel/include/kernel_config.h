/**
 * @file kernel_config.h
 * @brief 内核配置（自动生成）
 *
 * 由 menuconfig 生成，请勿手动修改。
 */

#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

/* 自动生成的配置 */
#define ASSERT_ENABLE 1
#define BOARD_DISPLAY_NAME "Nucleo-F767ZI"
#define BOARD_NAME "stm32f767"
#define BOARD_STM32F767 1
#define CONFIG_BOARD_STM32F767 1
#define DEBUG_ENABLE 1
#define DEBUG_LEVEL (2)
#define DEBUG_STACK_CHECK 1
#define IPC_EVENT 1
#define IPC_EVENT_MAX (4)
#define IPC_MQUEUE 1
#define IPC_MQUEUE_MAX (4)
#define IPC_MUTEX 1
#define IPC_MUTEX_MAX (8)
#define IPC_SEMAPHORE 1
#define IPC_SEMAPHORE_MAX (8)
#define KERNEL_IDLE_PRIORITY (31)
#define KERNEL_IDLE_SLEEP 1
#define KERNEL_IDLE_STACK_SIZE (256)
#define KERNEL_MAX_PRIORITIES (32)
#define KERNEL_MAX_TASKS (16)
#define KERNEL_TASK_STACK_SIZE (1024)
#define KERNEL_TICK_RATE (1000)
// #define KERNEL_WATCHDOG 0
#define KERN_DEFAULT_TIME_SLICE (5)
#define KERN_NAME "My-RTOS"
#define KERN_TASK_NAME_LEN (16)
#define KERN_VERSION_MAJOR (1)
#define KERN_VERSION_MINOR (0)
#define KERN_VERSION_PATCH (0)
#define MEM_DYNAMIC 1
#define MEM_HEAP_SIZE (4096)
#define MEM_POOL_COUNT (4)
#define PROJECT_NAME "my-rtos"qs"
#define PROJECT_VERSION "1.0.0"
#define TEST_ENABLE 1
// #define TEST_MODULE_EXAMPLE 0
#define TEST_MODULE_SCHEDULER 1
#define TIMER_CMD_QUEUE_SIZE (8)
#define TIMER_ENABLE 1
#define TIMER_MAX (16)
#define TIMER_NAME_LEN (16)
#define TIMER_TASK_PRIORITY (1)
#define TIMER_TASK_STACK_SIZE (512)

/*============================================================================
 * 兼容性别名（旧宏名 -> 新宏名）
 *============================================================================*/

/* 内核配置 */
#define KERN_MAX_TASKS            KERNEL_MAX_TASKS
#define KERN_MAX_PRIORITY         KERNEL_MAX_PRIORITIES
#define KERN_TICK_RATE_HZ         KERNEL_TICK_RATE
#define KERN_TICK_US              (1000000UL / KERNEL_TICK_RATE)
#define KERN_DEFAULT_STACK_SIZE   KERNEL_TASK_STACK_SIZE
#define KERN_IDLE_STACK_SIZE      KERNEL_IDLE_STACK_SIZE
#define KERN_IDLE_PRIORITY        KERNEL_IDLE_PRIORITY
#define KERN_HEAP_SIZE            MEM_HEAP_SIZE

/* IPC 配置 */
#define KERN_MAX_SEMAPHORES       IPC_SEMAPHORE_MAX
#define KERN_MAX_MUTEXES          IPC_MUTEX_MAX
#define KERN_MAX_MQUEUES          IPC_MQUEUE_MAX
#define KERN_MAX_EVENTS           IPC_EVENT_MAX
#define KERN_MUTEX_PI             1  /* 优先级继承 */

/* 消息队列配置 */
#define KERN_MQUEUE_DEPTH         16
#define KERN_MSG_MAX_SIZE         64

/* 栈配置 */
#define KERN_STACK_ALIGN          8
#define STACK_MAGIC_BYTE          0xAB

/* 调试配置 */
#define KERN_DEBUG_ENABLE         DEBUG_ENABLE

/* 调度器配置 */
#define SCHED_CRITICAL_PRIORITY   2
#define PENDSV_PRIORITY           15
#define SYSTICK_PRIORITY          15

#endif /* KERNEL_CONFIG_H */
