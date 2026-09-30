# flydigi_bridge — RP2350 飞智接收器 → macOS 原生手柄桥
#
#   输入：USB-A 走 PIO 软 USB host（Pico-PIO-USB）读飞智 2.4G 接收器
#   输出：USB-C 原生 USB device，冒充 Flydigi Vader2Pro.MobileUSB
#
# 构建：
#   export PICO_SDK_PATH=~/pico/pico-sdk
#   export PATH=~/pico/toolchain/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin:$PATH
#   cmake -B build-apple-compatible -G Ninja \
#     -DAPPLE_COMPATIBLE_PROFILE=ON -DCMAKE_BUILD_TYPE=Release
#   ninja -C build-apple-compatible
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pico-PIO-USB 位置（可用 -DPICO_PIO_USB_PATH=... 覆盖）
if(NOT DEFINED PICO_PIO_USB_PATH)
    set(PICO_PIO_USB_PATH "$ENV{HOME}/pico/Pico-PIO-USB")
endif()

# 板型：Waveshare RP2350-USB-A 与 pico2 同芯片（RP2350A）；DP=GP12 / DM=GP13
set(PICO_BOARD pico2 CACHE STRING "Board type")

# core1 跑完整的 TinyUSB host + PIO-USB 初始化，SDK 默认给它 2 KB 栈不够：
# 栈溢出会让 core1 静默停住（core0 看到的 .bss 计数器仍正常，极易误判为"库卡死"）。
# 注意这是编译期宏（写在 pico_multicore.h 里），必须走 add_compile_definitions，
# 放 CMake 的 set(...CACHE) 是无效的 —— 栈大小由链接脚本按宏算出来。
# 栈空间：SDK 默认把 core0/core1 两个栈都塞进 SCRATCH_X / SCRATCH_Y，各只有 4 KB。
# core1 要跑完整的 TinyUSB host + PIO-USB 初始化，实测栈会溢出（探针抓到 core1 的 sp
# 越过 __StackOneTop 进了 core0 的栈区）→ core1 静默停住，表现为“库卡死”。
# 注意 PICO_CORE1_STACK_SIZE / PICO_STACK_SIZE 是编译期宏，必须走
# add_compile_definitions（写进 CMake 的 set(...CACHE) 无效，栈大小由链接脚本按宏算）。
#
# 两个栈都放 SCRATCH 各自 4 KB 内 —— 这是 SDK 唯一不越界的合法配置；
# core1 需要更大栈时必须自备栈区（见 PICO_CORE1_STACK_SIZE=0 的用法）。
# core1 栈自备（放主 RAM），所以 SDK 不再为它在 SCRATCH_X 里预留：
# PICO_CORE1_STACK_SIZE=0 → 用主 RAM 里的 s_core1_stack，见 main.c 的
# multicore_launch_core1_with_stack()。core0 栈留在 SCRATCH_Y（2 KB 够）。
add_compile_definitions(
    PICO_CORE1_STACK_SIZE=0
    PICO_STACK_SIZE=0x800
    PICO_PANIC_FUNCTION=brg_panic
    PICO_PANIC_FUNCTION_DOES_NOT_RETURN=1
)

include(pico_sdk_import.cmake)

project(flydigi_bridge C CXX ASM)
pico_sdk_init()

option(GENERIC_GAMEPAD "Build standard Generic Game Pad identity for A/B testing" OFF)
option(GENERIC_PID_PROBE "Experimental Generic HID PID Force Feedback descriptor" OFF)
option(CHROMIUM_STADIA_PROBE "Experimental Chromium Stadia dual-rumble identity" OFF)
option(GENERIC_APPLE_IDENTITY "Experimental 26-button Apple-order report under Vader2Pro identity" OFF)
option(CORSAIR_APPLE_IDENTITY "Experimental 29-button SCUF Omega Apple model" OFF)
option(CORSAIR_KNOWN_USB_ID "Probe SCUF report under an already approved Vader2Pro USB ID" OFF)
option(APPLE_COMPATIBLE_PROFILE "Build the validated 26-button Apple-compatible profile" OFF)
option(APPLE_BATTERY_PROBE "Add a standard HID Battery Strength input report to the Apple profile" OFF)
option(APPLE_BATTERY_INTERFACE_PROBE "Add a separate HID Battery Strength interface to the Apple profile" OFF)

# This is the production profile for macOS Game Controllers: keep the
# Generic 26-button report (extra Vader keys and WebHID) while using the
# Vader2Pro USB identity that macOS actually instantiates as GCController.
# Keep the low-level switches available for isolated protocol experiments, but
# make the tested combination a single, hard-to-misconfigure build target.
if(APPLE_COMPATIBLE_PROFILE)
    if(GENERIC_PID_PROBE OR CHROMIUM_STADIA_PROBE OR CORSAIR_APPLE_IDENTITY OR CORSAIR_KNOWN_USB_ID)
        message(FATAL_ERROR "APPLE_COMPATIBLE_PROFILE cannot be combined with PID, Stadia, or SCUF experiments")
    endif()
    set(GENERIC_GAMEPAD ON CACHE BOOL "" FORCE)
    set(GENERIC_APPLE_IDENTITY ON CACHE BOOL "" FORCE)
endif()

if((APPLE_BATTERY_PROBE OR APPLE_BATTERY_INTERFACE_PROBE) AND
   (NOT APPLE_COMPATIBLE_PROFILE OR NOT GENERIC_APPLE_IDENTITY))
    message(FATAL_ERROR "Apple battery probes require APPLE_COMPATIBLE_PROFILE")
endif()
if(APPLE_BATTERY_PROBE AND APPLE_BATTERY_INTERFACE_PROBE)
    message(FATAL_ERROR "Select either the legacy in-game-report battery probe or the isolated interface probe")
endif()

# The validated Apple profile has its own Flash sector. Keep all descriptor
# probes transient so switching identities cannot rewrite a daily-use map.
if(APPLE_COMPATIBLE_PROFILE AND NOT APPLE_BATTERY_PROBE AND NOT APPLE_BATTERY_INTERFACE_PROBE)
    add_compile_definitions(APPLE_PERSISTENT_CONFIG=1)
endif()

if((GENERIC_APPLE_IDENTITY OR CORSAIR_APPLE_IDENTITY) AND NOT GENERIC_GAMEPAD)
    message(FATAL_ERROR "Apple identity experiments require GENERIC_GAMEPAD")
endif()
if(GENERIC_APPLE_IDENTITY AND CORSAIR_APPLE_IDENTITY)
    message(FATAL_ERROR "Select only one Apple identity experiment")
endif()
if(CORSAIR_KNOWN_USB_ID AND NOT CORSAIR_APPLE_IDENTITY)
    message(FATAL_ERROR "CORSAIR_KNOWN_USB_ID requires CORSAIR_APPLE_IDENTITY")
endif()
if(GENERIC_PID_PROBE AND (NOT GENERIC_GAMEPAD OR GENERIC_APPLE_IDENTITY OR CORSAIR_APPLE_IDENTITY))
    message(FATAL_ERROR "GENERIC_PID_PROBE requires the plain GENERIC_GAMEPAD identity")
endif()
if(CHROMIUM_STADIA_PROBE AND (NOT GENERIC_GAMEPAD OR GENERIC_PID_PROBE OR GENERIC_APPLE_IDENTITY OR CORSAIR_APPLE_IDENTITY))
    message(FATAL_ERROR "CHROMIUM_STADIA_PROBE requires the plain GENERIC_GAMEPAD report")
endif()
if(GENERIC_GAMEPAD)
    add_compile_definitions(GENERIC_GAMEPAD=1)
endif()
if(GENERIC_PID_PROBE)
    add_compile_definitions(GENERIC_PID_PROBE=1)
endif()
if(CHROMIUM_STADIA_PROBE)
    add_compile_definitions(CHROMIUM_STADIA_PROBE=1)
endif()
if(GENERIC_APPLE_IDENTITY)
    add_compile_definitions(GENERIC_APPLE_IDENTITY=1)
endif()
if(CORSAIR_APPLE_IDENTITY)
    add_compile_definitions(CORSAIR_APPLE_IDENTITY=1)
endif()
if(CORSAIR_KNOWN_USB_ID)
    add_compile_definitions(CORSAIR_KNOWN_USB_ID=1)
endif()
if(APPLE_BATTERY_PROBE)
    add_compile_definitions(APPLE_BATTERY_PROBE=1)
endif()
if(APPLE_BATTERY_INTERFACE_PROBE)
    add_compile_definitions(APPLE_BATTERY_INTERFACE_PROBE=1)
endif()

# Pico-PIO-USB：本版本 CMakeLists 在仓库根（提供 pico_pio_usb INTERFACE 目标）
add_subdirectory(${PICO_PIO_USB_PATH} ${CMAKE_CURRENT_BINARY_DIR}/pio_usb)

# 全局定义（库与应用都要看到同一套；PIO-USB 靠这些宏裁剪代码）
add_compile_definitions(
    PICO_PIO_USB=1
    PIO_USB_HOST=1
    PIO_USB_DP_PIN_DEFAULT=12
)

# 复用已通过主机侧验证的模块（firmware/src，见 firmware/verify.sh 第 7 步）
set(FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(FW_SRC  ${FW_ROOT}/src)

add_executable(flydigi_bridge
    main.c
    flydigi_host.c
    app_driver.c
    usb_descriptors.c
    ${FW_SRC}/flydigi_rx.c
    ${FW_SRC}/bridge_map.c
    ${FW_SRC}/bridge_config.c
    ${FW_SRC}/bridge_timing.c
    ${FW_SRC}/report_pack.c
    ${FW_SRC}/rumble_report.c
    # PIO-USB 的 tinyusb HCD 端口层（本 SDK 版本需显式加入）
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
if(GENERIC_PID_PROBE)
    target_sources(flydigi_bridge PRIVATE ${FW_SRC}/pid_probe_report.c)
endif()

target_include_directories(flydigi_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${FW_ROOT}
    ${FW_SRC}
)

pico_set_program_name(flydigi_bridge "flydigi_bridge")
pico_set_program_version(flydigi_bridge "0.1.0")

# 日志走自定义 CDC（接口 1/2），不启用 SDK stdio，避免描述符冲突
pico_enable_stdio_uart(flydigi_bridge 0)
pico_enable_stdio_usb(flydigi_bridge 0)

target_link_libraries(flydigi_bridge
    pico_stdlib
    pico_unique_id          # 序列号（设备描述符里要）
    pico_pio_usb
    hardware_dma
    hardware_pio
    hardware_timer
    hardware_clocks
    pico_flash
    tinyusb_device
    tinyusb_host
)

# 编译期就打开警告，屏蔽掉不属于我们的第三方源码
target_compile_options(flydigi_bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
# The Pico SDK's TinyUSB target injects CFG_TUSB_DEBUG=0 globally.  Undefine
# that interface definition for this executable before selecting the diagnostic
# level, otherwise the value in tusb_config.h is silently left at zero.
target_compile_options(flydigi_bridge PRIVATE
    -UCFG_TUSB_DEBUG -DCFG_TUSB_DEBUG=2
    -UCFG_TUD_LOG_LEVEL -DCFG_TUD_LOG_LEVEL=0
    -UCFG_TUH_LOG_LEVEL -DCFG_TUH_LOG_LEVEL=2
)

# A new USB identity may need a visible macOS accessory approval prompt.
if(CORSAIR_APPLE_IDENTITY OR CHROMIUM_STADIA_PROBE)
    target_compile_definitions(flydigi_bridge PRIVATE BRINGUP_BOOTSEL_FALLBACK_MS=60000)
else()
    target_compile_definitions(flydigi_bridge PRIVATE BRINGUP_BOOTSEL_FALLBACK_MS=15000)
endif()

pico_add_extra_outputs(flydigi_bridge)
