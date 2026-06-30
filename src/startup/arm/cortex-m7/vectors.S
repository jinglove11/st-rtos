/**
 * @file vectors.S
 * @brief STM32F767 中断向量表
 *
 * Cortex-M7 向量表布局：
 *   [0]  初始 MSP
 *   [1]  Reset_Handler
 *   [2-15] 系统异常
 *   [16+]  外部中断 (IRQ)
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

    .extern _default_handler
    .extern _reset_handler
    .extern HardFault_Handler
    .extern MemManage_Handler
    .extern BusFault_Handler
    .extern UsageFault_Handler
    .extern SVC_Handler
    .extern PendSV_Handler
    .extern SysTick_Handler

    .section .isr_vector, "a"
    .align 2
    .global _vectors

_vectors:
    .word   _estack                     // 0: 初始栈指针
    .word   _reset_handler              // 1: Reset
    .word   _default_handler            // 2: NMI
    .word   HardFault_Handler           // 3: HardFault
    .word   MemManage_Handler           // 4: MemManage
    .word   BusFault_Handler            // 5: BusFault
    .word   UsageFault_Handler          // 6: UsageFault
    .word   0                           // 7: Reserved
    .word   0                           // 8: Reserved
    .word   0                           // 9: Reserved
    .word   0                           // 10: Reserved
    .word   SVC_Handler                 // 11: SVCall
    .word   _default_handler            // 12: Debug Monitor
    .word   0                           // 13: Reserved
    .word   PendSV_Handler              // 14: PendSV
    .word   SysTick_Handler             // 15: SysTick

    // 外部中断 (STM32F767, 16-113)
    .word   _default_handler            // 16: WWDG
    .word   _default_handler            // 17: PVD
    .word   _default_handler            // 18: TAMP_STAMP
    .word   _default_handler            // 19: RTC_WKUP
    .word   _default_handler            // 20: FLASH
    .word   _default_handler            // 21: RCC
    .word   _default_handler            // 22: EXTI0
    .word   _default_handler            // 23: EXTI1
    .word   _default_handler            // 24: EXTI2
    .word   _default_handler            // 25: EXTI3
    .word   _default_handler            // 26: EXTI4
    .word   _default_handler            // 27: DMA1_Stream0
    .word   _default_handler            // 28: DMA1_Stream1
    .word   _default_handler            // 29: DMA1_Stream2
    .word   _default_handler            // 30: DMA1_Stream3
    .word   _default_handler            // 31: DMA1_Stream4
    .word   _default_handler            // 32: DMA1_Stream5
    .word   _default_handler            // 33: DMA1_Stream6
    .word   _default_handler            // 34: ADC
    .word   _default_handler            // 35: CAN1_TX
    .word   _default_handler            // 36: CAN1_RX0
    .word   _default_handler            // 37: CAN1_RX1
    .word   _default_handler            // 38: CAN1_SCE
    .word   _default_handler            // 39: EXTI9_5
    .word   _default_handler            // 40: TIM1_BRK_TIM9
    .word   _default_handler            // 41: TIM1_UP_TIM10
    .word   _default_handler            // 42: TIM1_TRG_COM_TIM11
    .word   _default_handler            // 43: TIM1_CC
    .word   _default_handler            // 44: TIM2
    .word   _default_handler            // 45: TIM3
    .word   _default_handler            // 46: TIM4
    .word   _default_handler            // 47: I2C1_EV
    .word   _default_handler            // 48: I2C1_ER
    .word   _default_handler            // 49: I2C2_EV
    .word   _default_handler            // 50: I2C2_ER
    .word   _default_handler            // 51: SPI1
    .word   _default_handler            // 52: SPI2
    .word   _default_handler            // 53: USART1
    .word   _default_handler            // 54: USART2
    .word   _default_handler            // 55: USART3
    .word   _default_handler            // 56: EXTI15_10
    .word   _default_handler            // 57: RTC_Alarm
    .word   _default_handler            // 58: OTG_FS_WKUP
    .word   _default_handler            // 59: TIM8_BRK_TIM12
    .word   _default_handler            // 60: TIM8_UP_TIM13
    .word   _default_handler            // 61: TIM8_TRG_COM_TIM14
    .word   _default_handler            // 62: TIM8_CC
    .word   _default_handler            // 63: DMA1_Stream7
    .word   _default_handler            // 64: FMC
    .word   _default_handler            // 65: SDMMC1
    .word   _default_handler            // 66: TIM5
    .word   _default_handler            // 67: SPI3
    .word   _default_handler            // 68: UART4
    .word   _default_handler            // 69: UART5
    .word   _default_handler            // 70: TIM6_DAC
    .word   _default_handler            // 71: TIM7
    .word   _default_handler            // 72: DMA2_Stream0
    .word   _default_handler            // 73: DMA2_Stream1
    .word   _default_handler            // 74: DMA2_Stream2
    .word   _default_handler            // 75: DMA2_Stream3
    .word   _default_handler            // 76: DMA2_Stream4
    .word   _default_handler            // 77: ETH
    .word   _default_handler            // 78: ETH_WKUP
    .word   _default_handler            // 79: CAN2_TX
    .word   _default_handler            // 80: CAN2_RX0
    .word   _default_handler            // 81: CAN2_RX1
    .word   _default_handler            // 82: CAN2_SCE
    .word   _default_handler            // 83: OTG_FS
    .word   _default_handler            // 84: DMA2_Stream5
    .word   _default_handler            // 85: DMA2_Stream6
    .word   _default_handler            // 86: DMA2_Stream7
    .word   _default_handler            // 87: USART6
    .word   _default_handler            // 88: I2C3_EV
    .word   _default_handler            // 89: I2C3_ER
    .word   _default_handler            // 90: OTG_HS_EP1_OUT
    .word   _default_handler            // 91: OTG_HS_EP1_IN
    .word   _default_handler            // 92: OTG_HS_WKUP
    .word   _default_handler            // 93: OTG_HS
    .word   _default_handler            // 94: DCMI
    .word   _default_handler            // 95: CRYP
    .word   _default_handler            // 96: HASH_RNG
    .word   _default_handler            // 97: FPU
    .word   _default_handler            // 98: UART7
    .word   _default_handler            // 99: UART8
    .word   _default_handler            // 100: SPI4
    .word   _default_handler            // 101: SPI5
    .word   _default_handler            // 102: SPI6
    .word   _default_handler            // 103: SAI1
    .word   _default_handler            // 104: LTDC
    .word   _default_handler            // 105: LTDC_ER
    .word   _default_handler            // 106: DMA2D
    .word   _default_handler            // 107: SAI2
    .word   _default_handler            // 108: QUADSPI
    .word   _default_handler            // 109: LPTIM1
    .word   _default_handler            // 110: CEC
    .word   _default_handler            // 111: I2C4_EV
    .word   _default_handler            // 112: I2C4_ER
    .word   _default_handler            // 113: SPDIF_RX

    .end
