#!/usr/bin/env python3
"""生成「冒充 Flydigi Vader2Pro.MobileUSB」所需的 HID 报告描述符。

目标（来自苹果模型库 GameControllers-Custom.bundle/Personalities/Flydigi/Vader2Pro/
MobileUSBWithBackButtons.plist 的一手导出，见 docs/11 §4）：
  * 6 个 8-bit 轴，Generic Desktop 顺序 X,Y,Z,Rx,Ry,Rz
      → 左摇杆 X,Y；右摇杆 Z,Rx；RT=Ry；LT=Rz
  * 1 个 hat switch（0x39）= dpad
  * 20 个按钮（Button page usage 1..20），M1..M4 = usage 15/16/17/18
布局刻意选成「usage-1 == 0-based 序号」的连续排列，使两种 UsageTypeIndex
解释（usage 偏移 / 同类位置）给出同一 index —— 见 docs/11 §4。

输出：desc_vader2pro.hex（每行 16 字节）+ desc_vader2pro.h（C 数组）。

输出报告：8 字节 vendor-defined Output，供浏览器/主机发送震动命令。
其字节布局沿用接收器 XInput 包：00 08 00 LL RR 00 00 00；
固件也兼容已有的 5a a5 12 06 LL RR 格式。
"""
import pathlib

D = []
def b(*xs):
    D.extend(xs)

# --- 描述符 ---
b(0x05, 0x01)              # Usage Page (Generic Desktop)
b(0x09, 0x04)              # Usage (Joystick)
b(0xA1, 0x01)              # Collection (Application)
b(0x05, 0x01)              #   Usage Page (Generic Desktop)
for u in (0x30, 0x31, 0x32, 0x33, 0x34, 0x35):   # X Y Z Rx Ry Rz
    b(0x09, u)
b(0x15, 0x00)              #   Logical Minimum (0)
b(0x26, 0xFF, 0x00)        #   Logical Maximum (255)
b(0x75, 0x08)              #   Report Size (8)
b(0x95, 0x06)              #   Report Count (6)
b(0x81, 0x02)              #   Input (Data,Var,Abs)
b(0x09, 0x39)              #   Usage (Hat switch)
b(0x15, 0x00)              #   Logical Minimum (0)
b(0x25, 0x07)              #   Logical Maximum (7)
b(0x35, 0x00)              #   Physical Minimum (0)
b(0x46, 0x3B, 0x01)        #   Physical Maximum (315)
b(0x65, 0x14)              #   Unit (Eng Rot: degrees)
b(0x75, 0x04)              #   Report Size (4)
b(0x95, 0x01)              #   Report Count (1)
b(0x81, 0x42)              #   Input (Data,Var,Abs,Null state)
b(0x65, 0x00)              #   Unit (None)
b(0x75, 0x04)              #   Report Size (4)
b(0x95, 0x01)              #   Report Count (1)
b(0x81, 0x03)              #   Input (Const,Var,Abs)  -> 4bit 填充
b(0x05, 0x09)              #   Usage Page (Button)
b(0x19, 0x01)              #   Usage Minimum (1)
b(0x29, 0x14)              #   Usage Maximum (20)
b(0x15, 0x00)              #   Logical Minimum (0)
b(0x25, 0x01)              #   Logical Maximum (1)
b(0x75, 0x01)              #   Report Size (1)
b(0x95, 0x14)              #   Report Count (20)
b(0x81, 0x02)              #   Input (Data,Var,Abs)
b(0x75, 0x01)              #   Report Size (1)
b(0x95, 0x04)              #   Report Count (4)
b(0x81, 0x03)              #   Input (Const,Var,Abs)  -> 4bit 填充
REPORT_SIZE_BYTES = 10      # 6 轴 + (hat4+pad4) + (20 按钮 + 4 填充) = 6+1+3
RUMBLE_REPORT_BYTES = 8

# 浏览器/主机要能发 Output report，描述符必须同时声明 Output 主项。
# 独立使用 vendor-defined usage，避免改变 Vader2Pro 输入元素的顺序。
b(0x06, 0x00, 0xff)        # Usage Page (Vendor-defined 0xFF00)
b(0x09, 0x01)              # Usage (rumble output)
b(0x15, 0x00)              # Logical Minimum (0)
b(0x26, 0xff, 0x00)        # Logical Maximum (255)
b(0x75, 0x08)              # Report Size (8)
b(0x95, RUMBLE_REPORT_BYTES)  # Report Count (8)
b(0x91, 0x02)              # Output (Data,Var,Abs)
b(0xc0)                    # End Collection

out = pathlib.Path(__file__).with_name("desc_vader2pro.hex")
lines = []
for i in range(0, len(D), 16):
    lines.append(" ".join(f"{x:02x}" for x in D[i:i+16]))
out.write_text("\n".join(lines) + "\n")

hdr = pathlib.Path(__file__).with_name("desc_vader2pro.h")
hdr.write_text(
    "/* 自动生成：hid_desc_vader2pro.py —— 冒充 Flydigi Vader2Pro.MobileUSB 的 HID 报告描述符 */\n"
    f"/* 描述符 {len(D)} 字节；输入报告 {REPORT_SIZE_BYTES} 字节；输出报告 {RUMBLE_REPORT_BYTES} 字节（均无 Report ID） */\n"
    "#pragma once\n#include <stdint.h>\n\n"
    f"static const uint8_t kDescVader2Pro[{len(D)}] = {{\n"
    + "".join("    " + ", ".join(f"0x{x:02x}" for x in D[i:i+12]) + ",\n" for i in range(0, len(D), 12))
    + "};\n\n"
    f"#ifndef VADER2PRO_REPORT_BYTES\n#define VADER2PRO_REPORT_BYTES {REPORT_SIZE_BYTES}\n#endif\n"
    f"#define VADER2PRO_RUMBLE_REPORT_BYTES {RUMBLE_REPORT_BYTES}\n",
)

print(f"描述符 {len(D)} 字节 / 输入报表 {REPORT_SIZE_BYTES} 字节 / 输出报表 {RUMBLE_REPORT_BYTES} 字节")
print(" ".join(f"{x:02x}" for x in D))
print("写出:", out.name, hdr.name)
