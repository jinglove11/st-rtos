/**
 * @file system_stm32f767.c
 * @brief STM32F767 系统初始化 (使用 HSI 简化配置)
 */

#include "stm32f767.h"

/*============================================================================
 * SystemInit - 系统时钟初始化
 *
 * 使用 HSI 16MHz (内部振荡器) 作为系统时钟
 * 简单可靠，适合调试
 *============================================================================*/

void SystemInit(void) {
    // 1. 使能 HSI (默认已使能)
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    // 2. 选择 HSI 作为系统时钟
    RCC->CFGR = (RCC->CFGR & ~3) | 0;  // SW = HSI

    // 3. 等待切换完成
    while ((RCC->CFGR & (3 << 2)) != 0);

    // 4. 设置向量表位置
    SCB->VTOR = 0x08000000UL;
}
