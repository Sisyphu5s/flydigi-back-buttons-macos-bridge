#!/usr/bin/env python3
"""Capture live EF button transitions from the bridge CDC console.

Usage: python3 capture_mkeys.py [seconds] [log_path] [serial_port]
       python3 capture_mkeys.py --triggered [seconds] [log_path] [serial_port]
The triggered form waits up to 120 seconds for the first button change, then
records for the requested duration.
The current firmware's 'x' command reads raw frames; 'r' resends receiver
initialization and must not be used for passive capture.
"""

import glob
import os
import pathlib
import select
import sys
import termios
import time
import tty


BUTTONS = (
    "Up", "Right", "Down", "Left", "A", "B", "Select", "X",
    "Y", "Start", "LB", "RB", "b2.4", "b2.5", "L3", "R3",
    "C", "Z", "M1", "M2", "M3", "M4", "LM", "RM",
    "O", "ext2.1", "ext2.2", "Home", "ext2.4", "ext2.5", "ext2.6", "ext2.7",
)


def choose_port(args):
    if len(args) > 2:
        return args[2]
    ports = glob.glob("/dev/cu.usbmodem*")
    if len(ports) != 1:
        raise RuntimeError(f"expected one bridge CDC port, found {ports}; pass serial_port")
    return ports[0]


def read_lines(fd, pending, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0.0, min(0.05, deadline - time.monotonic())))
        if ready:
            try:
                pending += os.read(fd, 8192)
            except BlockingIOError:
                continue
    parts = pending.split(b"\n")
    return [part.decode("utf-8", "replace").strip() for part in parts[:-1]], parts[-1]


def frame_from_line(line):
    prefix = "[RAW] last EF frame:"
    if not line.startswith(prefix):
        return None
    try:
        frame = bytes.fromhex(line[len(prefix):].strip())
    except ValueError:
        return None
    return frame if len(frame) == 32 and frame[:3] == b"\x5a\xa5\xef" else None


def main(argv):
    args = argv[1:]
    triggered = bool(args and args[0] == "--triggered")
    if triggered:
        args = args[1:]
    duration = max(1.0, float(args[0])) if args else 60.0
    log_path = pathlib.Path(args[1]) if len(args) > 1 else pathlib.Path("capture-mkeys.log")
    port = choose_port(args)
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original_attrs = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B115200
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        pending = b""
        _, pending = read_lines(fd, pending, 0.25)
        started = time.monotonic()
        trigger_deadline = started + 120.0
        capturing = not triggered
        previous = None
        samples = changes = 0
        with log_path.open("w", encoding="utf-8", buffering=1) as log:
            log.write(f"# port={port} duration={duration:.1f}s triggered={triggered}\n")
            if triggered:
                print(f"Waiting for EF button change on {port} (120s) -> {log_path}", flush=True)
            else:
                print(f"Capturing EF buttons on {port} for {duration:.0f}s -> {log_path}", flush=True)
            while time.monotonic() < (started + duration if capturing else trigger_deadline):
                os.write(fd, b"x")
                lines, pending = read_lines(fd, pending, 0.18)
                timestamp = time.monotonic() - started
                for line in lines:
                    frame = frame_from_line(line)
                    if frame is None:
                        continue
                    samples += 1
                    buttons = int.from_bytes(frame[11:15], "little")
                    just_triggered = False
                    if not capturing and previous is not None and buttons != previous:
                        capturing = True
                        just_triggered = True
                        started = time.monotonic()
                        timestamp = 0.0
                    if previous is None:
                        message = f"t={timestamp:7.2f} initial buttons={buttons:08x} frame={frame.hex(' ')}"
                    elif buttons != previous:
                        events = [f"{'down' if buttons & (1 << bit) else 'up'} {BUTTONS[bit]}"
                                  for bit in range(32) if (buttons ^ previous) & (1 << bit)]
                        prefix = 'trigger ' if just_triggered else ''
                        message = f"t={timestamp:7.2f} {prefix}{', '.join(events)} buttons={buttons:08x} frame={frame.hex(' ')}"
                        changes += 1
                    else:
                        message = None
                    if message and (capturing or previous is None):
                        print(message, flush=True)
                        log.write(message + "\n")
                    previous = buttons
            summary = f"# samples={samples} button_change_frames={changes} triggered={capturing}"
            print(summary, flush=True)
            log.write(summary + "\n")
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original_attrs)
        os.close(fd)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, RuntimeError, ValueError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
