#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/tests/host/build"

# Check if --sanitizers flag is passed
if [ "$1" = "--sanitizers" ]; then
    echo "=========================================================="
    echo "  Running Host Sanitizer Suite (ASan / UBSan / TSan)"
    echo "=========================================================="
    
    SAN_DIR="${SCRIPT_DIR}/tests/host/build_san"
    mkdir -p "${SAN_DIR}"
    
    COMMON_INC=(
        "-I${SCRIPT_DIR}/tests/host/stubs"
        "-I${SCRIPT_DIR}/shared/include"
        "-I${SCRIPT_DIR}/shared/src"
        "-I${SCRIPT_DIR}/soldier_unit/src/features"
        "-I${SCRIPT_DIR}/soldier_unit/src/sensors"
        "-I${SCRIPT_DIR}/soldier_unit/src/ui"
        "-I${SCRIPT_DIR}/base_station/main/state"
        "-I${SCRIPT_DIR}/base_station/main/ui"
        "-I${SCRIPT_DIR}/base_station/main"
    )
    ASAN_FLAGS=(-fsanitize=address,undefined -g -O1 -Wall -Wextra)
    
    echo "[1/15] ASan/UBSan: test_packet_codec"
    gcc "${ASAN_FLAGS[@]}" -I"${SCRIPT_DIR}/shared/include" -I"${SCRIPT_DIR}/shared/src" \
        "${SCRIPT_DIR}/shared/src/tests/host_main.c" \
        "${SCRIPT_DIR}/shared/src/tests/protocol_vectors.c" \
        "${SCRIPT_DIR}/shared/src/packet_codec.c" \
        -o "${SAN_DIR}/test_packet_codec_asan"
    "${SAN_DIR}/test_packet_codec_asan"
    
    echo "[2/15] ASan/UBSan: test_soldier_buttons"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_soldier_buttons.c" \
        "${SCRIPT_DIR}/soldier_unit/src/features/soldier_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/battery.c" \
        -o "${SAN_DIR}/test_soldier_buttons_asan"
    "${SAN_DIR}/test_soldier_buttons_asan"
    
    echo "[3/15] ASan/UBSan: test_mpu_fall_detect"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_mpu_fall_detect.c" \
        "${SCRIPT_DIR}/soldier_unit/src/sensors/fall_detect.c" \
        "${SCRIPT_DIR}/soldier_unit/src/sensors/mpu6050.c" \
        -lm -o "${SAN_DIR}/test_mpu_fall_detect_asan"
    "${SAN_DIR}/test_mpu_fall_detect_asan"
    
    echo "[4/15] ASan/UBSan: test_button_fsm"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_button_fsm.c" \
        "${SCRIPT_DIR}/soldier_unit/src/ui/buttons.c" \
        -o "${SAN_DIR}/test_button_fsm_asan"
    "${SAN_DIR}/test_button_fsm_asan"
    
    echo "[5/15] ASan/UBSan: test_def01_cal_mode"
    gcc "${ASAN_FLAGS[@]}" -I"${SCRIPT_DIR}/tests/host" \
        "${SCRIPT_DIR}/tests/host/test_def01_cal_mode.c" \
        -o "${SAN_DIR}/test_def01_cal_mode_asan"
    "${SAN_DIR}/test_def01_cal_mode_asan"
    
    echo "[6/15] ASan/UBSan: test_config_store"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_config_store.c" \
        "${SCRIPT_DIR}/soldier_unit/src/features/config_store.c" \
        "${SCRIPT_DIR}/tests/host/stubs/flash_stub.c" \
        -o "${SAN_DIR}/test_config_store_asan"
    "${SAN_DIR}/test_config_store_asan"
    
    echo "[7/15] ASan/UBSan: test_base_sm_logic"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_base_sm_logic.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/logger.c" \
        -o "${SAN_DIR}/test_base_sm_logic_asan"
    "${SAN_DIR}/test_base_sm_logic_asan"
    
    echo "[8/15] ASan/UBSan: test_base_sm_race"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_base_sm_race.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/logger.c" \
        -lpthread -o "${SAN_DIR}/test_base_sm_race_asan"
    "${SAN_DIR}/test_base_sm_race_asan"

    echo "[9/15] ASan/UBSan: test_buzzer_bounded"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_buzzer_bounded.c" \
        "${SCRIPT_DIR}/base_station/main/ui/buzzer.c" \
        -lpthread -o "${SAN_DIR}/test_buzzer_bounded_asan"
    "${SAN_DIR}/test_buzzer_bounded_asan"
    
    echo "[10/15] ASan/UBSan: test_nvs_hwm"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_nvs_hwm.c" \
        -lpthread -o "${SAN_DIR}/test_nvs_hwm_asan"
    "${SAN_DIR}/test_nvs_hwm_asan"
    
    echo "[11/15] ASan/UBSan: test_oled_page5"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_oled_page5.c" \
        "${SCRIPT_DIR}/base_station/main/ui/oled.c" \
        -o "${SAN_DIR}/test_oled_page5_asan"
    "${SAN_DIR}/test_oled_page5_asan"
    
    echo "[12/15] ASan/UBSan: test_rf_pipes"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_rf_pipes.c" \
        -o "${SAN_DIR}/test_rf_pipes_asan"
    "${SAN_DIR}/test_rf_pipes_asan"

    echo "[13/15] ASan/UBSan: test_radio_timing"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_radio_timing.c" \
        "${SCRIPT_DIR}/soldier_unit/src/features/soldier_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/battery.c" \
        -o "${SAN_DIR}/test_radio_timing_asan"
    "${SAN_DIR}/test_radio_timing_asan"

    echo "[14/15] ASan/UBSan: test_nrf24_model"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_nrf24_model.c" \
        -o "${SAN_DIR}/test_nrf24_model_asan"
    "${SAN_DIR}/test_nrf24_model_asan"

    echo "[15/16] ASan/UBSan: sim_dlpf"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/sim_dlpf.c" \
        -lm -o "${SAN_DIR}/sim_dlpf_asan"
    "${SAN_DIR}/sim_dlpf_asan" > /dev/null
    echo "  [PASS] sim_dlpf under ASan/UBSan"
    
    echo "[16/17] ASan/UBSan: sim_esb_link"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/sim_esb_link.c" \
        "${SCRIPT_DIR}/shared/src/packet_codec.c" \
        "${SCRIPT_DIR}/soldier_unit/src/features/soldier_sm.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/logger.c" \
        "${SCRIPT_DIR}/tests/host/stubs/battery.c" \
        -lm -o "${SAN_DIR}/sim_esb_link_asan"
    "${SAN_DIR}/sim_esb_link_asan" > /dev/null
    echo "  [PASS] sim_esb_link under ASan/UBSan"
    
    echo "[17/18] ASan/UBSan: test_base_sm_property"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_base_sm_property.c" \
        "${SCRIPT_DIR}/shared/src/packet_codec.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/logger.c" \
        -o "${SAN_DIR}/test_base_sm_property_asan"
    "${SAN_DIR}/test_base_sm_property_asan" > /dev/null
    echo "  [PASS] test_base_sm_property under ASan/UBSan"

    echo "[18/18] ASan/UBSan: test_aes_ccm_tamper"
    gcc "${ASAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        -I"${SCRIPT_DIR}/soldier_unit/lib/tinycrypt/lib/include" \
        "${SCRIPT_DIR}/tests/host/test_aes_ccm_tamper.c" \
        "${SCRIPT_DIR}/shared/src/packet_codec.c" \
        "${SCRIPT_DIR}/soldier_unit/src/crypto/aes_wrap.c" \
        "${SCRIPT_DIR}/soldier_unit/lib/tinycrypt/lib/source/aes_encrypt.c" \
        "${SCRIPT_DIR}/soldier_unit/lib/tinycrypt/lib/source/aes_decrypt.c" \
        "${SCRIPT_DIR}/soldier_unit/lib/tinycrypt/lib/source/ccm_mode.c" \
        "${SCRIPT_DIR}/soldier_unit/lib/tinycrypt/lib/source/utils.c" \
        -o "${SAN_DIR}/test_aes_ccm_tamper_asan"
    "${SAN_DIR}/test_aes_ccm_tamper_asan" > /dev/null
    echo "  [PASS] test_aes_ccm_tamper under ASan/UBSan"
    
    echo ""
    echo "--- ThreadSanitizer (TSan) Multi-threading & Queue Validation ---"
    TSAN_FLAGS=(-fsanitize=thread -g -O1 -Wall -Wextra)

    echo "[TSan 1/3] ThreadSanitizer: test_base_sm_race (s_soldiers Mutex Concurrency)"
    gcc "${TSAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_base_sm_race.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c" \
        "${SCRIPT_DIR}/tests/host/stubs/logger.c" \
        -lpthread -o "${SAN_DIR}/test_base_sm_race_tsan"
    setarch x86_64 -R "${SAN_DIR}/test_base_sm_race_tsan" || true

    echo "[TSan 2/3] ThreadSanitizer: test_buzzer_bounded (UI Async Worker Thread & Queue)"
    gcc "${TSAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_buzzer_bounded.c" \
        "${SCRIPT_DIR}/base_station/main/ui/buzzer.c" \
        -lpthread -o "${SAN_DIR}/test_buzzer_bounded_tsan"
    setarch x86_64 -R "${SAN_DIR}/test_buzzer_bounded_tsan" || true

    echo "[TSan 3/3] ThreadSanitizer: test_nvs_hwm (NVS Async Writer Queue & Threads)"
    gcc "${TSAN_FLAGS[@]}" "${COMMON_INC[@]}" \
        "${SCRIPT_DIR}/tests/host/test_nvs_hwm.c" \
        -lpthread -o "${SAN_DIR}/test_nvs_hwm_tsan"
    setarch x86_64 -R "${SAN_DIR}/test_nvs_hwm_tsan" || true
    
    echo ""
    echo "--- Cppcheck Static Analysis ---"
    cppcheck --enable=warning,portability,style --inline-suppr --error-exitcode=1 \
        -I "${SCRIPT_DIR}/shared/include" \
        -I "${SCRIPT_DIR}/soldier_unit/src/features" \
        -I "${SCRIPT_DIR}/soldier_unit/src/sensors" \
        -I "${SCRIPT_DIR}/base_station/main/state" \
        -I "${SCRIPT_DIR}/base_station/main/ui" \
        "${SCRIPT_DIR}/shared/src" \
        "${SCRIPT_DIR}/soldier_unit/src/features/soldier_sm.c" \
        "${SCRIPT_DIR}/soldier_unit/src/sensors/fall_detect.c" \
        "${SCRIPT_DIR}/base_station/main/state/base_sm.c"

    echo "=========================================================="
    echo "Sanitizer and Static Analysis Suite completed successfully."
    exit 0
fi

if [ "$1" = "--idf" ]; then
    echo "=========================================================="
    echo "  Building ESP32-S3 Target (idf.py build)"
    echo "=========================================================="
    cd "${SCRIPT_DIR}/base_station"
    idf.py build
    exit 0
fi

echo "=========================================================="
echo "  RP2040 - ESP32-S3 Telemetry Link Validation Suite"
echo "=========================================================="

echo "[1/2] Building host validation suite..."
cmake -B "${BUILD_DIR}" -S "${SCRIPT_DIR}/tests/host" -G Ninja
ninja -C "${BUILD_DIR}"

echo "[2/2] Running host test targets..."
FAILURES=0

run_test() {
    local name="$1"
    local bin="${BUILD_DIR}/$1"
    echo ""
    echo "----------------------------------------------------------"
    echo "Running: ${name}"
    echo "----------------------------------------------------------"
    if "${bin}"; then
        echo "[PASS] ${name}"
    else
        echo "[FAIL] ${name} (exit code $?)"
        FAILURES=$((FAILURES + 1))
    fi
}

run_test "test_packet_codec"
run_test "test_soldier_buttons"
run_test "test_mpu_fall_detect"
run_test "test_base_sm_logic"
run_test "test_base_sm_race"
run_test "test_def01_cal_mode"
run_test "test_button_fsm"
run_test "test_config_store"
run_test "test_buzzer_bounded"
run_test "test_nvs_hwm"
run_test "test_oled_page5"
run_test "test_rf_pipes"
run_test "test_radio_timing"
run_test "test_nrf24_model"
run_test "sim_dlpf"
run_test "sim_esb_link"
run_test "test_base_sm_property"
run_test "test_aes_ccm_tamper"

echo ""
echo "=========================================================="
if [ ${FAILURES} -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "TEST FAILURES DETECTED: ${FAILURES} test(s) failed."
    exit ${FAILURES}
fi
