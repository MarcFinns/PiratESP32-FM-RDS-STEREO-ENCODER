#!/bin/bash
# Simple serial test script using stty

DEVICE="/dev/tty.usbmodem5AAF1766471"
TIMEOUT=2

# Function to send command and read response
send_and_read() {
    local cmd="$1"
    local label="$2"

    # Send command
    echo "$cmd" > "$DEVICE"
    sleep 0.3

    # Read response with timeout
    local response=$(timeout $TIMEOUT cat < "$DEVICE" 2>/dev/null | head -c 200)

    echo "→ $label"
    echo "  Sent: $cmd"
    echo "  Got:  $response"
    echo ""
}

echo "========================================================================"
echo "Testing Console.cpp Fixes"
echo "========================================================================"
echo ""

# Disable logging
echo "=== Step 1: Disable Logging ==="
send_and_read "SYST:LOG:LEVEL OFF" "Disable logging"

sleep 1

# Test 1: SYST:CONF:DEFAULT (was broken)
echo "=== Test 1: SYST:CONF:DEFAULT (ISSUE #1 FIX) ==="
send_and_read "SYST:CONF:DEFAULT" "Reset to defaults"

# Test 2: RDS:PS? (was timing out due to wrong format)
echo "=== Test 2: RDS:PS? and RDS:RT? Response Format (ISSUE #2 FIX) ==="
send_and_read "RDS:PS \"TestPS\"" "Set PS"
send_and_read "RDS:PS?" "Read PS (should be OK PS=\"TestPS\")"

send_and_read "RDS:RT \"Test Title\"" "Set RT"
send_and_read "RDS:RT?" "Read RT (should be OK RT=\"Test Title\")"

# Test 3: RDS:PTY:LIST? (should now work)
echo "=== Test 3: RDS:PTY:LIST? Response (ISSUE #3 CHECK) ==="
send_and_read "RDS:PTY:LIST?" "Get PTY list (should contain POP_MUSIC)"

# Test 4: Other CONF commands
echo "=== Test 4: Other SYST:CONF:* Commands ==="
send_and_read "SYST:CONF:SAVE myconfig" "Save config"
send_and_read "SYST:CONF:LIST?" "List configs"
send_and_read "SYST:CONF:ACTIVE?" "Get active config"

echo "========================================================================"
echo "Test Complete - Check responses above"
echo "========================================================================"
