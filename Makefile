#============================================================================
# Makefile for My-RTOS (Multi-Platform)
#============================================================================

#----------------------------------------------------------------------------
# 目标板选择
#
# 使用方法:
#   make BOARD=rp2350       # 编译 RP2350 (Pico 2)
#   make BOARD=stm32f767    # 编译 STM32F767 (Nucleo-F767ZI)
#   make                    # 默认编译 STM32F767
#----------------------------------------------------------------------------

BOARD       ?= stm32f767

#----------------------------------------------------------------------------
# 平台配置
#----------------------------------------------------------------------------

ifeq ($(BOARD),rp2350)
    TARGET_MCU      = rp2350
    CPU             = -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
    BOARD_DEFINE    = BOARD_RP2350_PICO2
    LINK_SCRIPT     = link/rp2350.ld
    STARTUP_ASM     = src/startup/arm/boot2.S
    STARTUP_C       = src/startup/arm/startup.c
    SYSTEM_C        =
    UART_SRC        = src/drivers/chip/stm32f7/uart.c
    GPIO_SRC        = src/drivers/chip/stm32f7/gpio.c
    HAL_SRC         = src/arch/arm/cortex-m7/hal.c
    HAL_ASM         = src/arch/arm/cortex-m7/context.S
    FLASH_CMD       = picotool load $(TARGET_BIN) -t bin -x 0x10000000

else ifeq ($(BOARD),stm32f767)
    TARGET_MCU      = stm32f767
    CPU             = -mcpu=cortex-m7 -mthumb -mfloat-abi=soft
    BOARD_DEFINE    = BOARD_STM32F767_NUCLEO
    LINK_SCRIPT     = link/stm32f767.ld
    STARTUP_ASM     = src/startup/arm/startup.S
    STARTUP_C       =
    SYSTEM_C        = src/startup/arm/system.c
    UART_SRC        = src/drivers/chip/stm32f7/uart_stm32.c
    GPIO_SRC        = src/drivers/chip/stm32f7/gpio_stm32.c
    HAL_SRC         = src/arch/arm/cortex-m7/hal.c
    HAL_ASM         = src/arch/arm/cortex-m7/context.S
    FLASH_CMD       = openocd -f board/st_nucleo_f7.cfg -c "program $(TARGET) verify reset exit"

else
    $(error Unknown BOARD: $(BOARD). Use 'rp2350' or 'stm32f767')
endif

#----------------------------------------------------------------------------
# 工具链配置
#----------------------------------------------------------------------------

TOOLCHAIN   = tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin

CC          = $(TOOLCHAIN)/arm-none-eabi-gcc
AS          = $(TOOLCHAIN)/arm-none-eabi-gcc
LD          = $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY     = $(TOOLCHAIN)/arm-none-eabi-objcopy
OBJDUMP     = $(TOOLCHAIN)/arm-none-eabi-objdump
SIZE        = $(TOOLCHAIN)/arm-none-eabi-size

#----------------------------------------------------------------------------
# 项目配置
#----------------------------------------------------------------------------

PROJECT     = my-rtos-$(TARGET_MCU)

SRC_DIR     = src
LINK_DIR    = link
BUILD_DIR   = build/$(TARGET_MCU)

TARGET      = $(BUILD_DIR)/$(PROJECT).elf
TARGET_BIN  = $(BUILD_DIR)/$(PROJECT).bin
TARGET_HEX  = $(BUILD_DIR)/$(PROJECT).hex
TARGET_MAP  = $(BUILD_DIR)/$(PROJECT).map

#----------------------------------------------------------------------------
# 编译选项
#----------------------------------------------------------------------------

CFLAGS      = $(CPU)
CFLAGS      += -Wall -Wextra -Werror
CFLAGS      += -O2 -g
CFLAGS      += -ffreestanding -nostdlib
CFLAGS      += -fno-builtin -fno-common
CFLAGS      += -ffunction-sections -fdata-sections
CFLAGS      += -I$(SRC_DIR)/kernel/include
CFLAGS      += -I$(SRC_DIR)/kernel/core
CFLAGS      += -I$(SRC_DIR)/kernel/task
CFLAGS      += -I$(SRC_DIR)/kernel/ipc
CFLAGS      += -I$(SRC_DIR)/kernel/mem
CFLAGS      += -I$(SRC_DIR)/hal
CFLAGS      += -I$(SRC_DIR)/drivers/include
CFLAGS      += -I$(SRC_DIR)/board/$(TARGET_MCU)
CFLAGS      += -I$(SRC_DIR)/arch/arm/cortex-m7
CFLAGS      += -DTARGET_BOARD=$(BOARD_DEFINE)

ASFLAGS     = $(CPU)
ASFLAGS     += -Wall -g
ASFLAGS     += -x assembler-with-cpp

LDFLAGS     = $(CPU)
LDFLAGS     += -T$(LINK_SCRIPT)
LDFLAGS     += -nostdlib -nostartfiles
LDFLAGS     += -Wl,-Map=$(TARGET_MAP)
LDFLAGS     += -Wl,--gc-sections
LDFLAGS     += -Wl,--print-memory-usage
LDFLAGS     += -lgcc

#----------------------------------------------------------------------------
# 源文件
#----------------------------------------------------------------------------

KERN_SOURCES = src/kernel/kernel.c
KERN_SOURCES += src/kernel/core/scheduler.c
KERN_SOURCES += src/kernel/task/task.c
KERN_SOURCES += src/kernel/lib/kstring.c
KERN_SOURCES += src/kernel/mem/mem.c
KERN_SOURCES += src/kernel/mem/mempool.c
KERN_SOURCES += src/kernel/ipc/semaphore.c
KERN_SOURCES += src/kernel/ipc/mutex.c
KERN_SOURCES += src/kernel/ipc/mqueue.c
KERN_SOURCES += src/kernel/ipc/event.c

HAL_SOURCES  = $(HAL_SRC)

APP_SOURCES  = src/app/main.c
APP_SOURCES  += $(UART_SRC)
APP_SOURCES  += $(GPIO_SRC)
APP_SOURCES  += $(STARTUP_C)
APP_SOURCES  += $(SYSTEM_C)

ASM_SOURCES  = $(STARTUP_ASM)
ASM_SOURCES  += $(HAL_ASM)

C_SOURCES    = $(KERN_SOURCES) $(HAL_SOURCES) $(APP_SOURCES)

OBJECTS      = $(C_SOURCES:src/%.c=$(BUILD_DIR)/%.o)
OBJECTS      += $(ASM_SOURCES:src/%.S=$(BUILD_DIR)/%.o)

#----------------------------------------------------------------------------
# 构建规则
#----------------------------------------------------------------------------

.PHONY: all clean flash info help

all: $(TARGET)

$(TARGET): $(OBJECTS) $(LINK_SCRIPT)
	@echo "Linking: $@"
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@
	$(OBJCOPY) -O binary $@ $(TARGET_BIN)
	$(OBJCOPY) -O ihex $@ $(TARGET_HEX)
	$(SIZE) $@

$(BUILD_DIR)/%.o: src/%.c
	@echo "Compiling: $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.S
	@echo "Assembling: $<"
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

#----------------------------------------------------------------------------
# 清理
#----------------------------------------------------------------------------

clean:
	@echo "Cleaning..."
	rm -rf build

#----------------------------------------------------------------------------
# 烧录
#----------------------------------------------------------------------------

flash: $(TARGET_BIN)
	@echo "Flashing $(BOARD)..."
	$(FLASH_CMD)

flash-openocd: $(TARGET)
	openocd -f board/st_nucleo_f7.cfg -c "program $(TARGET) verify reset exit"

#----------------------------------------------------------------------------
# 信息
#----------------------------------------------------------------------------

info:
	@echo "My-RTOS Build Configuration"
	@echo ""
	@echo "  Board:      $(BOARD)"
	@echo "  MCU:        $(TARGET_MCU)"
	@echo "  CPU flags:  $(CPU)"
	@echo "  Linker:     $(LINK_SCRIPT)"
	@echo "  Build dir:  $(BUILD_DIR)"
	@echo ""
	@echo "Kernel sources:"
	@for f in $(KERN_SOURCES); do echo "  $$f"; done
	@echo ""
	@echo "HAL sources:"
	@for f in $(HAL_SOURCES); do echo "  $$f"; done
	@echo ""
	@echo "App sources:"
	@for f in $(APP_SOURCES); do echo "  $$f"; done
	@echo ""
	@echo "Assembly sources:"
	@for f in $(ASM_SOURCES); do echo "  $$f"; done

#----------------------------------------------------------------------------
# 帮助
#----------------------------------------------------------------------------

help:
	@echo "My-RTOS Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make BOARD=<board> [target]"
	@echo ""
	@echo "Boards:"
	@echo "  rp2350       - Raspberry Pi Pico 2 (RP2350)"
	@echo "  stm32f767    - STM32 Nucleo-F767ZI (default)"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the project (default)"
	@echo "  clean        - Remove build artifacts"
	@echo "  flash        - Flash to target board"
	@echo "  flash-openocd- Flash using OpenOCD (STM32)"
	@echo "  info         - Show build configuration"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make BOARD=stm32f767        # Build for Nucleo-F767ZI"
	@echo "  make BOARD=rp2350 flash     # Build and flash Pico 2"
