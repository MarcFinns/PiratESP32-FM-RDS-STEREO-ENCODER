#!/usr/bin/env python3
"""
Direct serial test - no pyserial dependency
Uses subprocess to communicate with device
"""

import subprocess
import time
import sys

def send_command(cmd, timeout=2):
    """Send command and capture response"""
    try:
        # Use printf to send command, timeout to read response
        send_process = subprocess.Popen(
            f'printf "{cmd}\\r" > /dev/tty.usbmodem5AAF1766471',
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        send_process.wait(timeout=1)

        # Read response
        time.sleep(0.3)
        read_process = subprocess.Popen(
            'timeout 0.5 cat < /dev/tty.usbmodem5AAF1766471',
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        stdout, stderr = read_process.communicate(timeout=2)
        response = (stdout + stderr).strip()[:500]  # Limit to 500 chars
        return response
    except Exception as e:
        return f"ERROR: {e}"

def test_cmd(name, cmd, expected):
    """Test a single command"""
    print(f"\n{name}")
    print(f"  Send: {cmd}")
    response = send_command(cmd)
    print(f"  Recv: {response[:100]}")

    if expected.lower() in response.lower():
        print(f"  ✓ PASS")
        return True
    else:
        print(f"  ✗ FAIL - Expected '{expected}' not found")
        return False

print("="*70)
print("DIRECT SERIAL TEST - CONF COMMANDS")
print("="*70)

# Give device time to be ready
time.sleep(1)

# Disable logging
print("\nDisabling logging...")
send_command("SYST:LOG:LEVEL OFF")
time.sleep(0.5)

# Test CONF commands
print("\n" + "="*70)
print("Testing SYST:CONF:* Commands")
print("="*70)

results = []

# Test 1: CONF:DEFAULT
results.append(test_cmd("Test 1: SYST:CONF:DEFAULT", "SYST:CONF:DEFAULT", "OK"))
time.sleep(1)

# Test 2: CONF:SAVE
results.append(test_cmd("Test 2: SYST:CONF:SAVE test", "SYST:CONF:SAVE test", "OK"))
time.sleep(1)

# Test 3: CONF:LIST?
results.append(test_cmd("Test 3: SYST:CONF:LIST?", "SYST:CONF:LIST?", "OK"))
time.sleep(1)

# Test 4: CONF:ACTIVE?
results.append(test_cmd("Test 4: SYST:CONF:ACTIVE?", "SYST:CONF:ACTIVE?", "OK"))
time.sleep(1)

# Test 5: CONF:DELETE
results.append(test_cmd("Test 5: SYST:CONF:DELETE test", "SYST:CONF:DELETE test", "OK"))
time.sleep(1)

# Test STATUS queries
print("\n" + "="*70)
print("Testing STATUS Queries")
print("="*70)

results.append(test_cmd("Test 6: RDS:STATUS?", "RDS:STATUS?", "OK"))
time.sleep(1)

results.append(test_cmd("Test 7: AUDIO:STATUS?", "AUDIO:STATUS?", "OK"))
time.sleep(1)

results.append(test_cmd("Test 8: SYST:STATUS?", "SYST:STATUS?", "OK"))
time.sleep(1)

results.append(test_cmd("Test 9: SYST:VERSION?", "SYST:VERSION?", "OK"))
time.sleep(1)

# Summary
print("\n" + "="*70)
print("TEST SUMMARY")
print("="*70)
passed = sum(1 for r in results if r)
total = len(results)
print(f"Passed: {passed}/{total}")
print(f"Success: {100*passed//total}%")

if passed == total:
    print("\n✓ ALL TESTS PASSED!")
    sys.exit(0)
else:
    print(f"\n✗ {total-passed} test(s) failed")
    sys.exit(1)
