#!/usr/bin/env python3
"""
Pull the log a diagnostic build saved in flash (src/diag/flash_log.cpp).

    ~/.platformio/penv/bin/python tools/pull_log.py                 # save to voltra_capture.log
    ~/.platformio/penv/bin/python tools/pull_log.py -o run2.log
    ~/.platformio/penv/bin/python tools/pull_log.py --clear         # pull, then wipe it

Plug the watch (or knob) in over USB first. Close any serial monitor: only one program
can hold the port.
"""
import argparse
import re
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
BEGIN = b"=== VOLTRA LOG BEGIN ===\n"
END_RE = re.compile(rb"\n=== VOLTRA LOG END (\d+) ===\n")


def find_port():
    ports = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not ports:
        sys.exit("No Espressif USB device found. Is the watch plugged in? Or pass --port.")
    if len(ports) > 1:
        print("Several Espressif devices; using", ports[0].device, "(pass --port to choose)")
    return ports[0].device


def open_port(name):
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = 115200
    ser.timeout = 0.2
    # Keep both lines low: toggling them is how esptool resets the chip.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def pull(ser, timeout_s):
    buf = b""
    deadline = time.time() + timeout_s
    next_ask = 0.0
    while time.time() < deadline:
        start = buf.find(BEGIN)
        if start < 0 and time.time() >= next_ask:
            # Ask again until it answers: the device may still be booting.
            ser.write(b"\ndump\n")
            next_ask = time.time() + 2
        buf += ser.read(65536)
        if start >= 0:
            m = END_RE.search(buf, start + len(BEGIN))
            if m:
                body = buf[start + len(BEGIN):m.start()]
                return body, int(m.group(1))
            deadline = max(deadline, time.time() + 5)   # data still arriving
    sys.exit("No log came back. Is a diagnostic build (watch206_diag / remote_diag) flashed?")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first Espressif USB device)")
    ap.add_argument("-o", "--output", default="voltra_capture.log")
    ap.add_argument("--clear", action="store_true", help="delete the log on the device after pulling it")
    ap.add_argument("--timeout", type=float, default=20)
    args = ap.parse_args()

    ser = open_port(args.port or find_port())
    body, expected = pull(ser, args.timeout)
    with open(args.output, "wb") as f:
        f.write(body)
    note = "" if len(body) == expected else f" (expected {expected}: some bytes lost on the way)"
    print(f"Saved {len(body)} bytes to {args.output}{note}")

    if args.clear:
        if len(body) != expected:
            print("Not clearing, since the copy is incomplete.")
        else:
            ser.write(b"\nclear\n")
            time.sleep(1)
            print("Cleared the log on the device.")
    ser.close()


if __name__ == "__main__":
    main()
