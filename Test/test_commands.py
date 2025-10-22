#!/usr/bin/env python3
"""
Comprehensive Serial Command Processor Test Suite
Tests all SCPI-style commands from SerialConsole.md specification
"""

import serial
import time
import sys
import json
from typing import Tuple, Optional, Dict, Any
from dataclasses import dataclass

@dataclass
class TestResult:
    """Result of a single test"""
    name: str
    passed: bool
    expected: str
    received: str
    error: Optional[str] = None

class CommandTester:
    def __init__(self, port: str = "/dev/tty.usbmodem5AAF1766471", timeout: float = 2.0):
        """Initialize serial connection"""
        self.port = port
        self.timeout = timeout
        self.ser = None
        self.results = []
        self.command_count = 0

    def connect(self) -> bool:
        """Connect to serial device"""
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=self.timeout)
            time.sleep(0.5)  # Allow device to settle
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            return True
        except Exception as e:
            print(f"ERROR: Failed to connect to {self.port}: {e}")
            return False

    def disconnect(self):
        """Close serial connection"""
        if self.ser:
            self.ser.close()

    def send_command(self, cmd: str, wait_time: float = 0.2) -> str:
        """Send a command and receive response"""
        self.command_count += 1
        try:
            # Send command
            self.ser.write((cmd + "\n").encode())
            time.sleep(wait_time)

            # Read response
            response = ""
            while self.ser.in_waiting > 0:
                chunk = self.ser.read(256).decode('utf-8', errors='replace')
                response += chunk
                time.sleep(0.05)

            return response.strip()
        except Exception as e:
            return f"SERIAL_ERROR: {e}"

    def test_set_get(self, set_cmd: str, get_cmd: str, expected_pattern: str = None) -> bool:
        """Test a set/get pair - set value then verify get returns it"""
        test_name = f"SET/GET: {set_cmd} / {get_cmd}"

        # Send set command
        set_resp = self.send_command(set_cmd)
        if not (set_resp.startswith("OK") or set_resp.startswith("{\"ok\"")):
            self.results.append(TestResult(test_name, False, "OK", set_resp, f"SET failed: {set_resp}"))
            return False

        time.sleep(0.1)

        # Send get command
        get_resp = self.send_command(get_cmd)
        if not (get_resp.startswith("OK") or get_resp.startswith("{\"ok\"")):
            self.results.append(TestResult(test_name, False, "OK <value>", get_resp, f"GET failed: {get_resp}"))
            return False

        # Check if get contains expected pattern if provided
        if expected_pattern and expected_pattern not in get_resp:
            self.results.append(TestResult(test_name, False, f"OK {expected_pattern}...", get_resp,
                                          f"Pattern '{expected_pattern}' not found"))
            return False

        self.results.append(TestResult(test_name, True, set_resp, get_resp))
        return True

    def test_get_only(self, cmd: str) -> bool:
        """Test a get-only query command"""
        test_name = f"GET: {cmd}"
        resp = self.send_command(cmd)

        if resp.startswith("OK"):
            self.results.append(TestResult(test_name, True, "OK ...", resp))
            return True
        else:
            self.results.append(TestResult(test_name, False, "OK ...", resp))
            return False

    def test_error_case(self, cmd: str, expect_error: bool = True) -> bool:
        """Test that an invalid command returns an error"""
        test_name = f"ERROR: {cmd}"
        resp = self.send_command(cmd)

        if expect_error:
            if resp.startswith("ERR"):
                self.results.append(TestResult(test_name, True, "ERR", resp))
                return True
            else:
                self.results.append(TestResult(test_name, False, "ERR", resp))
                return False

    def run_all_tests(self):
        """Run comprehensive test suite"""
        print("\n" + "="*80)
        print("PiratESP32 COMMAND PROCESSOR TEST SUITE")
        print("="*80 + "\n")

        # Step 1: Reboot device and wait for initialization
        print("[STARTUP] Sending SYST:REBOOT command...")
        self.send_command("SYST:REBOOT")
        print("[STARTUP] Waiting 5 seconds for device to reboot...")
        time.sleep(5)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

        # Step 2: Disable logging to reduce noise
        print("[STARTUP] Disabling logs (SYST:LOG:LEVEL OFF)...")
        resp = self.send_command("SYST:LOG:LEVEL OFF")
        if resp.startswith("OK"):
            print("[STARTUP] ✓ Logging disabled")
        else:
            print(f"[STARTUP] Warning: Could not disable logging: {resp}")

        time.sleep(0.5)

        # Test 1: Basic RDS PI command (set/get)
        print("TEST 1: RDS:PI (Integer with hex support)")
        self.test_set_get("RDS:PI 0x52A1", "RDS:PI?", "52A1")
        self.test_set_get("RDS:PI 20000", "RDS:PI?", "4E20")

        # Test 2: RDS PTY command (set/get)
        print("TEST 2: RDS:PTY (Enumeration)")
        self.test_set_get("RDS:PTY 10", "RDS:PTY?", "10")
        self.test_set_get("RDS:PTY 15", "RDS:PTY?", "15")

        # Test 3: RDS flags (TP, TA, MS)
        print("TEST 3: RDS:TP, RDS:TA, RDS:MS (Boolean flags)")
        self.test_set_get("RDS:TP 1", "RDS:TP?", "TP=1")
        self.test_set_get("RDS:TA 0", "RDS:TA?", "TA=0")
        self.test_set_get("RDS:MS 1", "RDS:MS?", "MS=1")

        # Test 4: RDS PS (8-char station name)
        print("TEST 4: RDS:PS (8-character string)")
        self.test_set_get('RDS:PS "TestSt"', 'RDS:PS?', "TestSt")
        self.test_set_get('RDS:PS "ABC12345"', 'RDS:PS?', "ABC12345")

        # Test 5: RDS RT (RadioText up to 64 chars)
        print("TEST 5: RDS:RT (64-character RadioText)")
        self.test_set_get('RDS:RT "Now playing: Artist - Song"', 'RDS:RT?', "Now playing")
        self.test_set_get('RDS:RT "Test message"', 'RDS:RT?', "Test message")

        # Test 6: RDS Enable
        print("TEST 6: RDS:ENABLE (RDS subcarrier enable/disable)")
        self.test_set_get("RDS:ENABLE 0", "RDS:ENABLE?", "ENABLE=0")
        self.test_set_get("RDS:ENABLE 1", "RDS:ENABLE?", "ENABLE=1")

        # Test 7: RDS Status
        print("TEST 7: RDS:STATUS? (Aggregate RDS status)")
        self.test_get_only("RDS:STATUS?")

        # Test 8: Audio Stereo
        print("TEST 8: AUDIO:STEREO (Stereo subcarrier enable)")
        self.test_set_get("AUDIO:STEREO 1", "AUDIO:STEREO?", "STEREO=1")
        self.test_set_get("AUDIO:STEREO 0", "AUDIO:STEREO?", "STEREO=0")

        # Test 9: Pilot control
        print("TEST 9: PILOT:ENABLE, PILOT:AUTO (Pilot tone control)")
        self.test_set_get("PILOT:ENABLE 1", "PILOT:ENABLE?", "ENABLE=1")
        self.test_set_get("PILOT:AUTO 0", "PILOT:AUTO?", "AUTO=0")

        # Test 10: Pilot threshold (float)
        print("TEST 10: PILOT:THRESH (Float threshold)")
        self.test_set_get("PILOT:THRESH 0.001", "PILOT:THRESH?", "THRESH")

        # Test 11: Pilot hold time
        print("TEST 11: PILOT:HOLD (Hold time in ms)")
        self.test_set_get("PILOT:HOLD 2000", "PILOT:HOLD?", "HOLD")

        # Test 12: Audio pre-emphasis
        print("TEST 12: AUDIO:PREEMPH (Pre-emphasis enable)")
        self.test_set_get("AUDIO:PREEMPH 1", "AUDIO:PREEMPH?", "PREEMPH=1")
        self.test_set_get("AUDIO:PREEMPH 0", "AUDIO:PREEMPH?", "PREEMPH=0")

        # Test 13: Audio Status
        print("TEST 13: AUDIO:STATUS? (Aggregate audio status)")
        self.test_get_only("AUDIO:STATUS?")

        # Test 14: System version
        print("TEST 14: SYST:VERSION? (System version info)")
        self.test_get_only("SYST:VERSION?")

        # Test 15: System status
        print("TEST 15: SYST:STATUS? (System health metrics)")
        self.test_get_only("SYST:STATUS?")

        # Test 16: Heap status
        print("TEST 16: SYST:HEAP? (Memory usage)")
        self.test_get_only("SYST:HEAP?")

        # Test 17: Log level control
        print("TEST 17: SYST:LOG:LEVEL (Log level configuration)")
        self.test_set_get("SYST:LOG:LEVEL WARN", "SYST:LOG:LEVEL?", "LEVEL")
        self.test_set_get("SYST:LOG:LEVEL INFO", "SYST:LOG:LEVEL?", "LEVEL")

        # Test 18: JSON mode toggle
        print("TEST 18: SYST:COMM:JSON (JSON mode enable/disable)")
        self.test_set_get("SYST:COMM:JSON ON", "SYST:COMM:JSON?")
        self.test_set_get("SYST:COMM:JSON OFF", "SYST:COMM:JSON?")

        # Test 19: Consistency test - rapid fire commands
        print("TEST 19: CONSISTENCY - Rapid command sequence")
        self.test_consistency_sequence()

        # Test 20: Error handling
        print("TEST 20: ERROR HANDLING - Invalid commands")
        self.test_error_case("INVALID:COMMAND 123", expect_error=True)
        self.test_error_case("RDS:PI", expect_error=True)  # Missing argument

        # Print results
        self.print_summary()

    def test_consistency_sequence(self):
        """Test that commands work consistently without degradation"""
        passed = 0
        total = 20

        for i in range(total):
            resp = self.send_command(f"RDS:PTY {i % 32}")
            if resp.startswith("OK"):
                passed += 1
                sys.stdout.write(f".")
            else:
                sys.stdout.write(f"F")
            sys.stdout.flush()

        print()
        test_name = f"CONSISTENCY: 20 rapid RDS:PTY commands"
        self.results.append(TestResult(test_name, passed == total, f"{total}/20", f"{passed}/20"))

    def print_summary(self):
        """Print test results summary"""
        print("\n" + "="*80)
        print("TEST RESULTS SUMMARY")
        print("="*80 + "\n")

        passed = sum(1 for r in self.results if r.passed)
        total = len(self.results)

        for result in self.results:
            status = "✓ PASS" if result.passed else "✗ FAIL"
            print(f"{status}: {result.name}")
            if not result.passed:
                print(f"  Expected: {result.expected}")
                print(f"  Received: {result.received}")
                if result.error:
                    print(f"  Error: {result.error}")

        print(f"\n{'='*80}")
        print(f"SUMMARY: {passed}/{total} tests passed ({100*passed//total}%)")
        print(f"Total commands sent: {self.command_count}")
        print(f"{'='*80}\n")

        return passed == total

def main():
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = "/dev/tty.usbmodem5AAF1766471"

    tester = CommandTester(port)

    if not tester.connect():
        return 1

    try:
        tester.run_all_tests()
    finally:
        tester.disconnect()

    return 0

if __name__ == "__main__":
    sys.exit(main())
