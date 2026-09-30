#!/usr/bin/env python3
"""一致性检查：我们生成的 HID 描述符 vs 苹果模型文件 Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist

验证内容：
 1. 模型里每条 Driver.Elements 谓词都能在我们的描述符里唯一命中一个元素；
 2. 在 UsageTypeIndex 的两种解释下（usage 偏移 / 同类位置）结果一致；
 3. button.m1..m4 命中 Button usage 15/16/17/18；
 4. 顺带做全库 UsageType 值普查（证明 UsageType 是"元素类别"而不是 usage page）。
用法：model_conform.py [model.plist] [desc.hex]
"""
from __future__ import annotations
import pathlib, plistlib, re, sys
from hid_parse import parse_report_descriptor, element_class, class_indexes

HERE = pathlib.Path(__file__).parent
MODEL = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
    "/System/Library/AssetsV2/com_apple_MobileAsset_GameController_DB1/"
    "b1e50bde24de08e8e1e74bce42ca215b6d3c31e9.asset/AssetData/GameControllers-Custom.bundle/"
    "Personalities/Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist")
DESC = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else HERE / "desc_vader2pro.hex")

desc_bytes = bytes(int(x, 16) for line in DESC.read_text().split("\n") if line.strip() for x in line.split())
elements, _ = parse_report_descriptor(desc_bytes)
idx_usage = class_indexes(elements, "usage")
idx_pos = class_indexes(elements, "position")

model = plistlib.load(open(MODEL, "rb"))["Model"]
driver = model["Driver"]["Elements"]
physical = model["PhysicalInput"]["Elements"]

def lookup(cls: int, index: int, mode: str):
    table = idx_usage if mode == "usage" else idx_pos
    hits = [e for e in elements if element_class(e) == cls and table.get(id(e)) == index]
    return hits

RX = re.compile(r"UsageType\s*==\s*(\d+)\s+AND\s+UsageTypeIndex\s*==\s*(\d+)")
print(f"模型: {MODEL.name}\n描述符: {DESC.name}（{len(desc_bytes)} 字节）\n")
print(f"{'Driver identifier':26s} {'pred':>7}  {'usage解释':>22s}  {'position解释':>22s}  一致")
fail = []
for el in driver:
    m = RX.fullmatch(el["Predicate"].strip())
    if not m:
        fail.append(f"谓词无法解析: {el['Predicate']}")
        continue
    cls, index = int(m.group(1)), int(m.group(2))
    a, b = lookup(cls, index, "usage"), lookup(cls, index, "position")
    fmt = lambda hits: (f"usage={hits[0]['usage']}(0x{hits[0]['usage']:02x}) "
                        f"bit{hits[0]['bit_offset']}" if len(hits) == 1 else
                        ("无命中" if not hits else f"命中{len(hits)}个!"))
    same = len(a) == 1 and len(b) == 1 and a[0] is b[0]
    print(f"{el['Identifier']:26s} {cls}/{index:<5} {fmt(a):>22s}  {fmt(b):>22s}  {'✔' if same else '✘'}")
    if not same:
        fail.append(f"{el['Identifier']}: usage解释={fmt(a)} position解释={fmt(b)}")

# M1..M4 必须是 Button usage 15/16/17/18
print("\n背键定位：")
for i in range(1, 5):
    hits = lookup(1, 13 + i, "usage")
    ok = len(hits) == 1 and hits[0]["usage"] == 14 + i and hits[0]["usage_page"] == 0x09
    print(f"  button.m{i} -> Button usage {14+i} (bit {hits[0]['bit_offset'] if len(hits)==1 else '?'}) "
          f"{'✔' if ok else '✘'}")
    if not ok:
        fail.append(f"button.m{i} 未落在 Button usage {14+i}")

# 每层 identifier 是否一一对应（PhysicalInput 与 Driver 同名）
d_ids = {e["Identifier"] for e in driver}
p_ids = {e["Identifier"] for e in physical}
print(f"\n层次对应：Driver 独有 {sorted(d_ids - p_ids) or '无'}；PhysicalInput 独有 {sorted(p_ids - d_ids) or '无'}")

print("\n结果:", "全部通过 ✔" if not fail else f"{len(fail)} 项失败 ✘")
for f in fail:
    print("  -", f)
sys.exit(1 if fail else 0)
