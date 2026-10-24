#!/usr/bin/env python3
"""Explicit hardware test, never called by builds. Sends only status, never resets/learns."""
import argparse
import re
import sys
import time


def check_status(port, repetitions, deadline_seconds):
    for iteration in range(1, repetitions + 1):
        command = b"status\n"
        if port.write(command) != len(command):
            raise RuntimeError("incomplete status write")
        deadline = time.monotonic() + deadline_seconds
        status_seen = False
        endpoints = set()
        lines = []
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            lines.append(line)
            if "STATUS mode=" in line:
                status_seen = True
                endpoints.clear()
            match = re.search(r"binding endpoint=(10|11|12|13)\b", line)
            if status_seen and match:
                endpoints.add(int(match.group(1)))
            if status_seen and endpoints == {10, 11, 12, 13}:
                print(f"PASS status {iteration}/{repetitions}: STATUS + four bindings")
                break
        else:
            raise RuntimeError("status response timeout; received:\n" + "\n".join(lines))


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--repeat", type=int, default=10)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    # Set control lines before opening: no intentional BOOT/RESET pulse.
    port = serial.Serial(port=None, baudrate=115200, timeout=0.25,
                         write_timeout=2, exclusive=True)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            check_status(port, args.repeat, 5)
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
