#!/usr/bin/env python3
"""
Serial Console Command Test Suite
Tests all commands from docs/SerialConsole.md against running device
"""

import serial
import time
import sys
from enum import Enum
from dataclasses import dataclass
from typing import Optional, List, Tuple

class TestStatus(Enum):
    PASS = "✓ PASS"
    FAIL = "✗ FAIL"
    PARTIAL = "⚠ PARTIAL"
    TIMEOUT = "⏱ TIMEOUT"

@dataclass
class TestResult:
    name: str
    command: str
    expected_pattern: str
    actual_response: str
    status: TestStatus
    notes: str = ""

class SerialConsoleTestor:
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self.results: List[TestResult] = []

    def connect(self) -> bool:
        """Connect to serial device"""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
            time.sleep(1)  # Wait for device to settle
            # Drain any existing data
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            print(f"✓ Connected to {self.port} at {self.baudrate} baud")
            return True
        except Exception as e:
            print(f"✗ Failed to connect: {e}")
            return False

    def disconnect(self):
        """Disconnect from serial device"""
        if self.ser:
            self.ser.close()

    def send_command(self, cmd: str) -> str:
        """Send command and capture response"""
        if not self.ser:
            return ""

        try:
            # Clear buffers
            self.ser.reset_input_buffer()
            time.sleep(0.05)

            # Send command with newline
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()

            # Capture response
            response = ""
            start_time = time.time()
            while time.time() - start_time < self.timeout:
                if self.ser.in_waiting > 0:
                    response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    # If we got a response ending with newline, likely done
                    if '\n' in response or response.endswith('OK') or response.startswith('ERR'):
                        time.sleep(0.1)  # Small delay to catch any trailing data
                        if self.ser.in_waiting > 0:
                            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                        break
                time.sleep(0.01)

            return response.strip()
        except Exception as e:
            return f"ERROR: {e}"

    def test_command(self, name: str, cmd: str, expected_pattern: str,
                     notes: str = "") -> TestResult:
        """Test a single command"""
        response = self.send_command(cmd)

        # Check if response contains expected pattern
        passed = expected_pattern.lower() in response.lower()
        status = TestStatus.PASS if passed else TestStatus.FAIL

        result = TestResult(
            name=name,
            command=cmd,
            expected_pattern=expected_pattern,
            actual_response=response[:100],  # Truncate long responses
            status=status,
            notes=notes
        )
        self.results.append(result)
        return result

    def print_result(self, result: TestResult):
        """Print a single test result"""
        print(f"{result.status.value} | {result.name}")
        print(f"   CMD: {result.command}")
        print(f"   EXP: {result.expected_pattern}")
        print(f"   GOT: {result.actual_response}")
        if result.notes:
            print(f"   NOTE: {result.notes}")
        print()

    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*70)
        print("TEST SUMMARY")
        print("="*70)

        passed = sum(1 for r in self.results if r.status == TestStatus.PASS)
        failed = sum(1 for r in self.results if r.status == TestStatus.FAIL)
        partial = sum(1 for r in self.results if r.status == TestStatus.PARTIAL)
        timeout = sum(1 for r in self.results if r.status == TestStatus.TIMEOUT)
        total = len(self.results)

        print(f"Total: {total} | Pass: {passed} | Fail: {failed} | Partial: {partial} | Timeout: {timeout}")
        print(f"Success Rate: {passed}/{total} ({100*passed/total if total > 0 else 0:.1f}%)")
        print("="*70)

        # Print failed tests
        failed_tests = [r for r in self.results if r.status == TestStatus.FAIL]
        if failed_tests:
            print("\nFAILED TESTS:")
            for result in failed_tests:
                print(f"  • {result.name}")

        # Print timeout tests
        timeout_tests = [r for r in self.results if r.status == TestStatus.TIMEOUT]
        if timeout_tests:
            print("\nTIMEOUT TESTS:")
            for result in timeout_tests:
                print(f"  • {result.name}")

def main():
    PORT = "/dev/tty.usbmodem5AAF1766471"

    tester = SerialConsoleTestor(PORT)

    if not tester.connect():
        sys.exit(1)

    try:
        # Step 1: Disable logging
        print("\n" + "="*70)
        print("STEP 1: DISABLE LOGGING")
        print("="*70)
        response = tester.send_command("SYST:LOG:LEVEL OFF")
        print(f"SYST:LOG:LEVEL OFF → {response}\n")
        time.sleep(0.5)

        # Step 2: Clear any previous state
        print("="*70)
        print("STEP 2: RESET TO DEFAULTS")
        print("="*70)
        response = tester.send_command("SYST:CONF:DEFAULT")
        print(f"SYST:CONF:DEFAULT → {response}\n")
        time.sleep(0.5)

        # Step 3: Run test suite
        print("="*70)
        print("STEP 3: RUNNING TEST SUITE")
        print("="*70 + "\n")

        # RDS Tests
        print("--- RDS Commands ---")
        tester.test_command("RDS:PI set hex", "RDS:PI 0x52A1", "OK")
        tester.test_command("RDS:PI? read", "RDS:PI?", "0x52A1")
        tester.test_command("RDS:PI set decimal", "RDS:PI 21153", "OK")
        tester.test_command("RDS:PI? read decimal", "RDS:PI?", "21153")

        tester.test_command("RDS:PTY set", "RDS:PTY 10", "OK")
        tester.test_command("RDS:PTY? read", "RDS:PTY?", "10")
        tester.test_command("RDS:PTY by name", "RDS:PTY POP_MUSIC", "OK")
        tester.test_command("RDS:PTY:LIST? exists", "RDS:PTY:LIST?", "OK")
        tester.test_command("RDS:PTY:LIST has POP_MUSIC", "RDS:PTY:LIST?", "POP_MUSIC")

        tester.test_command("RDS:TP set", "RDS:TP 1", "OK")
        tester.test_command("RDS:TP? read", "RDS:TP?", "1")

        tester.test_command("RDS:TA set", "RDS:TA 1", "OK")
        tester.test_command("RDS:TA? read", "RDS:TA?", "1")

        tester.test_command("RDS:MS set", "RDS:MS 1", "OK")
        tester.test_command("RDS:MS? read", "RDS:MS?", "1")

        tester.test_command("RDS:PS set", 'RDS:PS "TestPS"', "OK")
        tester.test_command("RDS:PS? read", "RDS:PS?", "TestPS")

        tester.test_command("RDS:RT set", 'RDS:RT "Artist - Title"', "OK")
        tester.test_command("RDS:RT? read", "RDS:RT?", "Artist - Title")

        tester.test_command("RDS:ENABLE set", "RDS:ENABLE 1", "OK")
        tester.test_command("RDS:ENABLE? read", "RDS:ENABLE?", "1")

        tester.test_command("RDS:STATUS? exists", "RDS:STATUS?", "OK")
        tester.test_command("RDS:STATUS? has PI", "RDS:STATUS?", "PI=")
        tester.test_command("RDS:STATUS? has PTY", "RDS:STATUS?", "PTY=")
        tester.test_command("RDS:STATUS? has PS", "RDS:STATUS?", "PS=")

        # RadioText Rotation Tests
        print("\n--- RDS RadioText Rotation ---")
        tester.test_command("RDS:RTLIST:CLEAR", "RDS:RTLIST:CLEAR", "OK")
        tester.test_command("RDS:RTLIST:ADD first", 'RDS:RTLIST:ADD "Text A"', "OK")
        tester.test_command("RDS:RTLIST:ADD second", 'RDS:RTLIST:ADD "Text B"', "OK")
        tester.test_command("RDS:RTLIST?", "RDS:RTLIST?", "OK")
        tester.test_command("RDS:RTLIST has Text A", "RDS:RTLIST?", "Text A")
        tester.test_command("RDS:RTLIST has Text B", "RDS:RTLIST?", "Text B")
        tester.test_command("RDS:RTLIST:DEL 0", "RDS:RTLIST:DEL 0", "OK")
        tester.test_command("RDS:RTPERIOD set", "RDS:RTPERIOD 30", "OK")
        tester.test_command("RDS:RTPERIOD? read", "RDS:RTPERIOD?", "30")

        # Audio Tests
        print("\n--- Audio Commands ---")
        tester.test_command("AUDIO:STEREO set", "AUDIO:STEREO 1", "OK")
        tester.test_command("AUDIO:STEREO? read", "AUDIO:STEREO?", "1")

        tester.test_command("AUDIO:PREEMPH set", "AUDIO:PREEMPH 1", "OK")
        tester.test_command("AUDIO:PREEMPH? read", "AUDIO:PREEMPH?", "1")

        tester.test_command("AUDIO:STATUS? exists", "AUDIO:STATUS?", "OK")

        # Pilot Tests
        print("\n--- Pilot Commands ---")
        tester.test_command("PILOT:ENABLE set", "PILOT:ENABLE 1", "OK")
        tester.test_command("PILOT:ENABLE? read", "PILOT:ENABLE?", "1")

        tester.test_command("PILOT:AUTO set", "PILOT:AUTO 1", "OK")
        tester.test_command("PILOT:AUTO? read", "PILOT:AUTO?", "1")

        tester.test_command("PILOT:THRESH set", "PILOT:THRESH 0.001", "OK")
        tester.test_command("PILOT:THRESH? read", "PILOT:THRESH?", "0.001")

        tester.test_command("PILOT:HOLD set", "PILOT:HOLD 2000", "OK")
        tester.test_command("PILOT:HOLD? read", "PILOT:HOLD?", "2000")

        # System Tests
        print("\n--- System Commands ---")
        tester.test_command("SYST:VERSION?", "SYST:VERSION?", "OK")
        tester.test_command("SYST:STATUS?", "SYST:STATUS?", "OK")
        tester.test_command("SYST:HEAP?", "SYST:HEAP?", "OK")
        tester.test_command("SYST:HELP?", "SYST:HELP?", "OK")

        # Configuration Tests
        print("\n--- Configuration Commands ---")
        tester.test_command("SYST:CONF:SAVE", "SYST:CONF:SAVE test_config", "OK")
        tester.test_command("SYST:CONF:LIST?", "SYST:CONF:LIST?", "OK")
        tester.test_command("SYST:CONF:ACTIVE?", "SYST:CONF:ACTIVE?", "OK")
        tester.test_command("SYST:CONF:LOAD", "SYST:CONF:LOAD test_config", "OK")
        tester.test_command("SYST:CONF:DELETE", "SYST:CONF:DELETE test_config", "OK")

        # Print detailed results
        print("\n" + "="*70)
        print("DETAILED TEST RESULTS")
        print("="*70 + "\n")
        for result in tester.results:
            tester.print_result(result)

        # Print summary
        tester.print_summary()

    finally:
        tester.disconnect()

if __name__ == "__main__":
    main()
