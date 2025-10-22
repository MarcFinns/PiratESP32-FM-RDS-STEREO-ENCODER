#!/bin/bash
# Serial Console Test Suite - using native tools

DEVICE="/dev/tty.usbmodem5AAF1766471"
BAUDRATE="115200"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
PASS_COUNT=0
FAIL_COUNT=0
TEST_COUNT=0

# Function to send command and capture response
send_cmd() {
    local cmd="$1"
    local timeout="$2"

    if [ -z "$timeout" ]; then
        timeout=1
    fi

    # Send command and capture response
    (echo "$cmd"; sleep 0.2) | timeout "$timeout" cat > "$DEVICE" < "$DEVICE" 2>/dev/null
}

# Function to run a test
test_cmd() {
    local name="$1"
    local cmd="$2"
    local expected="$3"
    local test_file="/tmp/test_response_$$.txt"

    TEST_COUNT=$((TEST_COUNT + 1))

    # Send command and capture response with timeout
    (echo "$cmd"; sleep 0.3) | timeout 1 cat < "$DEVICE" > "$test_file" 2>/dev/null &

    # Write to device in background
    echo "$cmd" > "$DEVICE" 2>/dev/null &
    sleep 0.5

    # Read response
    response=$(cat < "$DEVICE" 2>/dev/null | head -c 200)

    if echo "$response" | grep -qi "$expected"; then
        echo -e "${GREEN}✓ PASS${NC} | $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "${RED}✗ FAIL${NC} | $name"
        echo "    Cmd: $cmd"
        echo "    Expected: $expected"
        echo "    Got: $response"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    rm -f "$test_file"
}

echo "========================================================================"
echo "Serial Console Test Suite"
echo "Device: $DEVICE"
echo "========================================================================"
echo ""

# Step 1: Disable logging
echo "STEP 1: Disabling logging..."
echo "SYST:LOG:LEVEL OFF" > "$DEVICE"
sleep 0.5
echo ""

# Step 2: Reset to defaults
echo "STEP 2: Resetting to defaults..."
echo "SYST:CONF:DEFAULT" > "$DEVICE"
sleep 0.5
echo ""

# Step 3: Run tests
echo "========================================================================"
echo "STEP 3: Running Test Suite"
echo "========================================================================"
echo ""

echo "--- RDS Commands ---"
test_cmd "RDS:PI set hex" "RDS:PI 0x52A1" "OK"
test_cmd "RDS:PI read" "RDS:PI?" "52A1"
test_cmd "RDS:PTY set" "RDS:PTY 10" "OK"
test_cmd "RDS:PTY read" "RDS:PTY?" "10"
test_cmd "RDS:PTY:LIST" "RDS:PTY:LIST?" "POP_MUSIC"
test_cmd "RDS:TP set" "RDS:TP 1" "OK"
test_cmd "RDS:TA set" "RDS:TA 1" "OK"
test_cmd "RDS:MS set" "RDS:MS 1" "OK"
test_cmd "RDS:PS set" 'RDS:PS "TestPS"' "OK"
test_cmd "RDS:PS read" "RDS:PS?" "TestPS"
test_cmd "RDS:RT set" 'RDS:RT "Artist"' "OK"
test_cmd "RDS:ENABLE set" "RDS:ENABLE 1" "OK"
test_cmd "RDS:STATUS" "RDS:STATUS?" "OK"

echo ""
echo "--- Audio Commands ---"
test_cmd "AUDIO:STEREO set" "AUDIO:STEREO 1" "OK"
test_cmd "AUDIO:PREEMPH set" "AUDIO:PREEMPH 1" "OK"
test_cmd "AUDIO:STATUS" "AUDIO:STATUS?" "OK"

echo ""
echo "--- Pilot Commands ---"
test_cmd "PILOT:ENABLE set" "PILOT:ENABLE 1" "OK"
test_cmd "PILOT:AUTO set" "PILOT:AUTO 1" "OK"
test_cmd "PILOT:THRESH set" "PILOT:THRESH 0.001" "OK"
test_cmd "PILOT:HOLD set" "PILOT:HOLD 2000" "OK"

echo ""
echo "--- System Commands ---"
test_cmd "SYST:VERSION" "SYST:VERSION?" "OK"
test_cmd "SYST:STATUS" "SYST:STATUS?" "OK"
test_cmd "SYST:HEAP" "SYST:HEAP?" "OK"

echo ""
echo "========================================================================"
echo "TEST SUMMARY"
echo "========================================================================"
echo "Total Tests: $TEST_COUNT"
echo -e "Passed: ${GREEN}$PASS_COUNT${NC}"
echo -e "Failed: ${RED}$FAIL_COUNT${NC}"

if [ $TEST_COUNT -gt 0 ]; then
    success_rate=$((100 * PASS_COUNT / TEST_COUNT))
    echo "Success Rate: $success_rate%"
fi

echo "========================================================================"
