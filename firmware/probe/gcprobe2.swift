// gcprobe2.swift — GameController 异步发现 + HID 报告描述符 dump
// 运行：swift gcprobe2.swift
import Foundation
import GameController
import IOKit.hid

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

func dumpControllers(_ tag: String) {
    let cs = GCController.controllers()
    print("\n[\(tag)] GCController.controllers() 数量 = \(cs.count)")
    for c in cs {
        print("  vendorName=\(c.vendorName ?? "-") category=\(c.productCategory) type=\(type(of: c.physicalInputProfile))")
        let xb = c.extendedGamepad as? GCXboxGamepad
        print("  GCXboxGamepad=\(xb != nil)")
        if let xb = xb {
            for (n, b) in [("paddleButton1", xb.paddleButton1), ("paddleButton2", xb.paddleButton2),
                           ("paddleButton3", xb.paddleButton3), ("paddleButton4", xb.paddleButton4)] {
                print("    \(n)=\(b == nil ? "nil" : "name=\(b!.localizedName ?? "-")")")
            }
        }
        for k in c.physicalInputProfile.elements.keys.sorted() {
            print("    element: \(k)")
        }
    }
}

NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: nil) { n in
    print("  ★ 收到 GCControllerDidConnect: \((n.object as? GCController)?.vendorName ?? "?")")
}
NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: nil) { n in
    print("  ★ 收到 GCControllerDidDisconnect")
}
GCController.shouldMonitorBackgroundEvents = true   // 让 CLI 也参与发现

dumpControllers("t=0")
print("\n等待异步发现 5 秒（run loop）…")
RunLoop.current.run(until: Date().addingTimeInterval(5))
dumpControllers("t=5s")

print("\n=== HID 报告描述符比对 ===")
let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, nil)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
if let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> {
    for d in devs {
        let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
        let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
        let isGenericBridge = vid == 0x1209 && pid == 0x0001
        guard vid == 1118 || vid == 14295 || vid == 0x04B4 || isGenericBridge else { continue }
        let name = (IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String) ?? "-"
        let up = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
        let u = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
        let loc = (IOHIDDeviceGetProperty(d, kIOHIDLocationIDKey as CFString) as? Int) ?? -1
        print(String(format: "\n设备 %@ vid=0x%04x pid=0x%04x primary=%d/%d location=0x%x", name, vid, pid, up, u, loc))
        if let rd = IOHIDDeviceGetProperty(d, kIOHIDReportDescriptorKey as CFString) as? Data {
            print("  ReportDescriptor (\(rd.count) 字节): \(hex(rd))")
        } else {
            print("  ReportDescriptor: 不可读")
        }
        // 可用的输入元素（确认它到底报哪些 usage）
        if let el = IOHIDDeviceCopyMatchingElements(d, nil, 0) as? [IOHIDElement] {
            var seen = Set<String>()
            for e in el {
                let t = IOHIDElementGetType(e)
                guard t == kIOHIDElementTypeInput_Button || t == kIOHIDElementTypeInput_Axis ||
                      t == kIOHIDElementTypeInput_Misc else { continue }
                let s = String(format: "page=%d usage=%d", IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e))
                if !seen.contains(s) { seen.insert(s) }
            }
            print("  输入元素（去重）\(seen.count) 个: \(seen.sorted().prefix(40).joined(separator: " "))")
        }
    }
}
