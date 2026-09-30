#!/usr/bin/env python3
"""Sample live EF frames through the bridge CDC console and inspect byte 31."""
import os
import re
import select
import sys
import termios
import time
import tty

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem104"
samples = int(sys.argv[2]) if len(sys.argv) > 2 else 100
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    received = bytearray()
    for _ in range(samples):
        os.write(fd, b"x")
        until = time.monotonic() + 0.05
        while time.monotonic() < until:
            if select.select([fd], [], [], 0.01)[0]:
                try:
                    received.extend(os.read(fd, 8192))
                except BlockingIOError:
                    pass
    pattern = rb"\[RAW\] last EF frame:((?: [0-9a-f]{2}){32})"
    frames = [bytes.fromhex(match.decode()) for match in re.findall(pattern, received)]
    unique = set(frames)
    matching = [frame for frame in unique if (sum(frame[:31]) + 1) & 0xff == frame[31]]
    print(f"EF captures={len(frames)} unique={len(unique)} checksum matches={len(matching)}")
    for frame in list(unique - set(matching))[:5]:
        print("mismatch:", frame.hex(" "), "expected", f"{(sum(frame[:31]) + 1) & 0xff:02x}")
    if not unique or len(matching) != len(unique):
        sys.exit(1)
finally:
    os.close(fd)
