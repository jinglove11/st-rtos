/**
 * @file system_init.h
 * @brief 系统初始化接口
 *
 * 提供系统各阶段的初始化函数。
 */

#ifndef SYSTEM_INIT_H
#define SYSTEM_INIT_H

#include <stdint.h>

/**
 * @brief 系统初始化阶段
 *
 * 定义初始化的阶段顺序。
 */
typedef enum {
    INIT_PHASE_EARLY,       /**< 早期初始化（向量表、时钟） */
    INIT_PHASE_HARDWARE,    /**< 硬件初始化（UART、GPIO） */
    INIT_PHASE_KERNEL,      /**< 内核初始化（调度器、内存） */
    INIT_PHASE_DRIVER,      /**< 驱动初始化（外设驱动） */
    INIT_PHASE_APP,         /**< 应用初始化（用户应用） */
} init_phase_t;

/**
 * @brief 系统初始化配置
 */
typedef struct {
    uint32_t uart_baudrate;     /**< UART 波特率 */
    uint32_t systick_hz;        /**< SysTick 频率 */
} system_config_t;

/**
 * @brief 获取默认系统配置
 */
system_config_t system_get_default_config(void);

/**
 * @brief 初始化硬件
 *
 * 初始化 UART、GPIO 等基本硬件。
 *
 * @param config 系统配置
 */
void system_init_hardware(const system_config_t *config);

/**
 * @brief 初始化内核
 *
 * 初始化 RTOS 内核。
 */
void system_init_kernel(void);

/**
 * @brief 完整系统初始化
 *
 * 按顺序执行所有初始化阶段。
 *
 * @param config 系统配置（NULL 使用默认配置）
 */
void system_init(const system_config_t *config);

#endif /* SYSTEM_INIT_H */
