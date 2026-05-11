/**
 * @file nucleo_f767.h
 * @brief STM32 Nucleo-F767ZI 开发板定义
 */

#ifndef NUCLEO_F767_H
#define NUCLEO_F767_H

#include "stm32f767.h"

/*============================================================================
 * LED 配置 (Nucleo-F767ZI)
 *============================================================================*/

#define NUCLEO_LED1_PIN     0           // PB0 (绿 LED)
#define NUCLEO_LED1_PORT    GPIOB
#define NUCLEO_LED2_PIN     7           // PB7 (蓝 LED)
#define NUCLEO_LED2_PORT    GPIOB
#define NUCLEO_LED3_PIN     14          // PB14 (红 LED)
#define NUCLEO_LED3_PORT    GPIOB

// 默认使用 LED1
#define NUCLEO_LED_PIN      NUCLEO_LED1_PIN
#define NUCLEO_LED_PORT     NUCLEO_LED1_PORT

/*============================================================================
 * UART 配置 (Nucleo-F767ZI 默认使用 USART3 连接 ST-Link VCP)
 *   PD8  - USART3_TX
 *   PD9  - USART3_RX
 *============================================================================*/

#define NUCLEO_DEFAULT_UART     USART3
#define NUCLEO_UART_TX_PORT     GPIOD
#define NUCLEO_UART_TX_PIN      8
#define NUCLEO_UART_RX_PORT     GPIOD
#define NUCLEO_UART_RX_PIN      9
#define NUCLEO_UART_AF          7           // AF7 for USART3
#define NUCLEO_UART_BAUDRATE    115200

// 用户按键
#define NUCLEO_USER_BTN_PIN     13          // PC13
#define NUCLEO_USER_BTN_PORT    GPIOC

/*============================================================================
 * 时钟配置
 *
 * HSI 16MHz → PLL (M=16, N=192, P=4) → SYSCLK 48MHz
 * HCLK=/1, APB1=/1, APB2=/1
 *============================================================================*/

#define SYS_CLK_HZ          (48 * 1000 * 1000)      // SYSCLK 48MHz (PLL)
#define HCLK_HZ             SYS_CLK_HZ              // AHB = /1
#define PCLK1_HZ            HCLK_HZ                 // APB1 = /1
#define PCLK2_HZ            HCLK_HZ                 // APB2 = /1

/*============================================================================
 * 栈配置
 *============================================================================*/

#define STACK_SIZE          0x2000                  // 8KB 主栈

/*============================================================================
 * 延时函数 (简单忙等待)
 *============================================================================*/

static inline void delay_cycles(uint32_t cycles) {
    while (cycles--) {
        NOP();
    }
}

// 大约微秒级延时 (假设 48MHz)
static inline void delay_us(uint32_t us) {
    delay_cycles(us * 12);     // 约 48/4 = 12 cycles/us
}

// 大约毫秒级延时
static inline void delay_ms(uint32_t ms) {
    delay_us(ms * 1000);
}

#endif // NUCLEO_F767_H
