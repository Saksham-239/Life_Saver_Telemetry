param (
    [switch]$Sanitizers,
    [switch]$BuildIdf,
    [switch]$BuildPico,
    [switch]$Full
)

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Full) {
    $commit = (git rev-parse --short HEAD).Trim()
    $logDir = Join-Path $ScriptDir "docs\validation_logs"
    if (!(Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
    $logFile = Join-Path $logDir "$commit.txt"

    try {
        Start-Transcript -Path $logFile -Force
    } catch {
        Write-Warning "Could not start transcript: $_"
    }

    try {
        Write-Host "=========================================================="
        Write-Host "  RUNNING FULL VALIDATION SUITE (Host + WSL Sanitizers + IDF + Pico)
  Authoritative Tree : $ScriptDir
  Current Working Dir: $(Get-Location)
  Validation Log     : $logFile"
        Write-Host "=========================================================="
        
        # Synchronize clean ASCII path mirror if running from OneDrive
        if ((Test-Path "C:\dev\telemetry_link") -and ($ScriptDir -ne "C:\dev\telemetry_link")) {
            Write-Host "`n>>> [SYNC] Synchronizing authoritative tree to C:\dev\telemetry_link..."
            robocopy $ScriptDir "C:\dev\telemetry_link" /MIR /XD .git build build_san .venv managed_components /XF *.exe *.obj *.o *.a *.bin *.elf *.hex *.uf2 *.log | Out-Null
        }

        # 1. Host test suite
        Write-Host "`n>>> [STEP 1/4] Host Validation Suite..."
        powershell -ExecutionPolicy Bypass -File (Join-Path $ScriptDir "validate.ps1")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        # 2. WSL Sanitizers
        Write-Host "`n>>> [STEP 2/4] WSL Sanitizers (ASan / UBSan / TSan)..."
        powershell -ExecutionPolicy Bypass -File (Join-Path $ScriptDir "validate.ps1") -Sanitizers
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        # 3. ESP-IDF Build (Clean Rebuild)
        Write-Host "`n>>> [STEP 3/4] ESP32-S3 Target Clean Rebuild (idf.py clean && idf.py build)..."
        $idfBase = if (Test-Path "C:\dev\telemetry_link\base_station") { "C:\dev\telemetry_link\base_station" } else { Join-Path $ScriptDir "base_station" }
        powershell -ExecutionPolicy Bypass -Command ". C:\esp\esp-idf\export.ps1; cd $idfBase; idf.py clean; idf.py build"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        # 4. RP2040 Build (Clean Rebuild)
        Write-Host "`n>>> [STEP 4/4] RP2040 Target Clean Rebuild (ninja clean && ninja)..."
        $picoBase = if (Test-Path "C:\dev\telemetry_link\soldier_unit\build") { "C:\dev\telemetry_link\soldier_unit\build" } else { Join-Path $ScriptDir "soldier_unit\build" }
        ninja -C $picoBase clean
        ninja -C $picoBase
        arm-none-eabi-size (Join-Path $picoBase "soldier_unit.elf")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "`n=========================================================="
        Write-Host "FULL VALIDATION COMPLETED: ALL FOUR TIERS PASSED" -ForegroundColor Green
        Write-Host "=========================================================="
    }
    finally {
        try {
            Stop-Transcript
        } catch { }
    }
    exit 0
}

if ($Sanitizers) {
    Write-Host "=========================================================="
    Write-Host "  Invoking WSL Sanitizers Suite (ASan / UBSan / TSan)"
    Write-Host "=========================================================="
    if ((Test-Path "C:\dev\telemetry_link") -and ($ScriptDir -ne "C:\dev\telemetry_link")) {
        robocopy $ScriptDir "C:\dev\telemetry_link" /MIR /XD .git build build_san .venv managed_components /XF *.exe *.obj *.o *.a *.bin *.elf *.hex *.uf2 *.log | Out-Null
    }
    $targetPath = if (Test-Path "C:\dev\telemetry_link") { "/mnt/c/dev/telemetry_link" } else { $ScriptDir.Replace("\", "/").Replace("C:", "/mnt/c") }
    wsl bash -c "cd $targetPath && ./validate.sh --sanitizers"
    exit $LASTEXITCODE
}

if ($BuildIdf) {
    Write-Host "=========================================================="
    Write-Host "  Building ESP32-S3 Target (idf.py build)"
    Write-Host "=========================================================="
    if ((Test-Path "C:\dev\telemetry_link") -and ($ScriptDir -ne "C:\dev\telemetry_link")) {
        robocopy $ScriptDir "C:\dev\telemetry_link" /MIR /XD .git build build_san .venv managed_components /XF *.exe *.obj *.o *.a *.bin *.elf *.hex *.uf2 *.log | Out-Null
    }
    $idfBase = if (Test-Path "C:\dev\telemetry_link\base_station") { "C:\dev\telemetry_link\base_station" } else { Join-Path $ScriptDir "base_station" }
    powershell -ExecutionPolicy Bypass -Command ". C:\esp\esp-idf\export.ps1; cd $idfBase; idf.py build"
    exit $LASTEXITCODE
}

if ($BuildPico) {
    Write-Host "=========================================================="
    Write-Host "  Building RP2040 Soldier Unit Target (ninja)"
    Write-Host "=========================================================="
    if ((Test-Path "C:\dev\telemetry_link") -and ($ScriptDir -ne "C:\dev\telemetry_link")) {
        robocopy $ScriptDir "C:\dev\telemetry_link" /MIR /XD .git build build_san .venv managed_components /XF *.exe *.obj *.o *.a *.bin *.elf *.hex *.uf2 *.log | Out-Null
    }
    $picoBase = if (Test-Path "C:\dev\telemetry_link\soldier_unit\build") { "C:\dev\telemetry_link\soldier_unit\build" } else { Join-Path $ScriptDir "soldier_unit\build" }
    ninja -C $picoBase
    arm-none-eabi-size (Join-Path $picoBase "soldier_unit.elf")
    exit $LASTEXITCODE
}

Write-Host "=========================================================="
Write-Host "  RP2040 - ESP32-S3 Telemetry Link Validation Suite
  Authoritative Tree : $ScriptDir
  Current Working Dir: $(Get-Location)"
Write-Host "=========================================================="

Write-Host "[1/3] Verifying documentation consistency against source..."
py -3.12 (Join-Path $ScriptDir "tools\check_docs.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BuildDir = Join-Path $ScriptDir "tests\host\build"

Write-Host "[2/3] Building host validation suite..."
cmake -B $BuildDir -S (Join-Path $ScriptDir "tests\host") -G Ninja
ninja -C $BuildDir

Write-Host "[3/3] Running host test targets..."
$Failures = 0

$tests = @(
    "test_packet_codec",
    "test_soldier_buttons",
    "test_mpu_fall_detect",
    "test_base_sm_logic",
    "test_base_sm_race",
    "test_def01_cal_mode",
    "test_button_fsm",
    "test_config_store",
    "test_buzzer_bounded",
    "test_nvs_hwm",
    "test_oled_page5",
    "test_rf_pipes",
    "test_radio_timing",
    "test_nrf24_model",
    "sim_dlpf",
    "sim_esb_link",
    "test_base_sm_property",
    "test_aes_ccm_tamper"
)

foreach ($t in $tests) {
    Write-Host ""
    Write-Host "----------------------------------------------------------"
    Write-Host "Running: $t"
    Write-Host "----------------------------------------------------------"
    $bin = Join-Path $BuildDir "$t.exe"
    & $bin
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $t (exit code $LASTEXITCODE)" -ForegroundColor Red
        $Failures++
    } else {
        Write-Host "[PASS] $t" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "=========================================================="
if ($Failures -eq 0) {
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "TEST FAILURES DETECTED: $Failures test(s) failed." -ForegroundColor Yellow
    exit $Failures
}

