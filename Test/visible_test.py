#!/usr/bin/env python3
"""
Visible test - commands that should show on display
Watch the device screen to see PS and RT values change
"""

import serial
import time

PORT = "/dev/tty.usbmodem5AAF1766471"

ser = serial.Serial(PORT, 115200, timeout=2)
time.sleep(1)

print("="*70)
print("VISIBLE DEVICE TEST - Watch the display!")
print("="*70)
print()

commands = [
    ("Disable Logging", "SYST:LOG:LEVEL OFF"),
    ("Reset to Defaults", "SYST:CONF:DEFAULT"),
    ("", ""),  # Empty - just a pause
    ("Set PS to 'Test001'", 'RDS:PS "Test001"'),
    ("", ""),  # Pause to see display change
    ("Set PS to 'MyRadio'", 'RDS:PS "MyRadio"'),
    ("", ""),  # Pause
    ("Set RT to 'Song Title'", 'RDS:RT "Song Title"'),
    ("", ""),  # Pause
    ("Set PS to 'FM87.5'", 'RDS:PS "FM87.5"'),
    ("", ""),  # Pause
    ("Read PS back", "RDS:PS?"),
    ("Read RT back", "RDS:RT?"),
    ("Test CONF:DEFAULT", "SYST:CONF:DEFAULT"),
    ("Test CONF:SAVE", "SYST:CONF:SAVE test"),
    ("Test CONF:LIST?", "SYST:CONF:LIST?"),
]

for label, cmd in commands:
    if cmd:
        if label:
            print(f"\n>>> {label}")
        print(f"    Sending: {cmd}")

        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
        ser.flush()
        time.sleep(0.4)

        response = ser.read(300).decode('utf-8', errors='ignore')
        print(f"    Response: {response[:100]}")
        print()
    else:
        # Empty command - just a pause
        print("    [Waiting 2 seconds for display to update...]")
        time.sleep(2)

print("\n" + "="*70)
print("Test complete - did you see the PS and RT values change on display?")
print("="*70)

ser.close()
