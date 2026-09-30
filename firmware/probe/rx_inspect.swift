// rx_inspect.swift — 检查飞智接收器（或任意 USB HID 设备）：接口结构 + HID 报告描述符 + 元素表
// 用途：把接收器直接插到 Mac 上时，不依赖板子固件，独立看清楚它往哪条通道报数据。
// 运行：swift rx_inspect.swift            # 全量列出（含飞智接收器 VID 0x37D7）
//       swift rx_inspect.swift 14295      # 只看指定 VID
import Foundation
import IOKit.hid

let filterVid = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) : nil

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

print("=== IOHID 设备（vid/pid/usage/传输/描述符 长度）===")
let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, nil)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
guard let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> else { print("无法枚举"); exit(1) }

struct Info { let d: IOHIDDevice; let vid: Int; let pid: Int; let name: String; let up: Int; let u: Int }
var list: [Info] = []
for d in devs {
    let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
    let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
    let name = (IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String) ?? "-"
    let up = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
    let u = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
    list.append(Info(d: d, vid: vid, pid: pid, name: name, up: up, u: u))
}

// 只打印"可能有用的"：指定 VID、或非内置（transport 非 SPU/Audio）、或手柄/键盘/厂商页用法
for i in list.sorted(by: { ($0.vid, $0.pid, $0.up, $0.u) < ($1.vid, $1.pid, $1.up, $1.u) }) {
    let transport = (IOHIDDeviceGetProperty(i.d, kIOHIDTransportKey as CFString) as? String) ?? "-"
    let primary = i.up == 1 && [4, 5, 6, 7, 8].contains(i.u)
    let vendorPage = i.up >= 0xFF00
    if let f = filterVid, i.vid != f { continue }
    guard filterVid != nil || primary || vendorPage || i.vid == 14295 || i.vid == 0x37D7 || i.vid == 0x04B4 else { continue }
    let rd = IOHIDDeviceGetProperty(i.d, kIOHIDReportDescriptorKey as CFString) as? Data
    print(String(format: "\nvid=0x%04x(%d) pid=0x%04x(%d) usage=%d/%d transport=%@", i.vid, i.vid, i.pid, i.pid, i.up, i.u, transport))
    print("  Product: \(i.name)")
    if let rd = rd { print("  ReportDescriptor (\(rd.count) B): \(hex(rd))") } else { print("  ReportDescriptor: 无") }
    if let els = IOHIDDeviceCopyMatchingElements(i.d, nil, 0) as? [IOHIDElement] {
        var rows: [String] = []
        for e in els {
            let t = IOHIDElementGetType(e)
            let tp: String
            switch t {
            case kIOHIDElementTypeInput_Button: tp = "Btn"
            case kIOHIDElementTypeInput_Axis:   tp = "Axs"
            case kIOHIDElementTypeInput_Misc:   tp = "Msc"
            case kIOHIDElementTypeInput_ScanCodes: tp = "Key"
            case kIOHIDElementTypeOutput:       tp = "Out"
            case kIOHIDElementTypeFeature:      tp = "Fea"
            default: tp = "?"
            }
            let up = IOHIDElementGetUsagePage(e), u = IOHIDElementGetUsage(e)
            let lo = IOHIDElementGetLogicalMin(e), hi = IOHIDElementGetLogicalMax(e)
            let rs = IOHIDElementGetReportSize(e), rid = IOHIDElementGetReportID(e)
            let cnt = IOHIDElementGetReportCount(e)
            rows.append(String(format: "    %@ page=0x%02x usage=0x%02x rid=%d size=%d cnt=%d range=%d..%d", tp, up, u, rid, rs, cnt, lo, hi))
        }
        print("  元素 \(rows.count) 个:"); print(rows.prefix(60).joined(separator: "\n"))
        if rows.count > 60 { print("    … 余 \(rows.count - 60) 个") }
    }
}

// 可选：监听输入（键盘类设备需要「输入监控」权限；无权限则无输出）
if filterVid != nil || CommandLine.arguments.contains("--watch") {
    print("\n=== 监听输入 8 秒（Ctrl-C 停止）===")
    let match: [String: Any] = [kIOHIDDeviceUsagePageKey: 1, kIOHIDDeviceUsageKey: 0]
    IOHIDManagerSetDeviceMatching(mgr, match as CFDictionary)
    IOHIDManagerRegisterInputValueCallback(mgr, { _, _, _, value in
        let e = IOHIDValueGetElement(value)
        let page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e)
        let v = IOHIDValueGetIntegerValue(value)
        print(String(format: "  page=0x%02x usage=0x%02x value=%ld", page, usage, v))
    }, nil)
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
    RunLoop.current.run(until: Date().addingTimeInterval(8))
    print("（无输出 ¥ 可能是没数据，也可能是缺「输入监控」权限——键盘类设备需要）")
}
