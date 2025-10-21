#!/usr/bin/env python3
"""
Capture raw serial output to see what's happening
"""

import serial
import time

PORT = "/dev/tty.usbmodem5AAF1766471"

ser = serial.Serial(PORT, 115200, timeout=2)
time.sleep(1)

print("Sending: SYST:LOG:LEVEL OFF")
ser.write(b"SYST:LOG:LEVEL OFF\n")
ser.flush()
time.sleep(0.5)

print("\nReading raw response (hex dump):")
data = ser.read(200)
print(f"Raw bytes: {data}")
print(f"Hex: {data.hex()}")
print(f"Decoded: {data.decode('utf-8', errors='ignore')}")

ser.close()
