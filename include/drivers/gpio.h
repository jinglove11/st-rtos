/**
 * @file gpio.h
 * @brief GPIO 驱动头文件 (通用接口)
 */

#ifndef GPIO_H
#define GPIO_H

#include "board_config.h"

#define gpio_init(...)       rtos_gpio_init(__VA_ARGS__)
#define gpio_set(...)        rtos_gpio_set(__VA_ARGS__)
#define gpio_clr(...)        rtos_gpio_clr(__VA_ARGS__)
#define gpio_toggle(...)     rtos_gpio_toggle(__VA_ARGS__)
#define gpio_get(...)        rtos_gpio_get(__VA_ARGS__)
#define gpio_set_func(...)   rtos_gpio_set_func(__VA_ARGS__)
#define gpio_pull_up(...)    rtos_gpio_pull_up(__VA_ARGS__)
#define gpio_pull_down(...)  rtos_gpio_pull_down(__VA_ARGS__)
#define gpio_set_af(...)     rtos_gpio_set_af(__VA_ARGS__)

/*============================================================================
 * 平台相关类型定义
 *============================================================================*/

#if (TARGET_BOARD == BOARD_RP2350_PICO2)

// RP2350 使用简单 API
typedef enum {
    GPIO_DIR_INPUT = 0,
    GPIO_DIR_OUTPUT = 1
} gpio_dir_t;

typedef enum {
    GPIO_LOW = 0,
    GPIO_HIGH = 1
} gpio_level_t;

void gpio_init(uint32_t pin, gpio_dir_t dir);
void gpio_set(uint32_t pin, gpio_level_t level);
void gpio_clr(uint32_t pin);
void gpio_toggle(uint32_t pin);
gpio_level_t gpio_get(uint32_t pin);
void gpio_set_func(uint32_t pin, uint32_t func);
void gpio_pull_up(uint32_t pin, int enable);
void gpio_pull_down(uint32_t pin, int enable);

#elif (TARGET_BOARD == BOARD_STM32F767_NUCLEO)

// STM32 使用 GPIO 端口 + 引脚 API
typedef enum {
    GPIO_DIR_INPUT = 0,
    GPIO_DIR_OUTPUT = 1
} gpio_dir_t;

typedef enum {
    GPIO_LOW = 0,
    GPIO_HIGH = 1
} gpio_level_t;

void gpio_init(gpio_t *gpio, uint32_t pin, gpio_dir_t dir);
void gpio_set(gpio_t *gpio, uint32_t pin, gpio_level_t level);
void gpio_clr(gpio_t *gpio, uint32_t pin);
void gpio_toggle(gpio_t *gpio, uint32_t pin);
gpio_level_t gpio_get(gpio_t *gpio, uint32_t pin);
void gpio_set_af(gpio_t *gpio, uint32_t pin, uint32_t af);

#endif

#endif // GPIO_H
