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
#define BLOCK_DEVICE 1
#define BOARD_DISPLAY_NAME "Raspberry Pi Pico 2 W"
#define BOARD_NAME "rp2350"
#define CAP_ENABLE 1
#define CAP_MAX_COUNT (128)
#define CAP_RCU 1
#define CAP_RESTART_SUBSET 1
#define CONFIG_BOARD_RP2350 1
#define DEBUG_ENABLE 1
#define DEBUG_LEVEL (3)
#define DEBUG_STACK_CHECK 1
#define DRIVER_CYW43 1
#define DRIVER_ENABLE 1
#define DRIVER_GPIO_SERVER 1
#define DRIVER_I2C_SERVER 1
#define DRIVER_MAX_DEVICES (16)
#define DRIVER_RTC 1
#define DRIVER_SPI 1
#define DYNAMIC_LINKING 1
#define ELF_LOADER 1
#define FAULT_CRASH_DUMP 1
#define FAULT_ENABLE 1
#define FAULT_ENDPOINT 1
#define FB 1
#define FS_LITTLEFS 1
#define FS_PERSISTENT 1
#define FV 1
#define INIT_PROCESS 1
#define IPC_CHANNEL 1
#define IPC_CHANNEL_MAX (8)
#define IPC_CH_MSG_SIZE (128)
#define IPC_ENDPOINT 1
#define IPC_ENDPOINT_MAX (32)
#define IPC_EP_MAX_PENDING (8)
#define IPC_EP_MSG_SIZE (128)
#define IPC_EVENT 1
#define IPC_EVENT_MAX (8)
#define IPC_MQUEUE 1
#define IPC_MQUEUE_MAX (8)
#define IPC_MUTEX 1
#define IPC_MUTEX_MAX (16)
#define IPC_SEMAPHORE 1
#define IPC_SEMAPHORE_MAX (16)
#define IRQ_BH_ENABLE 1
#define IRQ_BH_MAX (16)
#define IRQ_DEFAULT_PRIORITY (8)
#define IRQ_ENABLE 1
#define IRQ_MAX_USER (32)
#define IRQ_THREADED_ENABLE 1
#define IRQ_THREADED_MAX (8)
#define IRQ_THREADED_STACK_SIZE (768)
#define KERNEL_IDLE_PRIORITY (31)
#define KERNEL_IDLE_SLEEP 1
#define KERNEL_IDLE_STACK_SIZE (512)
#define KERNEL_MAX_PRIORITIES (32)
#define KERNEL_MAX_TASKS (64)
#define KERNEL_TASK_STACK_SIZE (2048)
#define KERNEL_TICK_RATE (1000)
#define KERN_DEFAULT_TIME_SLICE (5)
#define KERN_NAME "My-RTOS"
#define KERN_TASK_CAP_SLOTS (32)
#define KERN_TASK_NAME_LEN (32)
#define KERN_TASK_STATS 1
#define KERN_VERSION_MAJOR (1)
#define KERN_VERSION_MINOR (0)
#define KERN_VERSION_PATCH (0)
#define MBEDTLS 1
#define MEM_DYNAMIC 1
#define MEM_HEAP_SIZE (16384)
#define MEM_POOL_COUNT (8)
#define MPU_ENABLE 1
#define MPU_REGION_COUNT (8)
#define MUTEX_DEADLOCK_DETECT 1
#define NET 1
#define NET_DHCP 1
#define NET_DNS 1
#define OTA 1
#define PANIC_LOG 1
#define PM 1
#define PROFILER 1
#define PROJECT_NAME "my-rtos"
#define PROJECT_VERSION "1.0.0"
#define RT_SCHED 1
#define SECURE_BOOT 1
#define SHELL_ENABLE 1
#define SHELL_PRIORITY (5)
#define SHELL_STACK_SIZE (4096)
#define SMP 1
#define SMP_MAX_CPUS (2)
#define SMP_WORK_STEALING 1
#define SUPERVISOR 1
#define SYSCALL_ENABLE 1
#define SYSCALL_TABLE_SIZE (128)
#define TEST_ENABLE 1
#define TEST_MODULE_BLOCK 1
#define TEST_MODULE_ALLOCATOR 1
#define TEST_MODULE_CAP 1
#define TEST_MODULE_ELF 1
// #define TEST_MODULE_EXAMPLE 0
#define TEST_MODULE_FAULT 1
#define TEST_MODULE_FS_STORE 1
#define TEST_MODULE_FS_DEVFS 1
#define TEST_MODULE_FS_FD_CLEANUP 1
#define TEST_MODULE_GPIO_DRIVER 1
#define TEST_MODULE_INIT_ORCHESTRATE 1
#define TEST_MODULE_IPC_UPGRADE 1
#define TEST_MODULE_MMIO 1
#define TEST_MODULE_NOTIFICATION 1
#define TEST_MODULE_RT_SCHED 1
#define TEST_MODULE_SCHEDULER 1
#define TEST_MODULE_SMP 1
#define TEST_MODULE_STATS 1
#define TEST_MODULE_VFS 1
#define TIMER_CMD_QUEUE_SIZE (16)
#define TIMER_ENABLE 1
#define TIMER_MAX (32)
#define TIMER_NAME_LEN (32)
#define TIMER_TASK_PRIORITY (1)
#define TIMER_TASK_STACK_SIZE (1024)
#define TRACE_BUFFER_SIZE (1024)
#define TRACE_ENABLE 1
#define USER_LIBC 1
#define VFS_ENABLE 0
#define VFS_MAX_FDS (16)
#define VFS_MAX_INODES (32)

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
#define KERN_MAX_ENDPOINTS        IPC_ENDPOINT_MAX
#define KERN_MAX_CHANNELS         IPC_CHANNEL_MAX
#define KERN_EP_MSG_SIZE          IPC_EP_MSG_SIZE
#define KERN_EP_MAX_PENDING       IPC_EP_MAX_PENDING
#define KERN_CH_MSG_SIZE          IPC_CH_MSG_SIZE
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
