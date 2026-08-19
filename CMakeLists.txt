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
# test_elf_app.c is compiled separately as a freestanding ELF (see custom
# command below), NOT linked into the kernel image.
list(REMOVE_ITEM MYRTOS_USER_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/user/apps/test_elf_app.c")
# Tests are tiered: k/ (kernel invariants), abi/ (syscall contract),
# sys/ (boot health). See docs/TEST_FRAMEWORK.md.
file(GLOB MYRTOS_TEST_FRAMEWORK CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/*.c")
file(GLOB MYRTOS_TEST_K_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/k/*.c")
file(GLOB MYRTOS_TEST_ABI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/abi/*.c")
file(GLOB MYRTOS_TEST_SYS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/sys/*.c")
list(REMOVE_ITEM MYRTOS_TEST_FRAMEWORK
    "${CMAKE_CURRENT_LIST_DIR}/src/tests/test_example.c")
set(MYRTOS_TEST_SOURCES
    ${MYRTOS_TEST_FRAMEWORK}
    ${MYRTOS_TEST_K_SOURCES}
    ${MYRTOS_TEST_ABI_SOURCES}
    ${MYRTOS_TEST_SYS_SOURCES})

set(MYRTOS_SOURCES
    ${MYRTOS_KERNEL_SOURCES}
    ${MYRTOS_USER_SOURCES}
    ${MYRTOS_TEST_SOURCES}
    src/app/main.c
    src/app/shell.c
    src/arch/arm/cortex-m/hal.c
    src/arch/arm/cortex-m/pendsv_handler.S
    src/arch/arm/cortex-m/svc_handler.S
    src/arch/arm/cortex-m/first_switch.S
    src/startup/arm/cortex-m7/fault_handlers.S
    src/drivers/chip/rp2350/uart_rp2350.c
    src/drivers/chip/rp2350/gpio_rp2350.c
    src/drivers/uart_dev.c
    src/drivers/block/flash_block.c
    src/drivers/block/block_dev.c
    src/kernel/loader/elf_loader.c
    src/board/rp2350/board.c
    src/board/rp2350/board_drivers.c
)

add_executable(my-rtos-pico2w ${MYRTOS_SOURCES})

# ---- Build the test ELF app (freestanding, separate from the kernel) ----
# The test ELF is a tiny program linked to the flash FS region. It's embedded
# into the firmware via .incbin so elf_load() can parse+execute it.
set(TEST_ELF_DIR ${CMAKE_CURRENT_LIST_DIR}/build/test_elf)
set(TEST_ELF_PATH ${TEST_ELF_DIR}/test_elf.elf)
set(TOOLCHAIN_BIN ${CMAKE_CURRENT_LIST_DIR}/tools/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin)
add_custom_command(
    OUTPUT ${TEST_ELF_PATH}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_ELF_DIR}
    COMMAND ${TOOLCHAIN_BIN}/arm-none-eabi-gcc
        -mcpu=cortex-m33 -mthumb -ffreestanding -nostdlib -Os
        -c ${CMAKE_CURRENT_LIST_DIR}/src/user/apps/test_elf_app.c
        -o ${TEST_ELF_DIR}/test_elf_app.o
    COMMAND ${TOOLCHAIN_BIN}/arm-none-eabi-ld
        -T ${CMAKE_CURRENT_LIST_DIR}/src/user/apps/test_elf_app.ld
        ${TEST_ELF_DIR}/test_elf_app.o
        -o ${TEST_ELF_PATH}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/user/apps/test_elf_app.c
            ${CMAKE_CURRENT_LIST_DIR}/src/user/apps/test_elf_app.ld
    COMMENT "Building test ELF app"
)
add_custom_target(test_elf_blob DEPENDS ${TEST_ELF_PATH})

# Always add the blob source; the #if ELF_LOADER guard in the file handles
# enable/disable. Kconfig defines ELF_LOADER in kernel_config.h, not CMake.
# CRITICAL: set the blob .c file's OBJECT_DEPENDS on the test ELF, so CMake
# recompiles test_elf_blob.c whenever the embedded ELF changes. Without this,
# .incbin embeds stale data (CMake doesn't track .incbin file dependencies).
target_compile_definitions(my-rtos-pico2w PRIVATE
    TEST_ELF_PATH=${TEST_ELF_PATH})
set_source_files_properties(src/kernel/loader/test_elf_blob.c PROPERTIES
    OBJECT_DEPENDS ${TEST_ELF_PATH})
target_sources(my-rtos-pico2w PRIVATE
    src/kernel/loader/test_elf_blob.c)
add_dependencies(my-rtos-pico2w test_elf_blob)

# Generate TCB field offsets for assembly (PendSV/SVC/first_switch).
# Tracks kernel_types.h automatically; the .S files .include the result.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_LIST_DIR}/include/kernel/tcb_offsets.inc
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/scripts/gen_tcb_offsets.py
            --cc ${CMAKE_C_COMPILER}
            --include ${CMAKE_CURRENT_LIST_DIR}/include/kernel
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/gen_tcb_offsets.c
            ${CMAKE_CURRENT_LIST_DIR}/include/kernel/kernel_types.h
            ${CMAKE_CURRENT_LIST_DIR}/include/kernel/kernel_config.h
            ${CMAKE_CURRENT_LIST_DIR}/scripts/gen_tcb_offsets.py
    VERBATIM
)
add_custom_target(gen_tcb_offsets
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/include/kernel/tcb_offsets.inc)
add_dependencies(my-rtos-pico2w gen_tcb_offsets)

target_include_directories(my-rtos-pico2w PRIVATE
    include/kernel
    include/drivers
    include/board
    include/app
    src/kernel/ipc
    src/kernel/vfs
    src/kernel
    src/board/rp2350
    src/user/nameserver
    src/user/drivers
    src/user/fs
    src/user/supervisor
    src/arch/arm/cortex-m
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
    pico_multicore
)

target_link_options(my-rtos-pico2w PRIVATE
    "LINKER:-T,${CMAKE_CURRENT_LIST_DIR}/link/rp2350_sections.ld"
)

pico_enable_stdio_uart(my-rtos-pico2w 0)
pico_enable_stdio_usb(my-rtos-pico2w 0)
pico_set_program_name(my-rtos-pico2w "My-RTOS")
pico_set_program_version(my-rtos-pico2w "1.0.0")
pico_add_extra_outputs(my-rtos-pico2w)
