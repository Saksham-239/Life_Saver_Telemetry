cmake_minimum_required(VERSION 3.13)

set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)
set(DOXYGEN_FOUND FALSE)
set(PICO_SDK_TOP_LEVEL_PROJECT FALSE)

# ── Pico SDK bootstrap (must be before project()) ────────────────────────────
# Set PICO_SDK_PATH in your environment or pass -DPICO_SDK_PATH=... to cmake.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(soldier_unit C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# ── Shared include path ───────────────────────────────────────────────────────
set(SHARED_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include)
set(SHARED_SRC     ${CMAKE_CURRENT_SOURCE_DIR}/../shared/src)

# ── tinycrypt library ─────────────────────────────────────────────────────────
# Clone into soldier_unit/lib/tinycrypt:
#   git clone https://github.com/intel/tinycrypt.git lib/tinycrypt
# Files needed from tinycrypt/lib/source/:
#   aes_encrypt.c aes_decrypt.c ccm_mode.c utils.c
# Headers in tinycrypt/lib/include/tinycrypt/

set(TINYCRYPT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinycrypt/lib)

add_library(tinycrypt STATIC
    ${TINYCRYPT_DIR}/source/aes_encrypt.c
    ${TINYCRYPT_DIR}/source/aes_decrypt.c
    ${TINYCRYPT_DIR}/source/ccm_mode.c
    ${TINYCRYPT_DIR}/source/utils.c
)
target_include_directories(tinycrypt PUBLIC
    ${TINYCRYPT_DIR}/include
)

# ── FreeRTOS-Kernel ─────────────────────────────────────────────────────────────
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/FreeRTOS-Kernel)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src) # For FreeRTOSConfig.h
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(soldier_unit
    main.c

    # Radio
    src/radio/nrf24_spi.c
    src/rtos/task_radio.c

    # Crypto
    src/crypto/aes_wrap.c

    # Sensors
    src/sensors/mpu6050.c
    src/sensors/fall_detect.c
    src/rtos/task_sensor.c

    # Features
    src/features/config_store.c
    src/features/battery.c
    src/features/soldier_sm.c
    src/rtos/task_sm.c

    # UI
    src/ui/buttons.c
    src/ui/vibration.c
    src/rtos/task_button.c
    src/rtos/task_vibration.c

    # Shared codec
    ${SHARED_SRC}/packet_codec.c

    # §16 test vectors (compiled into main for boot self-test)
    ${SHARED_SRC}/tests/protocol_vectors.c
)

target_include_directories(soldier_unit PRIVATE
    ${SHARED_INCLUDE}
    ${SHARED_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/src/features
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_link_libraries(soldier_unit PRIVATE
    pico_stdlib
    pico_time
    pico_multicore      # For cross-core lockouts
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_watchdog
    pico_unique_id      # For board serial (optional, useful for provisioning)
    tinycrypt
    m                   # libm for sqrtf / acosf in mpu6050.c and fall_detect.c
    FreeRTOS-Kernel-Heap4
)

# ── stdio: enable both USB CDC and UART0 ─────────────────────────────────────
pico_enable_stdio_usb(soldier_unit 1)
pico_enable_stdio_uart(soldier_unit 1)

# ── Output: .uf2 + .hex + .bin for flashing ──────────────────────────────────
# Override pico_add_map_output to avoid GNU ld failing on Unicode dash in project path
function(pico_add_map_output TARGET)
endfunction()

pico_add_extra_outputs(soldier_unit)

# ── Build type ────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(FALL_CALIBRATION_MODE "Enable fall sensor calibration mode" OFF)

# ── Compiler flags ────────────────────────────────────────────────────────────
target_compile_options(soldier_unit PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

if(FALL_CALIBRATION_MODE)
    target_compile_definitions(soldier_unit PRIVATE FALL_CALIBRATION_MODE=1)
endif()

# ── Optional: print build type ────────────────────────────────────────────────
message(STATUS "soldier_unit build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Pico SDK version: ${PICO_SDK_VERSION_STRING}")
# ── Host/Target Unit Tests ──────────────────────────────────────────────────
add_executable(test_ccm_rp2040
    src/tests/test_ccm_rp2040.c
    src/crypto/aes_wrap.c
    ${SHARED_SRC}/packet_codec.c
)
target_include_directories(test_ccm_rp2040 PRIVATE 
    ${SHARED_INCLUDE}
    ${TINYCRYPT_DIR}/include
)
target_link_libraries(test_ccm_rp2040 pico_stdlib tinycrypt)
pico_add_extra_outputs(test_ccm_rp2040)
