#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

int main(void) {
    printf("=== TEST DEF-01: Fall Calibration Mode CMake Configuration ===\n");

    const char *paths[] = {
        "soldier_unit/CMakeLists.txt",
        "../../soldier_unit/CMakeLists.txt",
        "../../../soldier_unit/CMakeLists.txt",
        "../soldier_unit/CMakeLists.txt"
    };

    FILE *f = NULL;
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }

    if (!f) {
        fprintf(stderr, "Error: cannot open soldier_unit/CMakeLists.txt\n");
        return 2;
    }

    char line[512];
    bool has_hardcoded_def = false;
    bool has_cmake_option  = false;
    bool option_default_off = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "-DFALL_CALIBRATION_MODE=1") != NULL) {
            has_hardcoded_def = true;
        }
        if (strstr(line, "option(FALL_CALIBRATION_MODE") != NULL) {
            has_cmake_option = true;
            if (strstr(line, "OFF") != NULL) {
                option_default_off = true;
            }
        }
    }
    fclose(f);

    if (has_hardcoded_def) {
        fprintf(stderr, "FAIL (EXPECTED ON UNMODIFIED BASELINE): soldier_unit/CMakeLists.txt hardcodes -DFALL_CALIBRATION_MODE=1! Fall alarms are muted in production builds!\n");
        return 1;
    }

    if (!has_cmake_option || !option_default_off) {
        fprintf(stderr, "FAIL: FALL_CALIBRATION_MODE is not configured as a CMake option defaulting to OFF!\n");
        return 1;
    }

    printf("PASS: FALL_CALIBRATION_MODE is a CMake option defaulting to OFF and not hardcoded.\n");
    return 0;
}
