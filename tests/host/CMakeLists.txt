cmake_minimum_required(VERSION 3.13)
project(telemetry_link_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(
    -Wall
    -Wextra
    -Wconversion
    -Wno-unused-parameter
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../soldier_unit/src/features
    ${CMAKE_CURRENT_SOURCE_DIR}/../../soldier_unit/src/sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/../../soldier_unit/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../../base_station/main/state
    ${CMAKE_CURRENT_SOURCE_DIR}/../../base_station/main/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../../base_station/main
)

# ── 1. Packet Codec Golden Vectors ───────────────────────────────────────────
add_executable(test_packet_codec
    ../../shared/src/tests/host_main.c
    ../../shared/src/tests/protocol_vectors.c
    ../../shared/src/packet_codec.c
)

# ── 2. DEF-03: Soldier Button Events ──────────────────────────────────────────
add_executable(test_soldier_buttons
    test_soldier_buttons.c
    ../../soldier_unit/src/features/soldier_sm.c
    stubs/battery.c
)

# ── 3. DEF-02 & DEF-09: MPU Range & Fall Detect Sequence ──────────────────────
add_executable(test_mpu_fall_detect
    test_mpu_fall_detect.c
    ../../soldier_unit/src/sensors/fall_detect.c
    ../../soldier_unit/src/sensors/mpu6050.c
)
target_link_libraries(test_mpu_fall_detect PRIVATE m)

# ── 4. DEF-11 & MISS-02: Base State Machine Logic ─────────────────────────────
add_executable(test_base_sm_logic
    test_base_sm_logic.c
    ../../base_station/main/state/base_sm.c
    stubs/logger.c
)

# ── 5. DEF-04: Base SM Concurrency Race (pthreads) ───────────────────────────
add_executable(test_base_sm_race
    test_base_sm_race.c
    ../../base_station/main/state/base_sm.c
    stubs/logger.c
)
target_link_libraries(test_base_sm_race PRIVATE pthread)

# ── 6. DEF-01: CMake Calibration Mode Default ────────────────────────────────
add_executable(test_def01_cal_mode
    test_def01_cal_mode.c
)

# ── 7. MISS-01: Button FSM & Hold/Double-Press Semantics ─────────────────────
add_executable(test_button_fsm
    test_button_fsm.c
    ../../soldier_unit/src/ui/buttons.c
)

# ── 8. DEF-06: Config Store & Counter Reservation ────────────────────────────
add_executable(test_config_store
    test_config_store.c
    ../../soldier_unit/src/features/config_store.c
    stubs/flash_stub.c
)

# ── 9. DEF-05: Non-blocking Buzzer & Bounded UI Loop Period ────────────────────
add_executable(test_buzzer_bounded
    test_buzzer_bounded.c
    ../../base_station/main/ui/buzzer.c
)
target_link_libraries(test_buzzer_bounded PRIVATE pthread)

# ── 10. MISS-03: Asynchronous NVS Coarse High-Water Mark Persistence ──────────
add_executable(test_nvs_hwm
    test_nvs_hwm.c
)
target_link_libraries(test_nvs_hwm PRIVATE pthread)

# ── 11. DEF-07: SSD1306 OLED Rendering for Soldier 5 on Page 5 ───────────────
add_executable(test_oled_page5
    test_oled_page5.c
    ../../base_station/main/ui/oled.c
)

# ── 12. DEF-12: MultiCeiver Pipes 1-5 & Header Cross-Check ───────────────────
add_executable(test_rf_pipes
    test_rf_pipes.c
)

# ── 13. Item f: Radio Timing, ARD Stagger & RTOS Delay Retroactive Suite ─────
add_executable(test_radio_timing
    test_radio_timing.c
    ../../soldier_unit/src/features/soldier_sm.c
    stubs/battery.c
)

# ── 14. nRF24L01+ Register Model Verification ────────────────────────────────
add_executable(test_nrf24_model
    test_nrf24_model.c
)

# ── 15. MPU6050 DLPF & Sample Rate Phase-Sweep Simulation ────────────────────
add_executable(sim_dlpf
    sim_dlpf.c
)
target_link_libraries(sim_dlpf PRIVATE m)

# ── 16. Multi-Agent ESB Link Simulation (Gate G4 Item B) ──────────────────────
add_executable(sim_esb_link
    sim_esb_link.c
    ../../shared/src/packet_codec.c
    ../../soldier_unit/src/features/soldier_sm.c
    ../../base_station/main/state/base_sm.c
    stubs/logger.c
    stubs/battery.c
)
target_link_libraries(sim_esb_link PRIVATE m)

# ── 17. Stateful Property Testing (Gate G4 Item C) ───────────────────────────
add_executable(test_base_sm_property
    test_base_sm_property.c
    ../../shared/src/packet_codec.c
    ../../base_station/main/state/base_sm.c
    stubs/logger.c
)

# ── 18. SisFall Evaluation Harness Linking Real C Firmware ───────────────────
add_executable(eval_sisfall
    eval_sisfall.c
    ../../soldier_unit/src/sensors/fall_detect.c
    ../../soldier_unit/src/sensors/mpu6050.c
    ../../soldier_unit/src/sensors/fall_features.c
)
target_link_libraries(eval_sisfall PRIVATE m)

enable_testing()
add_test(NAME test_packet_codec COMMAND test_packet_codec)
add_test(NAME test_soldier_buttons COMMAND test_soldier_buttons)
add_test(NAME test_mpu_fall_detect COMMAND test_mpu_fall_detect)
add_test(NAME test_def01_cal_mode COMMAND test_def01_cal_mode)
add_test(NAME test_button_fsm COMMAND test_button_fsm)
add_test(NAME test_config_store COMMAND test_config_store)
add_test(NAME test_base_sm_logic COMMAND test_base_sm_logic)
add_test(NAME test_base_sm_race COMMAND test_base_sm_race)
add_test(NAME test_buzzer_bounded COMMAND test_buzzer_bounded)
add_test(NAME test_nvs_hwm COMMAND test_nvs_hwm)
add_test(NAME test_oled_page5 COMMAND test_oled_page5)
add_test(NAME test_rf_pipes COMMAND test_rf_pipes)
add_test(NAME test_radio_timing COMMAND test_radio_timing)
add_test(NAME test_nrf24_model COMMAND test_nrf24_model)
add_test(NAME sim_dlpf COMMAND sim_dlpf)
add_test(NAME sim_esb_link COMMAND sim_esb_link)
add_test(NAME test_base_sm_property COMMAND test_base_sm_property)

# ── 19. AES-128-CCM Tamper & Authentication Test ─────────────────────────────
add_executable(test_aes_ccm_tamper
    test_aes_ccm_tamper.c
    ../../shared/src/packet_codec.c
    ../../soldier_unit/src/crypto/aes_wrap.c
    ../../soldier_unit/lib/tinycrypt/lib/source/aes_encrypt.c
    ../../soldier_unit/lib/tinycrypt/lib/source/aes_decrypt.c
    ../../soldier_unit/lib/tinycrypt/lib/source/ccm_mode.c
    ../../soldier_unit/lib/tinycrypt/lib/source/utils.c
)
target_include_directories(test_aes_ccm_tamper PRIVATE
    ../../soldier_unit/lib/tinycrypt/lib/include
)
add_test(NAME test_aes_ccm_tamper COMMAND test_aes_ccm_tamper)


