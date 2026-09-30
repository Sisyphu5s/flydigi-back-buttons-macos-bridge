// gc_watch.swift — 两条独立证据线
//   ① GameController 框架：连上通知 + controllers() 列表（含 BUTTON_M1..M4 是否可见）
//   ② IOKit 直查：HID 设备实际暴露的元素（usage 页/usage 号）+ 按钮数量
// 用法: swift gc_watch.swift [等待秒数]
import Foundation
import GameController
import IOKit.hid

let wait = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 6 : 6
let wantedVID = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2], radix: 16) ?? 0x1209 : 0x1209
let wantedPID = CommandLine.arguments.count > 3 ? Int(CommandLine.arguments[3], radix: 16) ?? 1 : 1

// ---------- ① GameController ----------
print("=== ① GCController ===")
if #available(macOS 11.0, *) {
    print("shouldMonitorBackgroundEvents(初始)=\(GCController.shouldMonitorBackgroundEvents)")
    GCController.shouldMonitorBackgroundEvents = true
}
var connected: [String] = []
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: nil) { n in
    if let c = n.object as? GCController {
        connected.append(c.vendorName ?? "(no vendor)")
        print("  [通知] GCControllerDidConnect: vendor=\(c.vendorName ?? "-") category=\(c.productCategory)")
    }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: nil) { _ in
    print("  [通知] GCControllerDidDisconnect")
}
_ = GCController.controllers()   // 触发一次枚举

print("controllers() 首次 = \(GCController.controllers().count)")
RunLoop.current.run(until: Date().addingTimeInterval(wait))
let cs = GCController.controllers()
print("controllers() 等待后 = \(cs.count)   期间收到连接通知 = \(connected.count)")
for c in cs {
    print("  vendor=\(c.vendorName ?? "-") category=\(c.productCategory)")
    let p = c.physicalInputProfile
    print("  elements=\(p.elements.count)")
    for k in p.elements.keys.sorted() { print("    - \(k)") }
    if let eg = c.extendedGamepad {
        let ops: [(String, GCControllerElement?)] = [("A", eg.buttonA), ("B", eg.buttonB), ("X", eg.buttonX),
            ("Y", eg.buttonY), ("L1", eg.leftShoulder), ("R1", eg.rightShoulder), ("L2", eg.leftTrigger),
            ("R2", eg.rightTrigger), ("menu", eg.buttonMenu), ("home", eg.buttonHome), ("options", eg.buttonOptions)]
        print("  extendedGamepad 非 nil: \(ops.filter { $0.1 != nil }.map { $0.0 }.joined(separator: ","))")
    }
}

// ---------- ② IOKit HID 直查 ----------
print("\n=== ② IOKit 直查 HID 元素 ===")
guard let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone)) as IOHIDManager? else {
    exit(1)
}
IOHIDManagerSetDeviceMatching(mgr, [kIOHIDVendorIDKey: wantedVID, kIOHIDProductIDKey: wantedPID] as CFDictionary)
IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
RunLoop.current.run(until: Date().addingTimeInterval(2.0))
let devs = (IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice>) ?? []
print(String(format: "匹配到 %d 个 HID 设备 (VID=%04x PID=%04x)", devs.count, wantedVID, wantedPID))
for d in devs {
    let props = IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString)
    let page = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
    let usage = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
    print("--- device: \(props ?? "-" as CFString) usage=\(String(format: "%04x:%04x", page, usage))")
    if #available(macOS 11.0, *) {
        print("    GCController.supportsHIDDevice = \(GCController.supportsHIDDevice(d))")
    }
    if let elems = IOHIDDeviceCopyMatchingElements(d, nil, IOOptionBits(kIOHIDOptionsTypeNone)) as? [IOHIDElement] {
        var buttons: [Int] = []
        var axes: [Int] = []
        var hats: [Int] = []
        for e in elems {
            let page = Int(IOHIDElementGetUsagePage(e))
            let usage = Int(IOHIDElementGetUsage(e))
            let type = IOHIDElementGetType(e)
            if page == 0x09 { buttons.append(usage) }
            else if page == 0x01 {
                if usage == 0x39 { hats.append(usage) } else { axes.append(usage) }
            }
            let _ = type
        }
        print("    按钮 usage(0x09) = \(buttons.sorted())   共 \(buttons.count) 个")
        print("    轴 usage(0x01)   = \(axes.sorted())   hat = \(hats.count)")
    } else {
        print("    (拿不到元素表)")
    }
}
exit(0)
