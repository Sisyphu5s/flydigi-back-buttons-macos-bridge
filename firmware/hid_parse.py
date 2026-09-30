#!/usr/bin/env python3
"""独立 HID 报告描述符解析器 + 报文打包/解包（不依赖生成脚本的任何假设）。

用途：把 desc_vader2pro.hex 当成"陌生字节流"从头解析，得到元素表（usage/位偏移/
位宽/logical 范围），再据此做位级打包与解包 —— 用于验证描述符自洽，并与苹果模型
文件做一致性检查（model_conform.py）。
"""
from __future__ import annotations

# ---- 解析 ----
_TYPE_MAIN, _TYPE_GLOBAL, _TYPE_LOCAL = 0, 1, 2
AXIS_USAGES = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38}
HAT_USAGE = 0x39


def parse_report_descriptor(data: bytes):
    """-> (elements, report_ids): elements 按位序排列。"""
    g = {"page": 0, "logmin": 0, "logmax": 0, "physmin": 0, "physmax": 0,
         "unit": 0, "rsize": 0, "rcount": 0, "rid": 0}
    stack, usages, umin, umax = [], [], None, None
    elements, bitpos, report_ids = [], {}, set()
    i = 0
    while i < len(data):
        pre = data[i]
        tag, typ, size = pre & 0xF0, (pre >> 2) & 0x03, pre & 0x03
        i += 1
        nbytes = 3 if size == 3 else size          # size==3 编码为 4 字节
        raw = data[i:i + nbytes]
        if len(raw) != nbytes:
            raise ValueError("描述符被截断")
        i += nbytes
        val = int.from_bytes(raw, "little") if nbytes else 0
        sval = int.from_bytes(raw, "little", signed=True) if nbytes else 0

        if typ == _TYPE_GLOBAL:
            if tag == 0x00:                            # Usage Page
                g["page"] = val
            elif tag == 0x10:
                g["logmin"] = sval
            elif tag == 0x20:
                g["logmax"] = sval
            elif tag == 0x30:
                g["physmin"] = sval
            elif tag == 0x40:
                g["physmax"] = sval
            elif tag == 0x50:                          # Unit
                g["unit"] = val
            elif tag == 0x70:                          # Report Size
                g["rsize"] = val
            elif tag == 0x80:                          # Report ID
                g["rid"] = val
                report_ids.add(val)
            elif tag == 0x90:                          # Report Count
                g["rcount"] = val
            elif tag == 0xA0:                          # Push
                stack.append(dict(g))
            elif tag == 0xB0:                          # Pop
                g = stack.pop()
        elif typ == _TYPE_LOCAL:
            if tag == 0x00:                            # Usage
                usages.append(val)
            elif tag == 0x10:
                umin = val
            elif tag == 0x20:
                umax = val
        else:                                          # Main
            if tag in (0x80, 0x90, 0xB0):              # Input / Output / Feature
                kind = {0x80: "input", 0x90: "output", 0xB0: "feature"}[tag]
                pos = bitpos.get((kind, g["rid"]), 0)
                flags = {"data": not bool(val & 0x01), "const": bool(val & 0x01),
                         "var": bool(val & 0x04), "rel": bool(val & 0x08),
                         "abs": not bool(val & 0x08), "null": bool(val & 0x40)}
                for k in range(g["rcount"]):
                    if k < len(usages):
                        u = usages[k]
                    elif umin is not None and umax is not None and umin + k <= umax:
                        u = umin + k
                    else:
                        u = None
                    elements.append({
                        "usage_page": g["page"], "usage": u, "bit_offset": pos,
                        "size": g["rsize"], "logical_min": g["logmin"],
                        "logical_max": g["logmax"], "report_id": g["rid"],
                        "kind": kind,
                        **flags,
                    })
                    pos += g["rsize"]
                bitpos[(kind, g["rid"])] = pos
                usages, umin, umax = [], None, None
            elif tag == 0xA0:                          # Collection
                usages, umin, umax = [], None, None
            elif tag == 0xC0:                          # End Collection
                usages, umin, umax = [], None, None
    return elements, report_ids


def element_class(el) -> int | None:
    """苹果模型里的 UsageType：1=按钮, 2=模拟量/轴, 3=hat/dpad（见 docs/11 §4）。"""
    if el["usage"] is None or el["const"]:
        return None
    if el["usage_page"] == 0x09:
        return 1
    if el["usage_page"] == 0x01 and el["usage"] in AXIS_USAGES:
        return 2
    if el["usage_page"] == 0x01 and el["usage"] == HAT_USAGE:
        return 3
    return None


def class_index(el, mode: str) -> int:
    """mode='usage': usage − 类别基址（1=Button页基址1 / 2=轴基址0x30 / 3=hat基址0x39）；
    mode='position': 同类元素在描述符中的 0-based 序号 —— 见 class_indexes()。"""
    raise ValueError("请用 class_indexes()")


CLASS_BASE = {1: 0x01, 2: 0x30, 3: 0x39}   # 类别基址（一手推导见 report-verification.md）


def class_indexes(elements, mode: str) -> dict:
    """-> {id(el): index}"""
    out, counters = {}, {}
    for el in elements:
        c = element_class(el)
        if c is None:
            continue
        if mode == "usage":
            out[id(el)] = el["usage"] - CLASS_BASE[c]
        else:
            out[id(el)] = counters.get(c, 0)
            counters[c] = counters.get(c, 0) + 1
    return out


# ---- 位级打包 / 解包 ----
def pack(elements, values: dict) -> bytes:
    """values: {usage: value}（Button page 与 Generic Desktop page 用 usage 区分，
    这里用 (page, usage) 键更安全）。"""
    nbits = max((e["bit_offset"] + e["size"] for e in elements
                 if e["report_id"] == 0 and e["kind"] == "input"), default=0)
    buf = bytearray((nbits + 7) // 8)
    for e in elements:
        if e["report_id"] != 0 or e["kind"] != "input" or e["const"] or e["usage"] is None:
            continue
        v = values.get((e["usage_page"], e["usage"]))
        if v is None:
            continue
        raw = int(v) & ((1 << e["size"]) - 1)
        for bit in range(e["size"]):
            if raw >> bit & 1:
                pos = e["bit_offset"] + bit
                buf[pos >> 3] |= 1 << (pos & 7)
    return bytes(buf)


def unpack(elements, report: bytes) -> dict:
    out = {}
    for e in elements:
        if e["kind"] != "input" or e["const"] or e["usage"] is None:
            continue
        v = 0
        for bit in range(e["size"]):
            pos = e["bit_offset"] + bit
            if pos >> 3 < len(report) and report[pos >> 3] >> (pos & 7) & 1:
                v |= 1 << bit
        out[(e["usage_page"], e["usage"])] = v
    return out


if __name__ == "__main__":
    import pathlib, sys
    p = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                     pathlib.Path(__file__).with_name("desc_vader2pro.hex"))
    data = bytes(int(x, 16) for line in p.read_text().split("\n") if line.strip()
                 for x in line.split())
    els, rids = parse_report_descriptor(data)
    print(f"描述符 {len(data)} 字节；Report ID: {sorted(rids) or '无'}")
    print(f"{'page':>4} {'usage':>5} {'class':>5} {'bitoff':>6} {'size':>4} {'logical':>9} flags")
    for e in els:
        print(f"0x{e['usage_page']:02x} {str(e['usage']):>5} {str(element_class(e)):>5} "
              f"{e['bit_offset']:>6} {e['size']:>4} {e['logical_min']:>3}..{e['logical_max']:<4} "
              f"{'data' if e['data'] else 'const'}{'/var' if e['var'] else ''}"
              f"{'/null' if e['null'] else ''}")
    for kind in ("input", "output", "feature"):
        bits = [e["bit_offset"] + e["size"] for e in els if e["kind"] == kind]
        if bits:
            print(f"{kind} 报表字节数:", (max(bits) + 7) // 8)
