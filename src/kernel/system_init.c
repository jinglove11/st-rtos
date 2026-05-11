/**
 * @file system_init.c
 * @brief 系统初始化实现
 *
 * 提供系统各阶段的初始化函数实现。
 *
 * ============================================================================
 * 初始化顺序
 * ============================================================================
 *
 * 1. 早期初始化 (EARLY)
 *    - 向量表重定位
 *    - 时钟配置（可选，当前使用 HSI）
 *
 * 2. 硬件初始化 (HARDWARE)
 *    - UART 初始化（调试输出）
 *    - GPIO 初始化（LED 等）
 *
 * 3. 内核初始化 (KERNEL)
 *    - 内核数据结构初始化
 *    - 调度器初始化
 *    - 内存管理初始化
 *
 * 4. 驱动初始化 (DRIVER)
 *    - 外设驱动初始化
 *
 * 5. 应用初始化 (APP)
 *    - 创建应用任务
 *    - 启动调度器
 */

#include "system_init.h"
#include "kernel.h"
#include "uart.h"
#include "gpio.h"
#include "board_config.h"
#include "hal.h"
#include "kernel_config.h"

#if DRIVER_ENABLE
extern void board_init_drivers(void);
#endif

/*============================================================================
 * 默认配置
 *============================================================================*/

/**
 * @brief 获取默认系统配置
 */
system_config_t system_get_default_config(void) {
    system_config_t config = {
        .uart_baudrate = NUCLEO_UART_BAUDRATE,
        .systick_hz    = 1000,  /* 1ms per tick */
    };
    return config;
}

/*============================================================================
 * 硬件初始化
 *============================================================================*/

/**
 * @brief 初始化硬件
 *
 * @param config 系统配置
 */
void system_init_hardware(const system_config_t *config) {
    system_config_t default_config;

    /* 使用默认配置 */
    if (config == NULL) {
        default_config = system_get_default_config();
        config = &default_config;
    }

    /* 初始化调试 UART */
    uart_init(NUCLEO_DEFAULT_UART, config->uart_baudrate);

    /* 初始化 LED GPIO */
    gpio_init(NUCLEO_LED_PORT, NUCLEO_LED_PIN, GPIO_DIR_OUTPUT);

    /* 初始化用户按钮（如果有） */
#ifdef NUCLEO_BUTTON_PORT
    gpio_init(NUCLEO_BUTTON_PORT, NUCLEO_BUTTON_PIN, GPIO_DIR_INPUT);
#endif
}

/*============================================================================
 * 内核初始化
 *============================================================================*/

/**
 * @brief 初始化内核
 */
void system_init_kernel(void) {
    /* 初始化 RTOS 内核 */
    kern_init();
}

/*============================================================================
 * 完整系统初始化
 *============================================================================*/

/**
 * @brief 完整系统初始化
 *
 * @param config 系统配置（NULL 使用默认配置）
 */
void system_init(const system_config_t *config) {
    system_config_t default_config;

    /* 使用默认配置 */
    if (config == NULL) {
        default_config = system_get_default_config();
        config = &default_config;
    }

    /* 阶段 1: 早期初始化 */
    /* HAL 初始化（向量表等）由启动代码完成 */

    /* 阶段 2: 硬件初始化 */
    system_init_hardware(config);

    /* 阶段 3: 内核初始化 */
    system_init_kernel();

    /* 阶段 4: 驱动初始化 */
#if DRIVER_ENABLE
    board_init_drivers();
#endif

    /* 阶段 5: 应用初始化 */
    /* 由应用代码完成 */
}
