/**
 * @file gpio.c
 * @brief GPIO 驱动实现
 */

#include "gpio.h"

/*============================================================================
 * 公开函数
 *============================================================================*/

void gpio_init(uint32_t pin, gpio_dir_t dir) {
    if (pin < 30) {
        // 低位 GPIO (0-29)
        if (dir == GPIO_DIR_OUTPUT) {
            SIO->gpio_oe_set = (1U << pin);
        } else {
            SIO->gpio_oe_clr = (1U << pin);
        }
    } else if (pin < 36) {
        // 高位 GPIO (30-35)
        uint32_t shift = pin - 30;
        if (dir == GPIO_DIR_OUTPUT) {
            SIO->gpio_hi_oe_set = (1U << shift);
        } else {
            SIO->gpio_hi_oe_clr = (1U << shift);
        }
    }

    // 设置为 SIO 功能
    gpio_set_func(pin, GPIO_FUNC_SIO);

    // 使能输入
    if (pin < 30) {
        PADS_BANK0->io[pin] |= PADS_IE;
    } else if (pin < 36) {
        PADS_BANK0->io_hi[pin - 30] |= PADS_IE;
    }
}

void gpio_set(uint32_t pin, gpio_level_t level) {
    if (pin < 30) {
        if (level) {
            SIO->gpio_out_set = (1U << pin);
        } else {
            SIO->gpio_out_clr = (1U << pin);
        }
    } else if (pin < 36) {
        uint32_t shift = pin - 30;
        if (level) {
            SIO->gpio_hi_out_set = (1U << shift);
        } else {
            SIO->gpio_hi_out_clr = (1U << shift);
        }
    }
}

void gpio_clr(uint32_t pin) {
    gpio_set(pin, GPIO_LOW);
}

void gpio_toggle(uint32_t pin) {
    if (pin < 30) {
        SIO->gpio_out_xor = (1U << pin);
    } else if (pin < 36) {
        SIO->gpio_hi_out_xor = (1U << (pin - 30));
    }
}

gpio_level_t gpio_get(uint32_t pin) {
    if (pin < 30) {
        return (SIO->gpio_in >> pin) & 1;
    } else if (pin < 36) {
        return (SIO->gpio_hi_in >> (pin - 30)) & 1;
    }
    return GPIO_LOW;
}

void gpio_set_func(uint32_t pin, uint32_t func) {
    if (pin < 30) {
        IO_BANK0->io[pin].ctrl = func;
    } else if (pin < 36) {
        IO_BANK0->io_hi[pin - 30].ctrl = func;
    }
}

void gpio_pull_up(uint32_t pin, int enable) {
    if (pin < 30) {
        if (enable) {
            PADS_BANK0->io[pin] |= PADS_PUE;
            PADS_BANK0->io[pin] &= ~PADS_PDE;
        } else {
            PADS_BANK0->io[pin] &= ~PADS_PUE;
        }
    } else if (pin < 36) {
        volatile uint32_t *pad = &PADS_BANK0->io_hi[pin - 30];
        if (enable) {
            *pad |= PADS_PUE;
            *pad &= ~PADS_PDE;
        } else {
            *pad &= ~PADS_PUE;
        }
    }
}

void gpio_pull_down(uint32_t pin, int enable) {
    if (pin < 30) {
        if (enable) {
            PADS_BANK0->io[pin] |= PADS_PDE;
            PADS_BANK0->io[pin] &= ~PADS_PUE;
        } else {
            PADS_BANK0->io[pin] &= ~PADS_PDE;
        }
    } else if (pin < 36) {
        volatile uint32_t *pad = &PADS_BANK0->io_hi[pin - 30];
        if (enable) {
            *pad |= PADS_PDE;
            *pad &= ~PADS_PUE;
        } else {
            *pad &= ~PADS_PDE;
        }
    }
}
