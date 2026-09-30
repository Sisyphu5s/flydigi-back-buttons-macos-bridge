#!/usr/bin/env python3
"""端到端 dry-run（纯主机，不需要硬件）：HID 报文 → 独立解析 → 套苹果模型 → GC 元素值。

验证三件事：
 1. 位级打包/解包自洽（往返一致）；
 2. 苹果模型里每条 Driver 谓词都能从报文里取到值（含 M1..M4）；
 3. PhysicalInput 层（LocalizedNameKey）能被我们填满 —— 即系统侧会看到的最终元素。
"""
from __future__ import annotations
import pathlib, plistlib, re, sys
from hid_parse import parse_report_descriptor, element_class, class_indexes, pack, unpack

HERE = pathlib.Path(__file__).parent
MODEL = pathlib.Path("/System/Library/AssetsV2/com_apple_MobileAsset_GameController_DB1/"
    "b1e50bde24de08e8e1e74bce42ca215b6d3c31e9.asset/AssetData/GameControllers-Custom.bundle/"
    "Personalities/Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist")
desc = bytes(int(x, 16) for line in (HERE / "desc_vader2pro.hex").read_text().split("\n")
             if line.strip() for x in line.split())
els, _ = parse_report_descriptor(desc)
idx_pos = class_indexes(els, "position")

model = plistlib.load(open(MODEL, "rb"))["Model"]
RX = re.compile(r"UsageType\s*==\s*(\d+)\s+AND\s+UsageTypeIndex\s*==\s*(\d+)")
drv = {}          # driver identifier -> descriptor element
for e in model["Driver"]["Elements"]:
    m = RX.fullmatch(e["Predicate"].strip())
    hits = [x for x in els if element_class(x) == int(m.group(1)) and idx_pos.get(id(x)) == int(m.group(2))]
    if len(hits) != 1:
        sys.exit(f"无法唯一定位 {e['Identifier']}（命中 {len(hits)}）")
    drv[e["Identifier"]] = hits[0]

# ---- 场景：模拟"飞智手柄正在被按下" ----
# 值按 (usage_page, usage) 给；按钮类 1=按下
BTN = lambda n: (0x09, n)
AX = lambda u: (0x01, u)
state = {
    BTN(15): 1, BTN(17): 1,              # M1, M3 按下
    BTN(1): 1, BTN(5): 1,                # A, Y 按下
    BTN(12): 1,                          # Start/Menu
    AX(0x30): 64, AX(0x31): 160,         # LX 左、LY 下
    AX(0x32): 128, AX(0x33): 128,        # 右摇杆居中
    AX(0x34): 255, AX(0x35): 0,          # RT 到底、LT 松开
    AX(0x39): 0,                          # hat = 上
}
report = pack(els, state)
print("报文:", " ".join(f"{b:02x}" for b in report), f"({len(report)} 字节)")

back = unpack(els, report)
# ---- 往返校验 ----
bad = [k for k, v in state.items() if back.get(k) != v]
print("位级往返:", "一致 ✔" if not bad else f"不一致 ✘ {bad}")

def hat_dirs(h):
    return {d: (h == v) for d, v in (("Up", 0), ("Right", 2), ("Down", 4), ("Left", 6))}

print(f"\n{'LocalizedNameKey':26s} {'物理来源':26s} {'GC 层值'}")
rows = []
for pe in model["PhysicalInput"]["Elements"]:
    ident, typ = pe["Identifier"], pe["Type"]
    lk = pe["LocalizedNameKey"]
    if ident == "dpad":
        d = hat_dirs(back[AX(0x39)])
        val = " ".join(k for k, v in d.items() if v) or "(中位)"
        src = f"hat=0x{back[AX(0x39)]:02x}"
    elif typ == "Dpad":                                # 摇杆（Driver 层是 .x/.y 两条）
        x, y = back[AX(drv[ident + ".x"]["usage"])], back[AX(drv[ident + ".y"]["usage"])]
        val = f"x={x/127.5-1:+.2f} y={y/127.5-1:+.2f}"
        src = f"用 {ident}.x/.y"
    else:
        e = drv[ident]
        raw = back[(e["usage_page"], e["usage"])]
        val = f"{raw/255:.2f}" if pe.get("Analog") else ("按下" if raw else "松开")
        src = f"usage=0x{e['usage']:02x} bit{e['bit_offset']}"
    rows.append((lk, src, val, ident))

for lk, src, val, ident in rows:
    flag = "  ← 背键" if re.fullmatch(r"BUTTON_M[1-9]", lk) else ""
    print(f"{lk:26s} {src:26s} {val}{flag}")

m_keys = [r for r in rows if re.fullmatch(r"BUTTON_M[1-9]", r[0])]
print(f"\n背键结论: 模型共声明 {len(m_keys)} 个背键元素 "
      f"({', '.join(r[0] for r in sorted(m_keys))})；"
      f"M1/M3 按下时 GC 层值 = {', '.join(r[2] for r in sorted(m_keys))}")
ok = all(r[2] == "按下" for r in sorted(m_keys) if r[0] in ("BUTTON_M1", "BUTTON_M3")) and \
     all(r[2] == "松开" for r in sorted(m_keys) if r[0] in ("BUTTON_M2", "BUTTON_M4"))
print("dry-run:", "通过 ✔" if (not bad and ok and len(m_keys) == 4) else "失败 ✘")
sys.exit(0 if (not bad and ok and len(m_keys) == 4) else 1)
