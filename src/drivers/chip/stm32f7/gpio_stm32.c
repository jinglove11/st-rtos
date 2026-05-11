/**
 * @file gpio_stm32.c
 * @brief STM32 GPIO 驱动实现
 */

#include "gpio.h"
#include "stm32f767.h"

/*============================================================================
 * 内部函数
 *============================================================================*/

static void gpio_clock_enable(gpio_t *gpio) {
    uint32_t bit = 0;

    if (gpio == GPIOA)      bit = RCC_AHB1ENR_GPIOAEN;
    else if (gpio == GPIOB) bit = RCC_AHB1ENR_GPIOBEN;
    else if (gpio == GPIOC) bit = RCC_AHB1ENR_GPIOCEN;
    else if (gpio == GPIOD) bit = RCC_AHB1ENR_GPIODEN;
    else if (gpio == GPIOE) bit = RCC_AHB1ENR_GPIOEEN;
    else if (gpio == GPIOF) bit = RCC_AHB1ENR_GPIOFEN;
    else if (gpio == GPIOG) bit = RCC_AHB1ENR_GPIOGEN;
    else if (gpio == GPIOH) bit = RCC_AHB1ENR_GPIOHEN;
    else if (gpio == GPIOI) bit = RCC_AHB1ENR_GPIOIEN;

    RCC->AHB1ENR |= bit;
}

/*============================================================================
 * 公开函数
 *============================================================================*/

void gpio_init(gpio_t *gpio, uint32_t pin, gpio_dir_t dir) {
    gpio_clock_enable(gpio);

    // 配置模式
    gpio->MODER &= ~(3U << (pin * 2));
    if (dir == GPIO_DIR_OUTPUT) {
        gpio->MODER |= (1U << (pin * 2));   // 输出模式
    }
    // 输入模式默认为 0

    // 配置输出类型 (推挽)
    gpio->OTYPER &= ~(1U << pin);

    // 配置速度 (高速)
    gpio->OSPEEDR |= (2U << (pin * 2));

    // 无上拉下拉
    gpio->PUPDR &= ~(3U << (pin * 2));
}

void gpio_set(gpio_t *gpio, uint32_t pin, gpio_level_t level) {
    if (level) {
        gpio->BSRR = (1U << pin);           // 置位
    } else {
        gpio->BSRR = (1U << (pin + 16));    // 复位
    }
}

void gpio_clr(gpio_t *gpio, uint32_t pin) {
    gpio->BSRR = (1U << (pin + 16));
}

void gpio_toggle(gpio_t *gpio, uint32_t pin) {
    gpio->ODR ^= (1U << pin);
}

gpio_level_t gpio_get(gpio_t *gpio, uint32_t pin) {
    return (gpio->IDR & (1U << pin)) ? GPIO_HIGH : GPIO_LOW;
}

void gpio_set_af(gpio_t *gpio, uint32_t pin, uint32_t af) {
    gpio->AFR[pin / 8] |= (af << ((pin % 8) * 4));
}
