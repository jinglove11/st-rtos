#include "gpio.h"

void gpio_set_func(uint32_t pin, uint32_t function) {
    if (pin < 30U) {
        io_bank0_hw->io[pin].ctrl = function;
    }
}

void gpio_init(uint32_t pin, gpio_dir_t direction) {
    if (pin >= 30U) {
        return;
    }
    gpio_set_func(pin, GPIO_FUNC_SIO);
    pads_bank0_hw->io[pin] |= PADS_IE;
    if (direction == GPIO_DIR_OUTPUT) {
        sio_hw->gpio_oe_set = 1U << pin;
    } else {
        sio_hw->gpio_oe_clr = 1U << pin;
    }
}

void gpio_set(uint32_t pin, gpio_level_t level) {
    if (pin >= 30U) {
        return;
    }
    if (level == GPIO_HIGH) {
        sio_hw->gpio_set = 1U << pin;
    } else {
        sio_hw->gpio_clr = 1U << pin;
    }
}

void gpio_clr(uint32_t pin) {
    gpio_set(pin, GPIO_LOW);
}

void gpio_toggle(uint32_t pin) {
    if (pin < 30U) {
        sio_hw->gpio_togl = 1U << pin;
    }
}

gpio_level_t gpio_get(uint32_t pin) {
    if (pin >= 30U) {
        return GPIO_LOW;
    }
    return (sio_hw->gpio_in & (1U << pin)) != 0U ? GPIO_HIGH : GPIO_LOW;
}

void gpio_pull_up(uint32_t pin, int enable) {
    if (pin < 30U) {
        uint32_t value = pads_bank0_hw->io[pin];
        value = enable ? ((value | PADS_PUE) & ~PADS_PDE) : (value & ~PADS_PUE);
        pads_bank0_hw->io[pin] = value;
    }
}

void gpio_pull_down(uint32_t pin, int enable) {
    if (pin < 30U) {
        uint32_t value = pads_bank0_hw->io[pin];
        value = enable ? ((value | PADS_PDE) & ~PADS_PUE) : (value & ~PADS_PDE);
        pads_bank0_hw->io[pin] = value;
    }
}

