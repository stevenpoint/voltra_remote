#!/usr/bin/env python3
"""
Screenshots from the watch (or knob) over USB, for design work.

    ~/.platformio/penv/bin/python tools/screenshot.py                    # current screen
    ~/.platformio/penv/bin/python tools/screenshot.py -o shots "demo set" "show main"
    ~/.platformio/penv/bin/python tools/screenshot.py --all -o shots     # every screen and state

Each argument is a serial command sent before the screenshot (see ui_command() in
src/ui/ui.h): "show settings", "demo ecc", "set attach 45"... The demo states stand in for a
Voltra, so no Voltra is needed; "demo off" goes back to the real one and the saved
attachment weight.

Plug the watch in over USB first, and close any serial monitor. Opening the port restarts
the watch, so give all of a screenshot's commands in one run. Every run ends by putting
the watch back on the real Voltra.
"""

import argparse
import os
import re
import struct
import sys
import time
import zlib

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
SHOT_RE = re.compile(rb"\nSHOT (\d+) (\d+) (\d) (\d+)\n")

# (file name, commands before the shot) for --all
ALL = [
    ("main_idle", ["demo idle", "set attach 0", "show main"]),
    ("main_loaded", ["demo loaded", "show main"]),
    ("main_set", ["demo set", "show main"]),
    ("main_set_eccentric", ["set ecc 15", "demo ecc", "show main"]),
    ("main_accessories", ["set ecc 15", "set chains 20", "demo idle", "show main"]),
    ("main_attachment", ["set ecc 0", "set chains 0", "set attach 45", "demo idle", "show main"]),
    ("main_twin", ["set attach 0", "demo twin", "show main"]),
    ("settings", ["set ecc 15", "set chains 20", "set attach 45", "demo idle", "show settings"]),
    ("adjust_eccentric", ["show ecc"]),
    ("adjust_chains", ["show chains"]),
    ("adjust_attachment", ["show attach"]),
    ("connect", ["show connect"]),
]
RESTORE = ["set ecc 0", "set chains 0", "demo off", "show main"]


def find_port():
    ports = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not ports:
        sys.exit("No Espressif USB device found. Is the watch plugged in? Or pass --port.")
    return ports[0].device


def open_port(name):
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = 115200
    ser.timeout = 0.2
    # Keep both lines low: toggling them is how esptool resets the chip.
    ser.dtr = False
    ser.rts = False
    ser.write_timeout = 5
    ser.open()
    return ser


def wait_ready(ser, timeout_s=15):
    """Opening the port restarts the watch: wait until it answers again."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ser.reset_input_buffer()
        ser.write(b"\nping\n")
        buf = b""
        end = time.time() + 1
        while time.time() < end:
            buf += ser.read(256)
            if b"\nOK\n" in buf:
                return
    sys.exit("The watch did not answer. Is a build with screenshots flashed?")


def command(ser, cmd, timeout_s=3):
    ser.reset_input_buffer()
    ser.write(b"\n" + cmd.encode() + b"\n")
    buf = b""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        buf += ser.read(256)
        if b"\nOK\n" in buf:
            return
        if b"\nERR" in buf:
            sys.exit(f"The watch did not take {cmd!r}.")
    sys.exit(f"No answer to {cmd!r}. Is a build with screenshots flashed?")


def shot(ser, timeout_s=15):
    ser.reset_input_buffer()
    ser.write(b"\nshot\n")
    buf = b""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        buf += ser.read(65536)
        m = SHOT_RE.search(buf)
        if b"\nSHOT ERR" in buf:
            sys.exit("The watch could not take the screenshot (out of memory?).")
        if m:
            w, h, swap, size = (int(x) for x in m.groups())
            start = m.end()
            if len(buf) >= start + size:
                return w, h, swap, buf[start:start + size]
            deadline = max(deadline, time.time() + 5)   # data still arriving
    sys.exit("No screenshot came back.")


def rgb565_to_png(w, h, swap, data):
    rows = []
    for y in range(h):
        row = bytearray([0])   # filter: none
        for x in range(w):
            i = (y * w + x) * 2
            v = (data[i] << 8) | data[i + 1] if swap else data[i] | (data[i + 1] << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6))
        rows.append(bytes(row))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def capture(ser, cmds, path):
    for c in cmds:
        command(ser, c)
    time.sleep(0.4)   # let a poll pick up a new demo state
    w, h, swap, data = shot(ser)
    with open(path, "wb") as f:
        f.write(rgb565_to_png(w, h, swap, data))
    print(f"{path}  ({w}x{h})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*", help="serial commands to send before the screenshot")
    ap.add_argument("--port", help="serial port (default: first Espressif USB device)")
    ap.add_argument("-o", "--out", default=".", help="folder to save into")
    ap.add_argument("-n", "--name", default="screen", help="file name, without .png")
    ap.add_argument("--all", action="store_true", help="every screen and demo state, then back to normal")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ser = open_port(args.port or find_port())
    wait_ready(ser)
    try:
        if args.all:
            for name, cmds in ALL:
                capture(ser, cmds, os.path.join(args.out, name + ".png"))
        else:
            capture(ser, args.commands, os.path.join(args.out, args.name + ".png"))
    finally:
        # Never leave the watch on a stand-in Voltra (it would also lapse on its own).
        for c in RESTORE:
            try:
                command(ser, c)
            except SystemExit:
                pass
        ser.close()


if __name__ == "__main__":
    main()
