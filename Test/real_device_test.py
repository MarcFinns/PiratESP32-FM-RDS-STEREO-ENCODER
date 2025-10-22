#!/usr/bin/env python3
"""
Real device test using pyserial
Actually communicates with the device to verify fixes
"""

import serial
import time
import sys

class DeviceTester:
    def __init__(self, port, baudrate=115200, timeout=2):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self.results = []

    def connect(self):
        """Connect to serial device"""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
            time.sleep(1)  # Wait for connection to settle
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

    def send_cmd(self, cmd):
        """Send command and read response"""
        if not self.ser:
            return ""

        try:
            # Clear buffers
            self.ser.reset_input_buffer()
            time.sleep(0.05)

            # Send command
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()

            # Read response
            response = ""
            start_time = time.time()
            while time.time() - start_time < self.timeout:
                if self.ser.in_waiting > 0:
                    chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    response += chunk
                    # Check if we got a complete response
                    if response.strip().endswith('OK') or response.startswith('ERR'):
                        time.sleep(0.1)  # Small delay to catch any trailing data
                        if self.ser.in_waiting > 0:
                            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                        break
                time.sleep(0.01)

            return response.strip()
        except Exception as e:
            return f"ERROR: {e}"

    def test(self, name, cmd, expected=None):
        """Test a command and verify response"""
        print(f"\n>>> {name}")
        print(f"    Cmd: {cmd}")

        response = self.send_cmd(cmd)
        print(f"    Resp: {response[:150]}")

        if expected is None:
            # Just checking command doesn't error
            passed = not response.startswith("ERR") and response != ""
        else:
            passed = expected.lower() in response.lower()

        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"    {status}")
        self.results.append((name, passed))
        return passed

    def test_set_get(self, name, set_cmd, get_cmd, expected_value):
        """Test set/get persistence"""
        print(f"\n>>> {name} (SET/GET)")
        print(f"    SET: {set_cmd}")

        set_resp = self.send_cmd(set_cmd)
        print(f"    Response: {set_resp}")

        if not set_resp.startswith("OK"):
            print(f"    ✗ FAIL - Set failed")
            self.results.append((name, False))
            return False

        time.sleep(0.3)

        print(f"    GET: {get_cmd}")
        get_resp = self.send_cmd(get_cmd)
        print(f"    Response: {get_resp[:150]}")

        passed = expected_value.lower() in get_resp.lower()
        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"    {status} - Expected: {expected_value}")
        self.results.append((name, passed))
        return passed

    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*70)
        print("TEST SUMMARY")
        print("="*70)

        passed = sum(1 for _, p in self.results if p)
        total = len(self.results)

        print(f"Total Tests: {total}")
        print(f"Passed: {passed}")
        print(f"Failed: {total - passed}")

        if total > 0:
            success_rate = 100 * passed / total
            print(f"Success Rate: {success_rate:.1f}%")

        # List failed tests
        failed = [name for name, p in self.results if not p]
        if failed:
            print("\nFailed Tests:")
            for name in failed:
                print(f"  • {name}")

        print("="*70)
        return passed == total

def main():
    PORT = "/dev/tty.usbmodem5AAF1766471"

    tester = DeviceTester(PORT)

    if not tester.connect():
        sys.exit(1)

    try:
        print("\n" + "="*70)
        print("REAL DEVICE TEST - ACTUAL SERIAL COMMUNICATION")
        print("="*70)

        # Step 1: Setup
        print("\n" + "="*70)
        print("STEP 1: SETUP")
        print("="*70)

        tester.test("Disable Logging", "SYST:LOG:LEVEL OFF", "OK")
        time.sleep(1)

        tester.test("Reset to Defaults", "SYST:CONF:DEFAULT", "OK")
        time.sleep(1)

        # Step 2: Test Issue #2 fixes (PS?/RT? format)
        print("\n" + "="*70)
        print("STEP 2: RDS:PS? AND RDS:RT? FORMAT TESTS (ISSUE #2)")
        print("="*70)

        tester.test_set_get(
            "RDS:PS Persistence",
            'RDS:PS "TestPS"',
            "RDS:PS?",
            "TestPS"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:PS Different Value",
            'RDS:PS "PirateFM"',
            "RDS:PS?",
            "PirateFM"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:RT Persistence",
            'RDS:RT "Artist - Title"',
            "RDS:RT?",
            "Artist - Title"
        )
        time.sleep(0.5)

        # Step 3: Test Issue #1 fixes (CONF commands)
        print("\n" + "="*70)
        print("STEP 3: SYST:CONF:* COMMANDS (ISSUE #1)")
        print("="*70)

        tester.test("SYST:CONF:DEFAULT", "SYST:CONF:DEFAULT", "OK")
        time.sleep(0.5)

        tester.test("SYST:CONF:SAVE", "SYST:CONF:SAVE test_config", "OK")
        time.sleep(0.5)

        tester.test("SYST:CONF:LIST?", "SYST:CONF:LIST?", "OK")
        time.sleep(0.5)

        tester.test("SYST:CONF:ACTIVE?", "SYST:CONF:ACTIVE?", "OK")
        time.sleep(0.5)

        tester.test("SYST:CONF:DELETE", "SYST:CONF:DELETE test_config", "OK")
        time.sleep(0.5)

        # Step 4: Test RDS set/get persistence
        print("\n" + "="*70)
        print("STEP 4: RDS PARAMETER PERSISTENCE TESTS")
        print("="*70)

        tester.test_set_get(
            "RDS:PI (hex)",
            "RDS:PI 0x52A1",
            "RDS:PI?",
            "52A1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:PI (decimal)",
            "RDS:PI 21153",
            "RDS:PI?",
            "21153"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:PTY",
            "RDS:PTY 10",
            "RDS:PTY?",
            "10"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:TP",
            "RDS:TP 1",
            "RDS:TP?",
            "1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:TA",
            "RDS:TA 1",
            "RDS:TA?",
            "1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:MS",
            "RDS:MS 0",
            "RDS:MS?",
            "0"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "RDS:ENABLE",
            "RDS:ENABLE 1",
            "RDS:ENABLE?",
            "1"
        )
        time.sleep(0.5)

        # Step 5: Test Issue #3 fix (PTY:LIST?)
        print("\n" + "="*70)
        print("STEP 5: RDS:PTY:LIST? (ISSUE #3)")
        print("="*70)

        tester.test("RDS:PTY:LIST? Response", "RDS:PTY:LIST?", "OK")
        time.sleep(0.5)

        tester.test("RDS:PTY:LIST? has POP_MUSIC", "RDS:PTY:LIST?", "POP_MUSIC")
        time.sleep(0.5)

        # Step 6: Audio/Pilot tests
        print("\n" + "="*70)
        print("STEP 6: AUDIO AND PILOT TESTS")
        print("="*70)

        tester.test_set_get(
            "AUDIO:STEREO",
            "AUDIO:STEREO 1",
            "AUDIO:STEREO?",
            "1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "AUDIO:PREEMPH",
            "AUDIO:PREEMPH 1",
            "AUDIO:PREEMPH?",
            "1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "PILOT:ENABLE",
            "PILOT:ENABLE 1",
            "PILOT:ENABLE?",
            "1"
        )
        time.sleep(0.5)

        tester.test_set_get(
            "PILOT:THRESH",
            "PILOT:THRESH 0.001",
            "PILOT:THRESH?",
            "0.001"
        )
        time.sleep(0.5)

        # Step 7: System tests
        print("\n" + "="*70)
        print("STEP 7: SYSTEM COMMANDS")
        print("="*70)

        tester.test("SYST:VERSION?", "SYST:VERSION?", "OK")
        time.sleep(0.5)

        tester.test("SYST:STATUS?", "SYST:STATUS?", "OK")
        time.sleep(0.5)

        tester.test("SYST:HEAP?", "SYST:HEAP?", "OK")
        time.sleep(0.5)

        # Print summary
        success = tester.print_summary()

        if success:
            print("\n✓ ALL TESTS PASSED!")
            return 0
        else:
            print("\n✗ SOME TESTS FAILED")
            return 1

    finally:
        tester.disconnect()

if __name__ == "__main__":
    sys.exit(main())
