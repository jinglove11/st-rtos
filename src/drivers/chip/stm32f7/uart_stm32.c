/**
 * @file uart_stm32.c
 * @brief STM32 UART 驱动实现
 */

#include "uart.h"
#include "nucleo_f767.h"

/*============================================================================
 * 公开函数
 *============================================================================*/

void uart_init(usart_t *usart, uint32_t baudrate) {
    // 1. 使能时钟
    // GPIOD 时钟
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;

    // USART3 时钟
    if (usart == USART3) {
        RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    }

    // 2. 配置 GPIO (PD8=TX, PD9=RX, AF7)
    // PD8 TX - 复用功能推挽输出
    GPIOD->MODER &= ~(3U << (8 * 2));       // 清除模式
    GPIOD->MODER |= (2U << (8 * 2));        // 复用模式
    GPIOD->AFR[1] &= ~(0xF << ((8 - 8) * 4)); // 清除 AF
    GPIOD->AFR[1] |= (7U << ((8 - 8) * 4));   // AF7
    GPIOD->OSPEEDR |= (3U << (8 * 2));      // 高速

    // PD9 RX - 复用功能输入
    GPIOD->MODER &= ~(3U << (9 * 2));       // 清除模式
    GPIOD->MODER |= (2U << (9 * 2));        // 复用模式
    GPIOD->AFR[1] &= ~(0xF << ((9 - 8) * 4)); // 清除 AF
    GPIOD->AFR[1] |= (7U << ((9 - 8) * 4));   // AF7
    GPIOD->PUPDR &= ~(3U << (9 * 2));       // 清除上拉下拉
    GPIOD->PUPDR |= (1U << (9 * 2));        // 上拉

    // 3. 配置 USART
    // 禁用 USART
    usart->CR1 &= ~USART_CR1_UE;

    // 配置波特率: BRR = fclk / (16 * baud)
    // 16MHz / (16 * 115200) = 8.68 ≈ 9
    usart->BRR = (PCLK1_HZ / baudrate);

    // 8N1 配置
    usart->CR1 = USART_CR1_TE | USART_CR1_RE;  // 发送接收使能

    // 使能 USART
    usart->CR1 |= USART_CR1_UE;
}

void uart_putc(usart_t *usart, char c) {
    // 等待发送寄存器空
    while (!(usart->ISR & USART_ISR_TXE));
    usart->TDR = c;
}

char uart_getc(usart_t *usart) {
    while (!(usart->ISR & USART_ISR_RXNE));
    return (char)(usart->RDR & 0xFF);
}

void uart_puts(usart_t *usart, const char *s) {
    while (*s) {
        uart_putc(usart, *s++);
    }
}

void uart_puthex(usart_t *usart, uint32_t value) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts(usart, "0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(usart, hex[(value >> i) & 0xf]);
    }
}

void uart_putdec(usart_t *usart, uint32_t value) {
    char buf[12];
    int i = 0;

    if (value == 0) {
        uart_putc(usart, '0');
        return;
    }

    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        uart_putc(usart, buf[--i]);
    }
}

int uart_readable(usart_t *usart) {
    return (usart->ISR & USART_ISR_RXNE) != 0;
}

int uart_writable(usart_t *usart) {
    return (usart->ISR & USART_ISR_TXE) != 0;
}
