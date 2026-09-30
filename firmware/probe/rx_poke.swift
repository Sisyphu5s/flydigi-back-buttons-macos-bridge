// rx_poke.swift — 直连飞智接收器（不经过小板）：dump 接口/描述符 → 发握手与 test-mode 命令 → 监听输入帧
// 用法: ./rx_poke [监听秒数]
// 依据 docs/02 §3：接口 1 扩展输入(含 M1–M4)需先开 test mode；命令魔数 5a a5
import Foundation
import IOKit.hid

let listenSec = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 20 : 20
func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined(separator: " ") }
func frame(_ b: [UInt8]) -> [UInt8] { var a = b; while a.count < 32 { a.append(0) }; return Array(a.prefix(32)) }

let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, nil)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
guard let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> else { print("枚举失败"); exit(1) }

var targets: [IOHIDDevice] = []
print("=== 候选设备（VID 0x37D7 / 厂商页 0xFFA0 / 0xFFEE）===")
for d in devs {
    let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
    let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
    let name = (IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String) ?? "-"
    let up = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
    let u = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
    guard vid == 0x37D7 || up == 0xFFA0 || up == 0xFFEE || vid == 0x04B4 else { continue }
    targets.append(d)
    print(String(format: "\n● vid=0x%04x pid=0x%04x primary=%d/%d  %@", vid, pid, up, u, name))
    if let rd = IOHIDDeviceGetProperty(d, kIOHIDReportDescriptorKey as CFString) as? Data {
        print("  ReportDescriptor (\(rd.count) B): \(hex(rd))")
    }
    if let els = IOHIDDeviceCopyMatchingElements(d, nil, 0) as? [IOHIDElement] {
        var outIds = Set<Int>(), inIds = Set<Int>(), featIds = Set<Int>()
        var n = 0
        for e in els {
            let t = IOHIDElementGetType(e)
            let rid = Int(IOHIDElementGetReportID(e))
            switch t {
            case kIOHIDElementTypeOutput: outIds.insert(rid)
            case kIOHIDElementTypeFeature: featIds.insert(rid)
            case kIOHIDElementTypeInput_Misc, kIOHIDElementTypeInput_Button,
                 kIOHIDElementTypeInput_Axis, kIOHIDElementTypeInput_ScanCodes: inIds.insert(rid)
            default: break
            }
            if t != kIOHIDElementTypeCollection { n += 1 }
            if n <= 12 {
                print(String(format: "    type=%d page=0x%02x usage=0x%02x rid=%d size=%d cnt=%d",
                             Int(t.rawValue), IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e), rid,
                             IOHIDElementGetReportSize(e), IOHIDElementGetReportCount(e)))
            }
        }
        print("  元素 \(n) 个；report id: in=\(inIds.sorted()) out=\(outIds.sorted()) feature=\(featIds.sorted())")
    }
}
if targets.isEmpty { print("\n没有找到飞智接收器（VID 0x37D7 / 厂商页 0xFFA0）——请把接收器插到 Mac 上再跑。"); exit(2) }

// ---- 发命令 ----
let cmds: [(String, [UInt8])] = [
    ("device info    ", [0x5a, 0xa5, 0x01, 0x02, 0x03]),
    ("MAC/serial     ", [0x5a, 0xa5, 0xa1, 0x02, 0xa3]),
    ("config read    ", [0x5a, 0xa5, 0x02, 0x02, 0x04]),
    ("config data    ", [0x5a, 0xa5, 0x04, 0x02, 0x06]),
    ("TEST MODE ON   ", [0x5a, 0xa5, 0x11, 0x07, 0xff, 0x01, 0xff, 0xff, 0xff, 0x15, 0x00]),
]
print("\n=== 打开设备并发送握手 + test mode ===")
for d in targets {
    let r = IOHIDDeviceOpen(d, IOOptionBits(kIOHIDOptionsTypeNone))
    print("open(0x\(String(IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int ?? 0, radix: 16))) → \(r == kIOReturnSuccess ? "成功" : "失败(\(r))")")
    for (label, c) in cmds {
        for rid in [0, 5, 1, 2] {
            let data = frame(c)
            let res = data.withUnsafeBufferPointer { buf in
                IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, CFIndex(rid), buf.baseAddress!, data.count)
            }
            if res == kIOReturnSuccess { print("  ✔ \(label) sent (reportID=\(rid))"); break }
            if rid == 2 { print("  ✘ \(label) 发送失败 res=\(res)") }
        }
        usleep(120_000)
    }
}

// ---- 监听 ----
print("\n=== 监听输入 \(Int(listenSec)) 秒（请在此期间按键：左摇杆画圈 → M1~M4 → A）===")
var lastFrame = ""
for d in targets { IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone)) }
IOHIDManagerRegisterInputReportCallback(mgr, { _, _, _, _, reportID, report, len in
    let data = Data(bytes: report, count: len)
    let s = hex(data)
    if s != lastFrame { lastFrame = s; print(String(format: "  [rid=%d len=%d] %@", reportID, len, s)) }
}, nil)
IOHIDManagerRegisterInputValueCallback(mgr, { _, _, _, value in
    let e = IOHIDValueGetElement(value)
    print(String(format: "  value: page=0x%02x usage=0x%02x = %ld", IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e), IOHIDValueGetIntegerValue(value)))
}, nil)
IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
RunLoop.current.run(until: Date().addingTimeInterval(listenSec))
print("监听结束。若完全无输出：多为「输入监控」权限未授予（系统设置 → 隐私与安全性 → 输入监控，给运行它的终端/终端 App 打勾后重跑）。")
