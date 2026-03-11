# Console Logger Testing Guide

## Overview
This document describes how to test the console log history feature for battery diagnostics.

## Feature Description
The console logger captures all console output in memory from power-up to deep sleep. It automatically saves the log to NVS if any errors occurred during the session (only when running on battery power). The system maintains a circular buffer of the last 5 log sessions.

Users can review stored logs by:
1. Connecting the device to USB power
2. Pressing the button 4 times (quadruple click)
3. The device will announce each stored log session via TTS

## Test Setup Requirements
- Unexpected Maker TinyS3D with firmware flashed
- USB cable for power and serial monitoring
- 3.7V LiPo battery
- Serial terminal (e.g., minicom, screen, or ESP-IDF monitor)

## Test Cases

### Test 1: Log Capture During Normal Operation
**Objective**: Verify that console logs are captured in memory

**Steps**:
1. Flash the firmware
2. Connect serial terminal
3. Power on the device (on battery or USB)
4. Press button to trigger a measurement
5. Observe serial output

**Expected Results**:
- Console logger initialization message appears: "Console logger initialized (buffer size: 4096 bytes)"
- All subsequent logs are visible in serial terminal
- No performance degradation

### Test 2: Error Detection and NVS Storage
**Objective**: Verify that errors are detected and logs are saved to NVS only when errors occur

**Steps**:
1. Ensure device is on battery power (not USB)
2. Power on device
3. Trigger an operation that will cause an error (e.g., disconnect I2C sensor and try measurement)
4. Wait for device to enter deep sleep (30 seconds)
5. Observe serial output before sleep

**Expected Results**:
- Before deep sleep, console should show:
  - "Step 1.5: Checking for errors to save console log"
  - "Console log saved to NVS (errors detected)"
- Error should be visible in serial output with "E (" prefix

**Steps (No Error Case)**:
1. Ensure device is on battery power
2. Power on device
3. Perform normal measurement without errors
4. Wait for device to enter deep sleep

**Expected Results**:
- Before deep sleep, console should show:
  - "Step 1.5: Checking for errors to save console log"
  - "No errors in this session, log not saved"

### Test 3: Log Storage on USB Power
**Objective**: Verify that logs are NOT saved when on USB power

**Steps**:
1. Connect device to USB power
2. Trigger an operation that causes an error
3. Wait for device to enter deep sleep

**Expected Results**:
- Before deep sleep, console should show:
  - "Step 1.5: Skipped log save (on USB power)"
- No log should be written to NVS even if errors occurred

### Test 4: Log Retrieval with Quad Button Press
**Objective**: Verify that stored logs can be retrieved via quad button press on USB power

**Steps**:
1. Ensure at least one log session with errors is stored (from Test 2)
2. Connect device to USB power
3. Wait for device to wake and announce "Ready"
4. Quickly press button 4 times (quad click)
5. Listen to TTS announcements

**Expected Results**:
- Device announces: "Found N stored log sessions" (where N is 1-5)
- For each log:
  - Device announces: "Log X of N"
  - Device extracts and announces errors: "Error: [error message]"
  - If no errors in log: "No errors found in this log"
- After all logs: "End of stored logs"
- Serial console shows full log content with markers:
  - "=== Stored Log X (most recent = 0) ==="
  - [log content]
  - "=== End of Log X ==="

### Test 5: Quad Button Press on Battery Power
**Objective**: Verify that quad button press is ignored when not on USB power

**Steps**:
1. Ensure device is on battery power (USB disconnected)
2. Power on device
3. Quickly press button 4 times (quad click)

**Expected Results**:
- Device announces: "Not on USB power"
- No logs are retrieved or announced

### Test 6: Circular Buffer Behavior
**Objective**: Verify that only the last 5 log sessions are kept

**Steps**:
1. Run Test 2 (error scenario) 7 times to create 7 error log sessions
2. Connect to USB power
3. Quad-click button to retrieve logs

**Expected Results**:
- Device announces: "Found 5 stored log sessions"
- Only the 5 most recent sessions are available
- Oldest sessions have been overwritten

### Test 7: Memory Usage and Performance
**Objective**: Verify no significant performance impact

**Steps**:
1. Monitor heap memory before and after console_logger_init()
2. Perform measurements with logger active
3. Measure time for typical operations

**Expected Results**:
- Memory usage increases by ~4KB (LOG_BUFFER_SIZE)
- No noticeable delay in measurements or TTS responses
- Normal operations complete in same timeframe

### Test 8: Buffer Overflow Protection
**Objective**: Verify that logger handles buffer overflow gracefully

**Steps**:
1. Generate extensive logging (e.g., multiple measurements, calibrations)
2. Fill the 4KB buffer
3. Continue operations
4. Check that device remains stable

**Expected Results**:
- Buffer stops at 4KB limit
- No crashes or hangs
- Newer logs may overwrite earlier logs, but last portion is preserved

## Success Criteria
All test cases pass with expected results.

## Known Limitations
1. Maximum log buffer size: 4KB per session
2. Maximum stored sessions: 5
3. Error detection is based on "E (" pattern matching in console output
4. Logs are only saved if errors occurred during battery operation
5. TTS announcements may be long if there are many errors
6. NVS partition is 24KB - buffer sized to fit alongside calibration data

## Debugging Tips
- Use `idf.py monitor` to see all serial output
- Check NVS namespace "console_log" with NVS partition tool
- Increase LOG_BUFFER_SIZE if logs are getting truncated
- Adjust MAX_STORED_LOGS if more history is needed
