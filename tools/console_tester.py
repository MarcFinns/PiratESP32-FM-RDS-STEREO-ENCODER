#!/usr/bin/env python3
import sys
import time
import re
import os

try:
    import serial  # pyserial
except Exception as e:
    print("ERR: pyserial not available (pip install pyserial)")
    sys.exit(2)


OK_RE = re.compile(r"^OK(?:\s+(.+))?$", re.IGNORECASE)
ERR_RE = re.compile(r"^ERR\s+(\S+)\s*(.*)$", re.IGNORECASE)


class ConsoleClient:
    def __init__(self, port, baud=115200, timeout=1.0):
        # Allow bare basename like 'tty.usbmodemXYZ'
        if not port.startswith('/dev/') and os.name != 'nt':
            port = f"/dev/{port}"
        self.port = port
        self.baud = baud
        self.s = serial.Serial(port=port, baudrate=baud, timeout=timeout)
        # Give the device a moment after (re)open
        time.sleep(0.2)
        # Drain any banner noise
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def send(self, line):
        if not line.endswith("\n"):
            line += "\n"
        self.s.write(line.encode('utf-8'))

    def read_reply(self, wait_ok_err=2.0):
        """Read lines until 'OK' or 'ERR ...' is seen, or timeout."""
        deadline = time.time() + wait_ok_err
        lines = []
        while time.time() < deadline:
            raw = self.s.readline()
            if not raw:
                continue
            try:
                line = raw.decode('utf-8', errors='replace').strip()
            except Exception:
                line = raw.decode('latin1', errors='replace').strip()

            if not line:
                continue

            # Keep non-OK/ERR as noise unless JSON mode
            if line.startswith('{') and line.endswith('}'):
                return True, line

            m_ok = OK_RE.match(line)
            if m_ok:
                return True, m_ok.group(1) or ""

            m_err = ERR_RE.match(line)
            if m_err:
                return False, f"{m_err.group(1)} {m_err.group(2)}".strip()

            # Save incidental lines for debugging
            lines.append(line)
        if lines:
            return False, "Timeout; seen: " + " | ".join(lines[-4:])
        return False, "Timeout; no response"


def expect_ok(cli, cmd):
    cli.send(cmd)
    ok, data = cli.read_reply()
    return ok, data


def expect_get(cli, cmd, key):
    cli.send(cmd)
    ok, data = cli.read_reply()
    if not ok:
        return False, data
    # Parse simple key=value pairs; strings may be quoted
    # Accept single or multiple pairs; find our key
    pairs = {}
    for part in (data or "").split(','):
        part = part.strip()
        if '=' in part:
            k, v = part.split('=', 1)
            pairs[k.strip().upper()] = v.strip()
    if key.upper() in pairs:
        return True, pairs[key.upper()]
    # If data was a single value (e.g., PS="foo") already returned
    return True, data


def main():
    import argparse
    ap = argparse.ArgumentParser(description="PiratESP32 Console SCPI tester")
    ap.add_argument('--port', required=True, help='Serial port (e.g., /dev/tty.usbmodemXXXX)')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--json', action='store_true', help='Also test JSON mode switch')
    ap.add_argument('--rt-rotate', action='store_true', help='Exercise RT rotation basics')
    args = ap.parse_args()

    cli = ConsoleClient(args.port, args.baud)
    results = []

    def add_result(name, ok, msg=""):
        results.append((name, ok, msg))

    try:
        # 0) Version sanity check (optional; ignore failure)
        ok, data = expect_get(cli, "SYST:VERSION?", "VERSION")
        add_result("SYST:VERSION?", ok, data)

        # 1) Try disabling logs to keep line discipline clean
        ok, data = expect_ok(cli, "SYST:LOG:LEVEL OFF")
        add_result("SYST:LOG:LEVEL OFF", ok, data)

        # 2) Basic RDS core
        for cmd, qcmd, key, expect in [
            ("RDS:PI 0x52A1", "RDS:PI?", "PI", None),
            ("RDS:PTY 10", "RDS:PTY?", "PTY", "10"),
            ("RDS:TP 1", "RDS:TP?", "TP", "1"),
            ("RDS:TA 0", "RDS:TA?", "TA", "0"),
            ("RDS:MS 1", "RDS:MS?", "MS", "1"),
        ]:
            ok, data = expect_ok(cli, cmd)
            add_result(cmd, ok, data)
            ok, val = expect_get(cli, qcmd, key)
            add_result(qcmd, ok and ((expect is None) or (val.strip('"') == expect)), f"got={val}")

        # 3) PS set/get (trim quotes and allow auto-padding internally)
        ok, data = expect_ok(cli, 'RDS:PS "Pippo"')
        add_result('RDS:PS "Pippo"', ok, data)
        ok, val = expect_get(cli, 'RDS:PS?', 'PS')
        # Accept with or without quotes
        vv = val.strip('"')
        add_result('RDS:PS?', ok and vv.startswith('Pippo'), f"got={val}")

        # 4) RT set/get
        ok, data = expect_ok(cli, 'RDS:RT "Artist • Title • Album"')
        add_result('RDS:RT "Artist • Title • Album"', ok, data)
        ok, val = expect_get(cli, 'RDS:RT?', 'RT')
        add_result('RDS:RT?', ok and ('Artist' in val), f"got={val}")

        # 5) RDS enable toggle
        for en in (1, 0, 1):
            ok, data = expect_ok(cli, f"RDS:ENABLE {en}")
            add_result(f"RDS:ENABLE {en}", ok, data)
            ok, val = expect_get(cli, 'RDS:ENABLE?', 'ENABLE')
            add_result('RDS:ENABLE?', ok and val.strip('"') in (str(en), f"ENABLE={en}"), f"got={val}")

        # 6) RT rotation basics (optional)
        if args.rt_rotate:
            seq = [
                ("RDS:RTMODE STATIC", True),
                ("RDS:RTLIST:CLEAR", True),
                ('RDS:RTLIST:ADD "One"', True),
                ('RDS:RTLIST:ADD "Two"', True),
                ("RDS:RTPERIOD 2", True),
                ("RDS:RTMODE ROTATE", True),
            ]
            for cmd, _ in seq:
                ok, data = expect_ok(cli, cmd)
                add_result(cmd, ok, data)
            ok, data = expect_ok(cli, 'RDS:RTLIST?')
            add_result('RDS:RTLIST?', ok and ('One' in (data or '') and 'Two' in (data or '')), f"got={data}")

        # 7) JSON mode switch (optional)
        if args.json:
            ok, data = expect_ok(cli, 'SYST:COMM:JSON ON')
            add_result('SYST:COMM:JSON ON', ok, data)
            # One JSON query
            cli.send('RDS:PTY?')
            ok, data = cli.read_reply()
            add_result('RDS:PTY? (JSON)', ok and data.startswith('{'), data)
            ok, data = expect_ok(cli, 'SYST:COMM:JSON OFF')
            add_result('SYST:COMM:JSON OFF', ok, data)

    finally:
        cli.close()

    # Summary
    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("=== Test Summary ===")
    for name, ok, msg in results:
        status = "PASS" if ok else "FAIL"
        if msg:
            print(f"{status:4}  {name:30}  {msg}")
        else:
            print(f"{status:4}  {name}")
    print(f"Result: {passed}/{total} passed")
    sys.exit(0 if passed == total else 1)


if __name__ == '__main__':
    main()
