#!/bin/bash
# Real direct serial test - manually verify what's actually on the device

DEVICE="/dev/tty.usbmodem5AAF1766471"

echo "========================================================================"
echo "REAL DEVICE TEST - DIRECT SERIAL INTERACTION"
echo "========================================================================"
echo ""
echo "Opening serial session with stty..."
echo ""

# Set up serial port
stty -f "$DEVICE" 115200 -echo -onlcr 2>/dev/null || true

sleep 1

# Send test commands and read responses
(
    echo ""
    echo "=== Test 1: Disable Logging ==="
    echo "SYST:LOG:LEVEL OFF"
    sleep 0.5

    echo ""
    echo "=== Test 2: Set PS ==="
    echo "RDS:PS \"TestDevice\""
    sleep 0.5

    echo ""
    echo "=== Test 3: Read PS back ==="
    echo "RDS:PS?"
    sleep 0.5

    echo ""
    echo "=== Test 4: Set PI ==="
    echo "RDS:PI 0xABCD"
    sleep 0.5

    echo ""
    echo "=== Test 5: Read PI back ==="
    echo "RDS:PI?"
    sleep 0.5

    echo ""
    echo "=== Test 6: CONF:DEFAULT ==="
    echo "SYST:CONF:DEFAULT"
    sleep 0.5

    echo ""
    echo "=== Test 7: Version ==="
    echo "SYST:VERSION?"
    sleep 0.5

) > "$DEVICE" 2>&1

sleep 2

# Read all accumulated responses
echo ""
echo "Reading device responses..."
timeout 3 cat < "$DEVICE" 2>/dev/null

echo ""
echo "========================================================================"
echo "Test complete"
echo "========================================================================"
