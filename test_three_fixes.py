#!/usr/bin/env python3
"""
Test the three specific fixes without hitting the parser failure
"""

import serial
import time

PORT = "/dev/tty.usbmodem5AAF1766471"

ser = serial.Serial(PORT, 115200, timeout=2)
time.sleep(1)

print("="*70)
print("TEST: The 3 Critical Fixes")
print("="*70)

results = []

# Test Issue #1: SYST:CONF:DEFAULT
print("\n=== ISSUE #1: SYST:CONF:* Commands ===")
print("Test: SYST:CONF:DEFAULT")
ser.write(b"SYST:CONF:DEFAULT\n")
ser.flush()
time.sleep(0.3)
resp = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp}")
passed = "OK" in resp
print(f"Status: {'✓ PASS' if passed else '✗ FAIL'}")
results.append(("SYST:CONF:DEFAULT", passed))

time.sleep(0.5)

# Test Issue #2a: RDS:PS? format and persistence
print("\n=== ISSUE #2A: RDS:PS? Format and Persistence ===")
print("Set: RDS:PS \"TestPS\"")
ser.write(b'RDS:PS "TestPS"\n')
ser.flush()
time.sleep(0.3)
resp_set = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp_set}")

time.sleep(0.3)

print("Get: RDS:PS?")
ser.write(b"RDS:PS?\n")
ser.flush()
time.sleep(0.3)
resp_get = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp_get}")

# Check format is correct (not the broken JSON-like format)
correct_format = 'PS="TestPS"' in resp_get or 'PS="testps"' in resp_get.lower()
value_persists = "TestPS" in resp_get or "testps" in resp_get.lower()
passed = correct_format and value_persists
print(f"Format correct: {correct_format}")
print(f"Value persists: {value_persists}")
print(f"Status: {'✓ PASS' if passed else '✗ FAIL'}")
results.append(("RDS:PS Persistence & Format", passed))

time.sleep(0.5)

# Test Issue #2b: RDS:RT? format and persistence
print("\n=== ISSUE #2B: RDS:RT? Format and Persistence ===")
print("Set: RDS:RT \"MyTitle\"")
ser.write(b'RDS:RT "MyTitle"\n')
ser.flush()
time.sleep(0.3)
resp_set = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp_set}")

time.sleep(0.3)

print("Get: RDS:RT?")
ser.write(b"RDS:RT?\n")
ser.flush()
time.sleep(0.3)
resp_get = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp_get}")

# Check format is correct
correct_format = 'RT="MyTitle"' in resp_get or 'RT="mytitle"' in resp_get.lower()
value_persists = "MyTitle" in resp_get or "mytitle" in resp_get.lower()
passed = correct_format and value_persists
print(f"Format correct: {correct_format}")
print(f"Value persists: {value_persists}")
print(f"Status: {'✓ PASS' if passed else '✗ FAIL'}")
results.append(("RDS:RT Persistence & Format", passed))

time.sleep(0.5)

# Test Issue #1 more: CONF:SAVE and LIST
print("\n=== ISSUE #1 (continued): SYST:CONF:SAVE and LIST ===")
print("Test: SYST:CONF:SAVE mytest")
ser.write(b"SYST:CONF:SAVE mytest\n")
ser.flush()
time.sleep(0.3)
resp = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp}")
passed = "OK" in resp
print(f"Status: {'✓ PASS' if passed else '✗ FAIL'}")
results.append(("SYST:CONF:SAVE", passed))

time.sleep(0.5)

print("Test: SYST:CONF:LIST?")
ser.write(b"SYST:CONF:LIST?\n")
ser.flush()
time.sleep(0.3)
resp = ser.read(200).decode('utf-8', errors='ignore')
print(f"Response: {resp}")
passed = "OK" in resp
print(f"Status: {'✓ PASS' if passed else '✗ FAIL'}")
results.append(("SYST:CONF:LIST?", passed))

# Summary
print("\n" + "="*70)
print("SUMMARY: Three Critical Fixes")
print("="*70)

total = len(results)
passed_count = sum(1 for _, p in results if p)

for name, passed in results:
    status = "✓ PASS" if passed else "✗ FAIL"
    print(f"{status} | {name}")

print(f"\nTotal: {passed_count}/{total} passed")
if passed_count == total:
    print("\n✓ ALL THREE FIXES WORKING!")
else:
    print(f"\n✗ {total - passed_count} test(s) failed")

ser.close()
