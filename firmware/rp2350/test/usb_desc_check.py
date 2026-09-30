#!/usr/bin/env python3
"""usb_desc_check.py — 从编译产物里抠出 USB 描述符并逐字段校验

为什么需要它：设备枚举失败在板子上是"看不见的"（没有串口就没有任何反馈）。
这个脚本把描述符链拿到主机侧做结构校验，能在刷机前抓到：
  - 配置总长与实走长度不一致（少写/多写字节）
  - IAD 重复 / 缺失（TUD_CDC_DESCRIPTOR 自带 IAD，手工再写一个就会踩）
  - 端点地址、类型、包长非法
  - HID 报告描述符长度与预期不符
  - 身份三元组（VID/PID/bcdDevice）不是冒充目标

用法：
    python3 usb_desc_check.py [--generic | --generic-apple | --apple-battery-interface | --scuf-apple | --scuf-known-id | --pid-probe] build/flydigi_bridge.elf
"""
from pathlib import Path
import re
import sys
import struct

PID_PROBE = "--pid-probe" in sys.argv[1:]
STADIA_PROBE = "--stadia-haptic" in sys.argv[1:]
APPLE_BATTERY = "--apple-battery" in sys.argv[1:]
APPLE_BATTERY_INTERFACE = "--apple-battery-interface" in sys.argv[1:]
GENERIC_APPLE = "--generic-apple" in sys.argv[1:]
SCUF_APPLE = "--scuf-apple" in sys.argv[1:]
SCUF_KNOWN_ID = "--scuf-known-id" in sys.argv[1:]
GENERIC = "--generic" in sys.argv[1:] or GENERIC_APPLE or APPLE_BATTERY or APPLE_BATTERY_INTERFACE or SCUF_APPLE or SCUF_KNOWN_ID or PID_PROBE or STADIA_PROBE
ARGS = [arg for arg in sys.argv[1:] if arg not in ("--generic", "--generic-apple", "--apple-battery", "--apple-battery-interface", "--scuf-apple", "--scuf-known-id", "--pid-probe", "--stadia-haptic")]
EXPECT_VID, EXPECT_PID, EXPECT_BCD = ((0x1B1C, 0x3A28, 0x0100) if SCUF_APPLE else
                                      (0x18D1, 0x9400, 0x0100) if STADIA_PROBE else
                                      (0x1209, 0x0001, 0x0101) if PID_PROBE else
                                      (0x1209, 0x0001, 0x0100) if GENERIC and not (GENERIC_APPLE or SCUF_KNOWN_ID) else
                                      (0x04B4, 0x2412, 0x0500))
SCUF_DESC = bytes.fromhex(
    "05010905a10105010930093109320935150026ff00750895048102"
    "050209c409c5150026ff00750895028102"
    "05010939150025073500463b0165147504950181426500750495018103"
    "05091901291d150025017501951d8102750195038103"
    "0600ff0901150026ff00750895089102c0"
)
EXPECT_HID_REPORT_LEN = 1299 if PID_PROBE else 101 if STADIA_PROBE else len(SCUF_DESC) if SCUF_APPLE or SCUF_KNOWN_ID else 120 if APPLE_BATTERY else 97
EXPECT_SENSOR_REPORT_LEN = 96
EXPECT_CONFIG_REPORT_LEN = 37
EXPECT_REPORT_BYTES = 18 if PID_PROBE else 12 if STADIA_PROBE else 11 if GENERIC else 10
EXPECT_RUMBLE_REPORT_BYTES = 5 if STADIA_PROBE else 8
BATTERY_DESC = bytes.fromhex("05840910a1000912a10005850965850615002564750895018102c0c0")

TYPE_DEVICE = 1
TYPE_CONFIG = 2
TYPE_STRING = 3
TYPE_INTERFACE = 4
TYPE_ENDPOINT = 5
TYPE_IAD = 11
TYPE_HID = 0x21
TYPE_CS_INTERFACE = 0x24


def find_device_descriptor(blob, off=0):
    while True:
        i = blob.find(b"\x12\x01", off)
        if i < 0:
            return None
        if i + 18 <= len(blob) and blob[i + 2] == 0x00 and blob[i + 3] == 0x02:
            return i
        off = i + 1


def walk_config(blob, start):
    """返回 (total_len, [条目], 问题列表)"""
    total = struct.unpack_from("<H", blob, start + 2)[0]
    itf_count = blob[start + 4]
    pos = start
    seen = []
    problems = []
    end = start + total
    while pos < end and blob[pos]:
        blen, btype = blob[pos], blob[pos + 1]
        if blen == 0:
            problems.append(f"偏移 {pos}: bLength=0（描述符链断裂）")
            break
        seen.append((pos - start, blen, btype))
        pos += blen
    if pos != end:
        problems.append(f"配置总长声明 {total}，实际走完 {pos - start} 字节")
    itfs = [s for s in seen if s[2] == TYPE_INTERFACE]
    iads = [s for s in seen if s[2] == TYPE_IAD]
    eps = [s for s in seen if s[2] == TYPE_ENDPOINT]
    hids = [s for s in seen if s[2] == TYPE_HID]
    if len(itfs) != itf_count:
        problems.append(f"bNumInterfaces={itf_count}，实际接口描述符 {len(itfs)} 个")
    if len(iads) != 1:
        problems.append(f"IAD 数量 = {len(iads)}（应为 1，重复/缺失都会让 macOS 不认）")
    # 端点合法性
    for off, blen, _ in eps:
        addr = blob[start + off + 2]
        attr = blob[start + off + 3]
        mps = struct.unpack_from("<H", blob, start + off + 4)[0]
        if addr == 0:
            problems.append(f"端点地址 0x00 非法（偏移 {off}）")
        if not (1 <= attr <= 5):
            problems.append(f"端点 0x{addr:02x} 传输类型 {attr} 非法")
        if not (1 <= mps <= 1024):
            problems.append(f"端点 0x{addr:02x} 包长 {mps} 非法")
    ep_addrs = [blob[start + off + 2] for off, _, _ in eps]
    if 0x01 not in ep_addrs or 0x81 not in ep_addrs:
        problems.append(f"HID 应同时声明 OUT 0x01 和 IN 0x81，实际端点 = {[f'0x{x:02x}' for x in ep_addrs]}")
    if 0x84 not in ep_addrs:
        problems.append(f"Sensors HID 应声明 IN 0x84，实际端点 = {[f'0x{x:02x}' for x in ep_addrs]}")
    if 0x85 not in ep_addrs:
        problems.append(f"WebHID config 应声明 IN 0x85，实际端点 = {[f'0x{x:02x}' for x in ep_addrs]}")
    if APPLE_BATTERY_INTERFACE and 0x86 not in ep_addrs:
        problems.append(f"Battery HID 应声明 IN 0x86，实际端点 = {[f'0x{x:02x}' for x in ep_addrs]}")
    return total, seen, problems, (len(itfs), len(iads), len(eps), hids)


def main():
    path = ARGS[0] if ARGS else "build/flydigi_bridge.elf"
    blob = open(path, "rb").read()
    ok = True

    # ---- 设备描述符 ----
    dev = find_device_descriptor(blob)
    if dev is None:
        print("✘ 未找到合法设备描述符")
        return 1
    vid, pid, bcd = struct.unpack_from("<HHH", blob, dev + 8)
    mps0 = blob[dev + 7]
    print(f"设备描述符 @0x{dev:x}: VID={vid:04x} PID={pid:04x} bcdDevice={bcd:04x} bMaxPacketSize0={mps0}")
    if (vid, pid, bcd) != (EXPECT_VID, EXPECT_PID, EXPECT_BCD):
        print(f"✘ 身份三元组不符（应为 {EXPECT_VID:04x}/{EXPECT_PID:04x}/{EXPECT_BCD:04x}）")
        ok = False
    if mps0 != 64:
        print(f"✘ bMaxPacketSize0 应为 64，实际 {mps0}")
        ok = False

    # ---- 配置描述符 ----
    # 逐个候选试走：能被完整、自洽走完的那个才是真配置描述符
    i, total, seen, problems, counts = None, 0, [], [], (0, 0, 0, [])
    off = 0
    while True:
        off = blob.find(b"\x09\x02", off)
        if off < 0:
            break
        t, s, p, c = walk_config(blob, off)
        if not p and c[0] == blob[off + 4] and c[0] > 0 and c[3]:
            i, total, seen, problems, counts = off, t, s, p, c
            break
        off += 1
    if i is None:
        print("✘ 未找到自洽的配置描述符")
        return 1
    n_itf, n_iad, n_ep, hids = counts
    print(f"配置描述符 @0x{i:x}: 总长 {total} B / 接口 {n_itf} / IAD {n_iad} / 端点 {n_ep} / HID描述符 {len(hids)}")
    for off, blen, btype in seen:
        print(f"   +{off:3d}  bLength={blen:2d}  type=0x{btype:02x}")
    if problems:
        ok = False
        for p in problems:
            print("✘ " + p)
    else:
        print("✔ 描述符链结构自洽")

    # ---- HID 报告描述符 ----
    if hids:
        hoff = i + hids[0][0]
        rlen = struct.unpack_from("<H", blob, hoff + 7)[0]
        print(f"HID 报告描述符长度字段 = {rlen}")
        if rlen != EXPECT_HID_REPORT_LEN:
            target = "Generic Game Pad" if GENERIC else "Vader2Pro"
            print(f"✘ 报告描述符长度应为 {EXPECT_HID_REPORT_LEN}（{target} 模型）")
            ok = False
        if PID_PROBE:
            header = (Path(__file__).resolve().parents[2] / "desc_pid_probe.h").read_text()
            array = re.search(r"kDescPidProbe\[\]\s*=\s*\{(.*?)\};", header, re.S)
            if array is None:
                raise ValueError("PID probe descriptor array missing")
            desc = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array.group(1)))
            sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
            from hid_parse import parse_report_descriptor
            elements, report_ids = parse_report_descriptor(desc)
            input_bits = sum(e["size"] for e in elements if e["kind"] == "input" and e["report_id"] == 1)
            if input_bits != 17 * 8 or not {1, 2, 5, 6, 7}.issubset(report_ids):
                print(f"✘ PID 报告结构不符（input ID1={input_bits} bits, IDs={sorted(report_ids)}）")
                ok = False
        elif STADIA_PROBE:
            header = (Path(__file__).resolve().parents[2] / "desc_stadia_haptic_probe.h").read_text()
            array = re.search(r"kDescStadiaHapticProbe\[\d+\]\s*=\s*\{(.*?)\};", header, re.S)
            if array is None:
                raise ValueError("Stadia probe descriptor array missing")
            desc = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array.group(1)))
            sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
            from hid_parse import parse_report_descriptor
            elements, report_ids = parse_report_descriptor(desc)
            input_bits = sum(e["size"] for e in elements if e["kind"] == "input" and e["report_id"] == 1)
            output_bits = sum(e["size"] for e in elements if e["kind"] == "output" and e["report_id"] == 5)
            if input_bits != 11 * 8 or output_bits != 4 * 8 or not {1, 5}.issubset(report_ids):
                print(f"✘ Stadia 报告结构不符（input ID1={input_bits}, output ID5={output_bits} bits）")
                ok = False
        elif SCUF_APPLE or SCUF_KNOWN_ID:
            desc = SCUF_DESC
        else:
            top = ("05010905a1010501093009310932093309340935150026ff00" if GENERIC else
                   "05010904a1010501093009310932093309340935150026ff00")
            button_tail = ("05091901291a150025017501951a8102750195068103" if GENERIC else
                           "05091901291415002501750195148102750195048103")
            desc = bytes.fromhex(
                top +
                "7508950681020939150025073500463b0165147504950181426500750495018103" +
                button_tail +
                "0600ff0901150026ff00750895089102c0")
            if APPLE_BATTERY:
                desc += bytes.fromhex(
                    "05840901a10185030584092015002564750895018102c0")
        expected_usage = 0x04 if PID_PROBE else 0x05 if GENERIC else 0x04
        if desc[3] != expected_usage:
            print(f"✘ 顶层 Usage 应为 0x{expected_usage:02x}，实际 0x{desc[3]:02x}")
            ok = False
        if len(desc) != EXPECT_HID_REPORT_LEN or desc not in blob:
            target = "PID probe" if PID_PROBE else "Stadia haptics probe" if STADIA_PROBE else "SCUF Omega" if SCUF_APPLE or SCUF_KNOWN_ID else "Generic Game Pad" if GENERIC else "Vader2Pro"
            print(f"✘ 二进制里没找到与 {target} 一致、含震动 Output 的报告描述符字节串")
            ok = False
        else:
            target = "PID probe" if PID_PROBE else "Stadia haptics probe" if STADIA_PROBE else "SCUF Omega" if SCUF_APPLE or SCUF_KNOWN_ID else "Generic Game Pad" if GENERIC else "Vader2Pro"
            output_label = "多种 PID Output/Feature" if PID_PROBE else f"Output {EXPECT_RUMBLE_REPORT_BYTES} B"
            print(f"✔ 报告描述符字节与 {target} 目标一致（{EXPECT_HID_REPORT_LEN} B，输入 {EXPECT_REPORT_BYTES} B，{output_label}）")
        if b"\x91\x02" not in blob:
            print("✘ 未找到 HID Output 主项（0x91 0x02）")
            ok = False

        expected_hids = 4 if APPLE_BATTERY_INTERFACE else 3
        if len(hids) != expected_hids:
            label = "手柄 + 传感器 + WebHID config + Battery" if APPLE_BATTERY_INTERFACE else "手柄 + 传感器 + WebHID config"
            print(f"✘ HID 接口数量应为 {expected_hids}（{label}），实际 {len(hids)}")
            ok = False
        else:
            sensor_len = struct.unpack_from("<H", blob, i + hids[1][0] + 7)[0]
            print(f"HID 传感器报告描述符长度字段 = {sensor_len}")
            if sensor_len != EXPECT_SENSOR_REPORT_LEN:
                print(f"✘ 传感器报告描述符长度应为 {EXPECT_SENSOR_REPORT_LEN}")
                ok = False
            sensor_desc = bytes.fromhex(
                "05200976a1008501052016008026ff7f751095010a5704651555008102"
                "0a58046515550081020a5904651555008102c005200973a10085020520"
                "16008026ff7f751095010a5304651a550481020a5404651a55048102"
                "0a5504651a55048102c0"
            )
            if len(sensor_desc) != EXPECT_SENSOR_REPORT_LEN or sensor_desc not in blob:
                print("✘ 二进制里没找到标准 Sensors 页 gyro/accel 报告描述符")
                ok = False
            else:
                print(f"✔ 传感器描述符字节存在（{EXPECT_SENSOR_REPORT_LEN} B，gyro ID=1，accel ID=2）")
            config_len = struct.unpack_from("<H", blob, i + hids[2][0] + 7)[0]
            print(f"HID WebHID config 报告描述符长度字段 = {config_len}")
            if config_len != EXPECT_CONFIG_REPORT_LEN:
                print(f"✘ WebHID config 报告描述符长度应为 {EXPECT_CONFIG_REPORT_LEN}")
                ok = False
            config_desc = bytes([
                0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
                0x85, 0x10, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x3f, 0xb1, 0x02,
                0x85, 0x11, 0x75, 0x08, 0x95, 0x3f, 0xb1, 0x02,
                0x85, 0x12, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x02, 0xc0,
            ])
            if len(config_desc) != EXPECT_CONFIG_REPORT_LEN or config_desc not in blob:
                print("✘ 二进制里没找到 WebHID Feature/Status 报告描述符")
                ok = False
            else:
                print(f"✔ WebHID 配置描述符字节存在（{EXPECT_CONFIG_REPORT_LEN} B，Feature IDs 0x10/0x11）")
            if APPLE_BATTERY_INTERFACE:
                battery_hid = hids[3]
                battery_len = struct.unpack_from("<H", blob, i + battery_hid[0] + 7)[0]
                print(f"HID Battery 报告描述符长度字段 = {battery_len}")
                if battery_len != len(BATTERY_DESC) or BATTERY_DESC not in blob:
                    print("✘ 二进制里没找到独立 Battery Strength 报告描述符")
                    ok = False
                else:
                    print(f"✔ 独立 Battery System/Absolute State Of Charge 描述符字节存在（{len(BATTERY_DESC)} B）")

    print(("PASS: 描述符检查全部通过" if ok else "FAIL: 描述符检查不通过"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
