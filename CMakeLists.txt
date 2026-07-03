cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico2_w CACHE STRING "Pico SDK board")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico SDK platform")

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/tools/pico-sdk/pico_sdk_init.cmake")
        set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/tools/pico-sdk")
    else()
        message(FATAL_ERROR
            "Pico SDK not found. Set PICO_SDK_PATH or run 'make setup-pico-sdk'.")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(my_rtos_pico2w C CXX ASM)
pico_sdk_init()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB_RECURSE MYRTOS_KERNEL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/kernel/*.c")
file(GLOB_RECURSE MYRTOS_USER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/user/*.c")
file(GLOB MYRTOS_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/*.c")
list(REMOVE_ITEM MYRTOS_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/test_example.c")

set(MYRTOS_SOURCES
    ${MYRTOS_KERNEL_SOURCES}
    ${MYRTOS_USER_SOURCES}
    ${MYRTOS_TEST_SOURCES}
    src/app/main.c
    src/app/shell.c
    src/arch/arm/cortex-m7/hal.c
    src/arch/arm/cortex-m7/pendsv_handler.S
    src/arch/arm/cortex-m7/svc_handler.S
    src/arch/arm/cortex-m7/first_switch.S
    src/startup/arm/cortex-m7/fault_handlers.S
    src/drivers/chip/rp2350/uart_rp2350.c
    src/drivers/chip/rp2350/gpio_rp2350.c
    src/drivers/uart_dev.c
    src/drivers/block/flash_block.c
    src/drivers/block/block_dev.c
    src/board/rp2350/board.c
    src/board/rp2350/board_drivers.c
)

add_executable(my-rtos-pico2w ${MYRTOS_SOURCES})

# Generate TCB field offsets for assembly (PendSV/SVC/first_switch).
# Tracks kernel_types.h automatically; the .S files .include the result.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_LIST_DIR}/src/kernel/include/tcb_offsets.inc
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/scripts/gen_tcb_offsets.py
            --cc ${CMAKE_C_COMPILER}
            --include ${CMAKE_CURRENT_LIST_DIR}/src/kernel/include
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/gen_tcb_offsets.c
            ${CMAKE_CURRENT_LIST_DIR}/src/kernel/include/kernel_types.h
            ${CMAKE_CURRENT_LIST_DIR}/scripts/gen_tcb_offsets.py
    VERBATIM
)
add_custom_target(gen_tcb_offsets
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/kernel/include/tcb_offsets.inc)
add_dependencies(my-rtos-pico2w gen_tcb_offsets)

target_include_directories(my-rtos-pico2w PRIVATE
    src/kernel/include
    src/kernel/core
    src/kernel/task
    src/kernel/ipc
    src/kernel/timer
    src/kernel/irq
    src/kernel/mpu
    src/kernel/syscall
    src/kernel/usercopy
    src/kernel/cap
    src/kernel/fault
    src/kernel/mem
    src/kernel/vfs
    src/kernel/trace
    src/kernel/stats
    src/kernel/dev
    src/kernel
    src/hal
    src/drivers/include
    src/drivers/block
    src/board/include
    src/board/rp2350
    src/user/nameserver
    src/user/drivers
    src/user/fs
    src/user/supervisor
    src/arch/arm/cortex-m7
    src/tests
    src/app
)

target_compile_definitions(my-rtos-pico2w PRIVATE
    TARGET_BOARD=BOARD_RP2350_PICO2
    PICO_STDIO_UART=0
    PICO_STDIO_USB=0
)

target_compile_options(my-rtos-pico2w PRIVATE
    -Wall -Wextra -Werror
    -Wno-stringop-truncation
    -fno-common
    -ffunction-sections -fdata-sections
)

target_link_libraries(my-rtos-pico2w PRIVATE
    pico_stdlib
    hardware_clocks
    hardware_flash
)

target_link_options(my-rtos-pico2w PRIVATE
    "LINKER:-T,${CMAKE_CURRENT_LIST_DIR}/link/rp2350_sections.ld"
)

pico_enable_stdio_uart(my-rtos-pico2w 0)
pico_enable_stdio_usb(my-rtos-pico2w 0)
pico_set_program_name(my-rtos-pico2w "My-RTOS")
pico_set_program_version(my-rtos-pico2w "1.0.0")
pico_add_extra_outputs(my-rtos-pico2w)
