#!/usr/bin/env python3
"""brg_console.py — 跟桥固件的 CDC 控制台对话

用法:  python3 brg_console.py [串口] [要发的字符] [读多久秒]
        python3 brg_console.py /dev/cu.usbmodem1102 i 3
"""
import os
import sys
import termios
import time
import tty

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1102"
keys = sys.argv[2] if len(sys.argv) > 2 else "i"
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    time.sleep(0.3)
    while True:                                    # 冲掉积压
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break
    os.write(fd, keys.encode())
    end = time.time() + dur
    buf = b""
    while time.time() < end:
        try:
            chunk = os.read(fd, 4096)
            if chunk:
                buf += chunk
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            break
        time.sleep(0.02)
    sys.stdout.write(buf.decode("utf-8", "replace"))
finally:
    os.close(fd)
