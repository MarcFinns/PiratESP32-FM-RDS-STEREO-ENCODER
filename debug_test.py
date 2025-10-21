#!/usr/bin/env python3

import serial
import time

PORT = "/dev/tty.usbmodem5AAF1766471"

ser = serial.Serial(PORT, 115200, timeout=2)
time.sleep(1)

commands = [
    "RDS:PI?",
    "RDS:STATUS?",
    "SYST:VERSION?",
    "SYST:LOG:LEVEL OFF",
    "RDS:PTY INVALID_NAME",  # This should definitely error
]

for cmd in commands:
    print(f"\nSending: {cmd}")
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(0.3)

    response = ser.read(200).decode('utf-8', errors='ignore')
    print(f"Response: {response}")

ser.close()
