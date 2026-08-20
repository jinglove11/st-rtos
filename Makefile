#============================================================================
# Makefile for My-RTOS (Multi-Platform)
#============================================================================

#----------------------------------------------------------------------------
# 目标板选择
#
# 使用方法:
#   make menuconfig         # 交互式配置
#   make BOARD=stm32f767    # 编译 STM32F767
#   make BOARD=rp2350       # 编译 RP2350
#   make                    # 使用 .config 中的配置
#----------------------------------------------------------------------------

# 从配置文件读取目标板
-include .config

# 命令行 BOARD 参数优先
ifdef BOARD
    BOARD_NAME := $(BOARD)
else ifneq ($(filter y,$(CONFIG_BOARD_STM32F767)),)
    BOARD_NAME := stm32f767
else ifneq ($(filter y,$(CONFIG_BOARD_RP2350)),)
    BOARD_NAME := rp2350
else ifneq ($(filter "stm32f767",$(BOARD_NAME)),)
    # 从 BOARD_NAME 配置读取
    BOARD_NAME := stm32f767
else ifneq ($(filter "rp2350",$(BOARD_NAME)),)
    BOARD_NAME := rp2350
else
    # 默认值
    BOARD_NAME ?= stm32f767
endif

BOARD := $(BOARD_NAME)

ifeq ($(BOARD),rp2350)

PICO_SDK_PATH       ?= $(CURDIR)/tools/pico-sdk
PICO_TOOLCHAIN_PATH ?= $(CURDIR)/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin
PICOTOOL_DIR        ?= $(CURDIR)/tools/picotool/picotool
PICO_BUILD_DIR      ?= build/rp2350-pico-sdk
PICO_TARGET          = $(PICO_BUILD_DIR)/my-rtos-pico2w.elf
PICO_UF2             = $(PICO_BUILD_DIR)/my-rtos-pico2w.uf2

.PHONY: all configure clean flash verify setup-pico-sdk info help \
	rp2350_defconfig rp2350_smp_defconfig stm32f767_defconfig menuconfig genconfig \
	test-serial-boot test-serial-svc-runtime test-daplink \
	test-smp-soak \
	agent agent-start agent-stop agent-status agent-test

all: configure
	cmake --build $(PICO_BUILD_DIR) -j$${JOBS:-4}

configure:
	@test -f "$(PICO_SDK_PATH)/pico_sdk_init.cmake" || \
		{ echo "Pico SDK not found; run 'make setup-pico-sdk'"; exit 1; }
	cmake -S . -B $(PICO_BUILD_DIR) \
		-DPICO_SDK_PATH=$(PICO_SDK_PATH) \
		-DPICO_TOOLCHAIN_PATH=$(PICO_TOOLCHAIN_PATH) \
		-Dpicotool_DIR=$(PICOTOOL_DIR) \
		-DPICO_BOARD=pico2_w \
		-DCMAKE_BUILD_TYPE=Release

setup-pico-sdk:
	scripts/setup_pico_sdk.sh

verify: all
	python3 scripts/verify_pico2w_build.py \
		--elf $(PICO_TARGET) --uf2 $(PICO_UF2) \
		--nm $(PICO_TOOLCHAIN_PATH)/arm-none-eabi-nm \
		--picotool $(PICOTOOL_DIR)/picotool

# Rescue: clear stuck RP2350 (e.g. CM0 stuck in fault loop from prior bad
# image). Writes RESCUE_RESTART bit via DAP — both cores restart in bootrom.
# Idempotent: safe to run on a healthy chip. Must run in its own openocd
# session because the *next* openocd init is what re-examines the cores
# cleanly post-rescue.
rescue:
	openocd -f tools/openocd.cfg \
		-c "rescue_reset" -c "shutdown"

# flash 先做一次 rescue(对健康芯片无副作用),再用第二个 openocd session
# 做 program/verify。这样上一帧镜像把 CM0 卡进 fault loop 时也能自救。
flash: all
	openocd -f tools/openocd.cfg \
		-c "rescue_reset" -c "shutdown"
	openocd -f tools/openocd.cfg \
		-c "program $(PICO_TARGET) verify reset exit"

#----------------------------------------------------------------------------
# Live debug session: 后台串口 tee + openocd gdbserver
#
#   make debug-start         起 uart_tee + openocd(常驻 :3333)
#   make debug-stop          停两个后台
#   make debug-uart-log      tail /tmp/uart.log(LINES=N 改行数)
#   make debug-probe CMD="..."  一次性 gdb 命令(halt→cmd→resume)
#
# Claude 用法:
#   1) Bash: make debug-start
#   2) Read: /tmp/uart.log
#   3) Bash: scripts/gdb_probe.sh "info reg" "x/16i \$pc"
#   4) Bash: make debug-stop      (flash 前必须停 openocd)
#----------------------------------------------------------------------------

DEBUG_UART_LOG := /tmp/uart.log
DEBUG_OCD_LOG  := /tmp/openocd.log
DEBUG_GDB_PORT := 3333

debug-uart-start:
	@pkill -f "python3 .*uart_tee\.py" 2>/dev/null || true
	@setsid scripts/uart_tee.py 2>/tmp/uart_tee.err < /dev/null &
	@sleep 0.4
	@cat /tmp/uart_tee.err 2>/dev/null || true

debug-uart-stop:
	@pkill -f "python3 .*uart_tee\.py" 2>/dev/null || true
	@echo "uart_tee stopped"

debug-gdb-start:
	@pkill -x openocd 2>/dev/null || true
	@setsid openocd -f tools/openocd.cfg \
		-c "gdb_port $(DEBUG_GDB_PORT)" \
		> $(DEBUG_OCD_LOG) 2>&1 < /dev/null &
	@sleep 1.5
	@tail -5 $(DEBUG_OCD_LOG)
	@echo "openocd gdbserver on :$(DEBUG_GDB_PORT) (log: $(DEBUG_OCD_LOG))"

debug-gdb-stop:
	@pkill -x openocd 2>/dev/null || true
	@echo "openocd stopped"

debug-start: debug-uart-start debug-gdb-start
	@echo "=== debug up: uart=$(DEBUG_UART_LOG)  gdb=:$(DEBUG_GDB_PORT) ==="

debug-stop: debug-uart-stop debug-gdb-stop
	@echo "=== debug down ==="

debug-uart-log:
	@tail -n $${LINES:-40} $(DEBUG_UART_LOG)

# 一次性 gdb 探针 — 用分号串多个命令:
#   make debug-probe CMD="info reg; bt"
# 复杂命令(含 ;/$)请直接调 scripts/gdb_probe.sh
debug-probe:
	@scripts/gdb_probe.sh $(foreach c,$(subst ;, ,$(CMD)),"$(c)")

clean:
	cmake -E remove_directory $(PICO_BUILD_DIR)

rp2350_defconfig:
	@cp configs/rp2350_defconfig .config
	@python3 scripts/menuconfig.py genconfig
	@echo "Loaded Raspberry Pi Pico 2 W default configuration"

rp2350_smp_defconfig:
	@cp configs/rp2350_smp_defconfig .config
	@python3 scripts/menuconfig.py genconfig
	@echo "Loaded RP2350 dual-core M1 acceptance configuration"

stm32f767_defconfig:
	@cp configs/stm32f767_defconfig .config
	@python3 scripts/menuconfig.py genconfig
	@echo "Loaded STM32F767 default configuration"

menuconfig:
	@python3 scripts/menuconfig.py

genconfig:
	@python3 scripts/menuconfig.py genconfig

test-serial-svc-runtime:
	python3 scripts/serial_svc_runtime_test.py --port $${PORT:-/dev/ttyUSB0}

test-serial-boot:
	python3 scripts/serial_boot_test.py --port $${PORT:-/dev/ttyUSB0}

test-smp-soak:
	python3 scripts/smp_soak_test.py \
		--port $${PORT:-/dev/ttyACM0} \
		--duration $${DURATION:-1800} \
		--startup-timeout $${STARTUP_TIMEOUT:-1800} \
		--log $${LOG:-/tmp/my-rtos-smp-soak.log}

test-daplink: all
	python3 scripts/serial_boot_test.py \
		--port $${PORT:-/dev/ttyACM0} \
		--daplink-flash \
		--openocd-config tools/openocd.cfg \
		--elf $(PICO_TARGET)

agent:
	python3 tools/embedded-agent/embedded_agent.py daemon \
		--config tools/embedded-agent/my-rtos.json \
		--port $${PORT:-/dev/ttyACM0} --baud $${BAUD:-921600}

agent-start:
	@nohup python3 tools/embedded-agent/embedded_agent.py daemon \
		--config tools/embedded-agent/my-rtos.json \
		--port $${PORT:-/dev/ttyACM0} --baud $${BAUD:-921600} \
		>/tmp/my-rtos-agent.out 2>&1 & echo $$! >/tmp/my-rtos-agent.pid
	@for i in $$(seq 1 30); do \
		test -S /tmp/my-rtos-agent.sock && break; \
		kill -0 $$(cat /tmp/my-rtos-agent.pid) 2>/dev/null || { \
			cat /tmp/my-rtos-agent.out; exit 1; }; \
		sleep 0.1; \
	done; \
	test -S /tmp/my-rtos-agent.sock || { \
		echo "agent socket did not become ready"; exit 1; }; \
	echo "My-RTOS agent started (pid $$(cat /tmp/my-rtos-agent.pid))"

agent-stop:
	@if [ -f /tmp/my-rtos-agent.pid ]; then \
		kill $$(cat /tmp/my-rtos-agent.pid) 2>/dev/null || true; \
		rm -f /tmp/my-rtos-agent.pid; \
	fi

agent-status:
	python3 tools/embedded-agent/embedded_agent.py ctl \
		--config tools/embedded-agent/my-rtos.json status

agent-test:
	PYTHONPATH=tools/embedded-agent python3 -m unittest -v \
		tools/embedded-agent/test_embedded_agent.py

info:
	@echo "My-RTOS Build Configuration"
	@echo "  Board:      Raspberry Pi Pico 2 W"
	@echo "  MCU:        RP2350 (Cortex-M33 secure / flat image)"
	@echo "  SDK:        $(PICO_SDK_PATH)"
	@echo "  Build dir:  $(PICO_BUILD_DIR)"
	@echo "  UART:       GPIO0 TX / GPIO1 RX / 921600 8N1"

help:
	@echo "My-RTOS Pico 2 W build"
	@echo "  make setup-pico-sdk  Download Pico SDK and build picotool"
	@echo "  make                 Build ELF/BIN/HEX/UF2"
	@echo "  make verify          Validate RP2350 image and RTOS vectors"
	@echo "  make flash           Flash via DAPLink/OpenOCD (CMSIS-DAP)"
	@echo "  make test-serial-boot PORT=/dev/ttyUSB0"
	@echo "  make test-daplink PORT=/dev/ttyACM0  Build, DAPLink flash, and test"
	@echo "  make test-serial-svc-runtime PORT=/dev/ttyUSB0"
	@echo "  make rp2350_smp_defconfig  Load dual-core 1M-iteration acceptance profile"
	@echo "  make test-smp-soak PORT=/dev/ttyACM0 DURATION=1800"
	@echo "  make agent-start PORT=/dev/ttyACM0 BAUD=921600"
	@echo "  make agent-status | agent-stop | agent-test"
	@echo "  make stm32f767_defconfig && make BOARD=stm32f767"

else

#----------------------------------------------------------------------------
# 平台配置
#----------------------------------------------------------------------------

ifeq ($(BOARD),stm32f767)
    TARGET_MCU      = stm32f767
    CPU             = -mcpu=cortex-m7 -mthumb -mfloat-abi=soft
    BOARD_DEFINE    = BOARD_STM32F767_NUCLEO
    LINK_SCRIPT     = link/stm32f767.ld
    STARTUP_ASM     = src/startup/arm/cortex-m7/vectors.S
    STARTUP_ASM     += src/startup/arm/cortex-m7/fault_handlers.S
    STARTUP_ASM     += src/startup/arm/cortex-m7/reset_handler.S
    STARTUP_C       =
    SYSTEM_C        = src/startup/arm/cortex-m7/system.c
    UART_SRC        = src/drivers/chip/stm32f7/uart_stm32.c
    GPIO_SRC        = src/drivers/chip/stm32f7/gpio_stm32.c
    HAL_SRC         = src/arch/arm/cortex-m/hal.c
    HAL_ASM         = src/arch/arm/cortex-m/pendsv_handler.S
    HAL_ASM         += src/arch/arm/cortex-m/svc_handler.S
    HAL_ASM         += src/arch/arm/cortex-m/first_switch.S
    FLASH_CMD       = openocd -f board/st_nucleo_f7.cfg -c "program $(TARGET) verify reset exit"

else
    $(error Unknown BOARD: $(BOARD). Use 'rp2350' (handled via CMake at top of Makefile) or 'stm32f767')
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
CFLAGS      += -MMD -MP
CFLAGS      += -Iinclude/kernel
CFLAGS      += -Iinclude/drivers
CFLAGS      += -Iinclude/board
CFLAGS      += -Iinclude/app
CFLAGS      += -I$(SRC_DIR)/kernel/ipc
CFLAGS      += -I$(SRC_DIR)/kernel/vfs
CFLAGS      += -I$(SRC_DIR)/kernel
CFLAGS      += -I$(SRC_DIR)/board/$(TARGET_MCU)
CFLAGS      += -I$(SRC_DIR)/user/nameserver
CFLAGS      += -I$(SRC_DIR)/user/drivers
CFLAGS      += -I$(SRC_DIR)/user/fs
CFLAGS      += -I$(SRC_DIR)/user/supervisor
CFLAGS      += -I$(SRC_DIR)/arch/arm/cortex-m
CFLAGS      += -I$(SRC_DIR)/tests
CFLAGS      += -I$(SRC_DIR)/app
CFLAGS      += -DTARGET_BOARD=$(BOARD_DEFINE)

ASFLAGS     = $(CPU)
ASFLAGS     += -Wall -g
ASFLAGS     += -x assembler-with-cpp
ASFLAGS     += -MMD -MP
ASFLAGS     += -Iinclude/kernel
ASFLAGS     += -I$(SRC_DIR)/arch/arm/cortex-m

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
KERN_SOURCES += src/kernel/ipc/wait_queue.c
KERN_SOURCES += src/kernel/ipc/semaphore.c
KERN_SOURCES += src/kernel/ipc/mutex.c
KERN_SOURCES += src/kernel/ipc/mqueue.c
KERN_SOURCES += src/kernel/ipc/event.c
KERN_SOURCES += src/kernel/ipc/notification.c
KERN_SOURCES += src/kernel/ipc/ipc_transfer.c
KERN_SOURCES += src/kernel/ipc/endpoint.c
KERN_SOURCES += src/kernel/ipc/channel.c
KERN_SOURCES += src/kernel/irq/irq.c
KERN_SOURCES += src/kernel/irq/bh.c
KERN_SOURCES += src/kernel/mpu/mpu.c
KERN_SOURCES += src/kernel/syscall/syscall.c
KERN_SOURCES += src/kernel/usercopy/usercopy.c
KERN_SOURCES += src/kernel/timer/timer.c
KERN_SOURCES += src/kernel/cap/capability.c
KERN_SOURCES += src/kernel/cap/cap_subset.c
KERN_SOURCES += src/kernel/fault/fault.c
KERN_SOURCES += src/kernel/fault/fault_endpoint.c
KERN_SOURCES += src/kernel/fault/panic_log.c
KERN_SOURCES += src/kernel/dev/device.c
KERN_SOURCES += src/kernel/trace/trace.c
KERN_SOURCES += src/kernel/stats/stats.c
KERN_SOURCES += src/kernel/core/lockdep.c
KERN_SOURCES += src/kernel/core/smp.c
KERN_SOURCES += src/kernel/syscall/continuation.c
KERN_SOURCES += src/kernel/cap/factory.c
KERN_SOURCES += src/kernel/loader/elf_loader.c
KERN_SOURCES += src/kernel/root_bootstrap.c
KERN_SOURCES += src/kernel/system_init.c

# ---- 测试源(三层:K 内核不变量 / ABI syscall 契约 / SYS 启动健康)----
# 框架
TEST_SOURCES  = src/tests/test_framework.c
# K 层: 内核不变量(白盒)
TEST_SOURCES += src/tests/k/test_allocator.c
TEST_SOURCES += src/tests/k/test_block.c
TEST_SOURCES += src/tests/k/test_capability.c
TEST_SOURCES += src/tests/k/test_deadlock.c
TEST_SOURCES += src/tests/k/test_diag.c
TEST_SOURCES += src/tests/k/test_driver.c
TEST_SOURCES += src/tests/k/test_elf.c
TEST_SOURCES += src/tests/k/test_factory.c
TEST_SOURCES += src/tests/k/test_fault.c
TEST_SOURCES += src/tests/k/test_fs_devfs.c
TEST_SOURCES += src/tests/k/test_fs_fd_cleanup.c
TEST_SOURCES += src/tests/k/test_fs_store.c
TEST_SOURCES += src/tests/k/test_gpio_driver.c
TEST_SOURCES += src/tests/k/test_init_orchestrate.c
TEST_SOURCES += src/tests/k/test_irq.c
TEST_SOURCES += src/tests/k/test_ipc_upgrade.c
TEST_SOURCES += src/tests/k/test_mem.c
TEST_SOURCES += src/tests/k/test_mmio.c
TEST_SOURCES += src/tests/k/test_rt_sched.c
TEST_SOURCES += src/tests/k/test_scheduler.c
TEST_SOURCES += src/tests/k/test_service_model.c
TEST_SOURCES += src/tests/k/test_shell.c
TEST_SOURCES += src/tests/k/test_smp.c
TEST_SOURCES += src/tests/k/test_stats.c
TEST_SOURCES += src/tests/k/test_supervisor_monitor.c
TEST_SOURCES += src/tests/k/test_svc_runtime.c
TEST_SOURCES += src/tests/k/test_sync_server.c
TEST_SOURCES += src/tests/k/test_syscall.c
TEST_SOURCES += src/tests/k/test_task.c
TEST_SOURCES += src/tests/k/test_timer.c
TEST_SOURCES += src/tests/k/test_trace.c
TEST_SOURCES += src/tests/k/test_vfs.c
TEST_SOURCES += src/tests/k/test_watchdog.c
# ABI 层: syscall 契约(用户任务黑盒)
TEST_SOURCES += src/tests/abi/test_abi.c
TEST_SOURCES += src/tests/abi/test_factory_user.c
TEST_SOURCES += src/tests/abi/test_fault_user.c
TEST_SOURCES += src/tests/abi/test_fuzz.c
TEST_SOURCES += src/tests/abi/test_irq_user.c
TEST_SOURCES += src/tests/abi/test_mpu.c
TEST_SOURCES += src/tests/abi/test_notification.c
TEST_SOURCES += src/tests/abi/test_ntfn_user.c
TEST_SOURCES += src/tests/k/test_notification.c
TEST_SOURCES += src/tests/k/test_mpu_aspace.c
TEST_SOURCES += src/tests/abi/test_syscall_user.c
TEST_SOURCES += src/tests/abi/test_usercopy.c
# SYS 层: 启动健康
TEST_SOURCES += src/tests/sys/test_boot_health.c
# TEST_SOURCES += src/tests/test_example.c  # 示例测试模块（取消注释启用）

HAL_SOURCES  = $(HAL_SRC)

APP_SOURCES  = src/app/main.c
APP_SOURCES  += src/app/shell.c
APP_SOURCES  += src/user/nameserver/nameserver.c
APP_SOURCES  += src/user/drivers/uart_server.c
APP_SOURCES  += src/user/drivers/driver_registry.c
APP_SOURCES  += src/user/drivers/driver_client.c
APP_SOURCES  += src/user/drivers/driver_runtime.c
APP_SOURCES  += src/user/fs/fs_server.c
APP_SOURCES  += src/user/fs/fs_store.c
APP_SOURCES  += src/user/fs/fs_runtime.c
APP_SOURCES  += src/user/supervisor/supervisor.c
APP_SOURCES  += src/user/init/init.c
APP_SOURCES  += src/user/apps/crashy_app.c
APP_SOURCES  += $(UART_SRC)
APP_SOURCES  += $(GPIO_SRC)
APP_SOURCES  += src/drivers/uart_dev.c
APP_SOURCES  += src/drivers/block/flash_block.c
APP_SOURCES  += src/drivers/block/block_dev.c
APP_SOURCES  += src/board/stm32f767/board_drivers.c
APP_SOURCES  += src/board/stm32f767/board.c
APP_SOURCES  += $(STARTUP_C)
APP_SOURCES  += $(SYSTEM_C)

ASM_SOURCES  = $(STARTUP_ASM)
ASM_SOURCES  += $(HAL_ASM)

C_SOURCES    = $(KERN_SOURCES) $(HAL_SOURCES) $(APP_SOURCES) $(TEST_SOURCES)

OBJECTS      = $(C_SOURCES:src/%.c=$(BUILD_DIR)/%.o)
OBJECTS      += $(ASM_SOURCES:src/%.S=$(BUILD_DIR)/%.o)
DEPS         = $(OBJECTS:.o=.d)

#----------------------------------------------------------------------------
# 构建规则
#----------------------------------------------------------------------------

.PHONY: all clean flash flash-openocd test-serial-svc-runtime info help menuconfig defconfig genconfig

# 配置文件
CONFIG_FILE   = .config
CONFIG_HEADER = include/kernel/kernel_config.h

all: $(TARGET)

# 检查配置文件
$(CONFIG_FILE):
	@echo "No configuration found, loading $(BOARD) defconfig..."
	@if [ -f configs/$(BOARD)_defconfig ]; then \
		cp configs/$(BOARD)_defconfig $(CONFIG_FILE); \
	else \
		python3 scripts/menuconfig.py defconfig; \
	fi

# 生成配置头文件
$(CONFIG_HEADER): $(CONFIG_FILE)
	@python3 scripts/menuconfig.py genconfig

# 编译前检查配置
$(TARGET): $(CONFIG_HEADER) $(OBJECTS) $(LINK_SCRIPT)
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

-include $(DEPS)

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

test-serial-svc-runtime:
	python3 scripts/serial_svc_runtime_test.py --port $${PORT:-/dev/ttyACM0}

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
# 配置
#----------------------------------------------------------------------------

# 加载默认配置
menuconfig:
	@python3 scripts/menuconfig.py

defconfig:
	@python3 scripts/menuconfig.py defconfig

# 根据目标板加载默认配置
stm32f767_defconfig:
	@cp configs/stm32f767_defconfig .config
	@python3 scripts/menuconfig.py genconfig
	@echo "Loaded STM32F767 default configuration"

rp2350_defconfig:
	@cp configs/rp2350_defconfig .config
	@python3 scripts/menuconfig.py genconfig
	@echo "Loaded RP2350 default configuration"

genconfig:
	@python3 scripts/menuconfig.py genconfig

#----------------------------------------------------------------------------
# 帮助
#----------------------------------------------------------------------------

help:
	@echo "My-RTOS Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make [target] [BOARD=<board>]"
	@echo ""
	@echo "Configuration:"
	@echo "  menuconfig         - Interactive configuration"
	@echo "  stm32f767_defconfig- Load STM32F767 default config"
	@echo "  rp2350_defconfig   - Load RP2350 default config"
	@echo "  genconfig          - Generate config header"
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
	@echo "  test-serial-svc-runtime - Run svc runtime serial smoke test"
	@echo "  info         - Show build configuration"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make stm32f767_defconfig   # Load STM32 config"
	@echo "  make menuconfig            # Configure the project"
	@echo "  make                       # Build with .config"
	@echo "  make BOARD=rp2350 flash    # Build and flash Pico 2"
	@echo "  make test-serial-svc-runtime PORT=/dev/ttyACM0"

endif
