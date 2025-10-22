#!/usr/bin/env python3
"""
Serial Console Test Suite - Pure Python implementation
Tests commands against running device without external dependencies
"""

import subprocess
import time
import sys
from dataclasses import dataclass
from typing import List, Tuple

@dataclass
class TestResult:
    name: str
    command: str
    expected: str
    response: str
    passed: bool

class SerialConsoleTest:
    def __init__(self, device: str = "/dev/tty.usbmodem5AAF1766471"):
        self.device = device
        self.results: List[TestResult] = []

    def send_cmd(self, cmd: str, timeout: float = 1.0) -> str:
        """Send command using screen or stty"""
        try:
            # Use screen with -L option to send command and capture
            full_cmd = f"screen -S console -L -Logfile /tmp/screen.log -dm -h 100 {self.device} 115200"

            # Try sending with printf
            send_cmd = f'printf "{cmd}\\r" > {self.device}'
            subprocess.run(send_cmd, shell=True, timeout=0.5, stderr=subprocess.DEVNULL)

            # Wait for device to process
            time.sleep(timeout)

            # Read response from device
            read_cmd = f"timeout 0.2 cat < {self.device}"
            result = subprocess.run(read_cmd, shell=True, capture_output=True, text=True)
            response = result.stdout + result.stderr

            return response.strip()
        except Exception as e:
            return f"ERROR: {str(e)}"

    def test(self, name: str, cmd: str, expected: str) -> TestResult:
        """Run a single test"""
        response = self.send_cmd(cmd)
        passed = expected.lower() in response.lower()

        result = TestResult(
            name=name,
            command=cmd,
            expected=expected,
            response=response[:150] if response else "(no response)",
            passed=passed
        )
        self.results.append(result)
        return result

    def print_result(self, result: TestResult):
        """Print single test result"""
        status = "✓ PASS" if result.passed else "✗ FAIL"
        print(f"{status} | {result.name}")
        if not result.passed:
            print(f"     Cmd: {result.command}")
            print(f"     Exp: {result.expected}")
            print(f"     Got: {result.response}")

    def print_summary(self):
        """Print test summary"""
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed

        print("\n" + "="*70)
        print("TEST SUMMARY")
        print("="*70)
        print(f"Total:   {total}")
        print(f"Passed:  {passed}")
        print(f"Failed:  {failed}")
        if total > 0:
            success_rate = 100.0 * passed / total
            print(f"Success: {success_rate:.1f}%")
        print("="*70)

        if failed > 0:
            print("\nFailed tests:")
            for r in self.results:
                if not r.passed:
                    print(f"  • {r.name}")

def main():
    print("="*70)
    print("Serial Console Command Test Suite")
    print("="*70)
    print("")

    tester = SerialConsoleTest()

    # Step 1: Disable logging
    print("STEP 1: Disable logging")
    print("-" * 70)
    tester.send_cmd("SYST:LOG:LEVEL OFF")
    time.sleep(0.5)
    print("  Sent: SYST:LOG:LEVEL OFF")
    print("")

    # Step 2: Reset to defaults
    print("STEP 2: Reset to defaults")
    print("-" * 70)
    tester.send_cmd("SYST:CONF:DEFAULT")
    time.sleep(0.5)
    print("  Sent: SYST:CONF:DEFAULT")
    print("")

    # Step 3: Test suite
    print("STEP 3: Running tests")
    print("-" * 70)
    print("")

    print("--- RDS Commands ---")
    tester.test("RDS:PI set hex", "RDS:PI 0x52A1", "OK")
    tester.test("RDS:PI read", "RDS:PI?", "OK")
    tester.test("RDS:PTY set", "RDS:PTY 10", "OK")
    tester.test("RDS:PTY read", "RDS:PTY?", "OK")
    tester.test("RDS:PTY:LIST", "RDS:PTY:LIST?", "OK")
    tester.test("RDS:TP set", "RDS:TP 1", "OK")
    tester.test("RDS:TA set", "RDS:TA 1", "OK")
    tester.test("RDS:MS set", "RDS:MS 1", "OK")
    tester.test("RDS:PS set", "RDS:PS \"TestPS\"", "OK")
    tester.test("RDS:PS read", "RDS:PS?", "OK")
    tester.test("RDS:RT set", "RDS:RT \"Title\"", "OK")
    tester.test("RDS:ENABLE", "RDS:ENABLE 1", "OK")
    tester.test("RDS:STATUS", "RDS:STATUS?", "OK")

    print("\n--- RTLIST Commands ---")
    tester.test("RDS:RTLIST:CLEAR", "RDS:RTLIST:CLEAR", "OK")
    tester.test("RDS:RTLIST:ADD", "RDS:RTLIST:ADD \"Text\"", "OK")
    tester.test("RDS:RTLIST read", "RDS:RTLIST?", "OK")
    tester.test("RDS:RTPERIOD set", "RDS:RTPERIOD 30", "OK")

    print("\n--- Audio Commands ---")
    tester.test("AUDIO:STEREO", "AUDIO:STEREO 1", "OK")
    tester.test("AUDIO:PREEMPH", "AUDIO:PREEMPH 1", "OK")
    tester.test("AUDIO:STATUS", "AUDIO:STATUS?", "OK")

    print("\n--- Pilot Commands ---")
    tester.test("PILOT:ENABLE", "PILOT:ENABLE 1", "OK")
    tester.test("PILOT:AUTO", "PILOT:AUTO 1", "OK")
    tester.test("PILOT:THRESH", "PILOT:THRESH 0.001", "OK")
    tester.test("PILOT:HOLD", "PILOT:HOLD 2000", "OK")

    print("\n--- System Commands ---")
    tester.test("SYST:VERSION", "SYST:VERSION?", "OK")
    tester.test("SYST:STATUS", "SYST:STATUS?", "OK")
    tester.test("SYST:HEAP", "SYST:HEAP?", "OK")
    tester.test("SYST:CONF:SAVE", "SYST:CONF:SAVE test", "OK")

    print("\n" + "="*70)
    print("DETAILED RESULTS")
    print("="*70)
    for result in tester.results:
        tester.print_result(result)

    tester.print_summary()

if __name__ == "__main__":
    main()
