// gc_probe.swift — 用 GameController 框架直接问系统：认到了哪些手柄、每个手柄暴露了哪些元素
// 运行：swift gc_probe.swift
import Foundation
import GameController
import IOKit.hid

print("=== GCController.controllers() ===")
let cs = GCController.controllers()
print("数量: \(cs.count)")
for (i, c) in cs.enumerated() {
    print("\n[\(i)] vendorName=\(c.vendorName ?? "-")  productCategory=\(c.productCategory)  attached=\(c.isAttachedToDevice)")
    print("    physicalInputProfile: \(type(of: c.physicalInputProfile))")

    if let xb = c.extendedGamepad as? GCXboxGamepad {
        print("    ★ GCXboxGamepad 生效（Xbox 类别）")
        print("      paddleButton1..4 是否存在: \(xb.paddleButton1 != nil) \(xb.paddleButton2 != nil) \(xb.paddleButton3 != nil) \(xb.paddleButton4 != nil)")
        for (n, b) in [("paddle1", xb.paddleButton1), ("paddle2", xb.paddleButton2),
                       ("paddle3", xb.paddleButton3), ("paddle4", xb.paddleButton4)] {
            if let b = b { print("      \(n).localizedName=\(b.localizedName ?? "-") value=\(b.value)") }
        }
        if #available(macOS 16.0, *) {
            if let s = xb.buttonShare { print("      buttonShare.localizedName=\(s.localizedName ?? "-") value=\(s.value)") }
        }
    } else if c.extendedGamepad != nil {
        print("    GCXboxGamepad 未生效（只是通用 extendedGamepad）")
    }

    // 全部元素（按 identifier）——这是"背键到底叫什么"的直接答案
    let els = c.physicalInputProfile.elements
    print("    元素数: \(els.count)")
    for k in els.keys.sorted() {
        let e = els[k]!
        var v = ""
        if let b = e as? GCControllerButtonInput { v = "value=\(String(format: "%.2f", b.value)) pressed=\(b.isPressed)" }
        else if let d = e as? GCControllerDirectionPad { v = "x=\(String(format: "%.2f", d.xAxis.value)) y=\(String(format: "%.2f", d.yAxis.value))" }
        else if let a = e as? GCControllerAxisInput { v = "value=\(String(format: "%.2f", a.value))" }
        print(String(format: "      %-28s %-22s %@", (k as NSString).utf8String!,
                     ((e.localizedName ?? "-") as NSString).utf8String!, v))
    }

    if let m = c.motion { print("    motion: accel=\(m.hasGravityAndUserAcceleration) gyro=\(m.hasRotationRate)") }
    print("    haptics available: \(c.haptics != nil)")
    if let b = c.battery { print("    battery: level=\(b.batteryLevel) state=\(b.batteryState.rawValue)") }
    if let l = c.light { print("    light: color=\(String(describing: l.color))") }
}

print("\n=== IOHID 层看到的设备（vendor 1118 / 游戏手柄相关）===")
let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, nil)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
if let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> {
    for d in devs {
        let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
        let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
        let name = (IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String) ?? "-"
        let usagePage = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
        let usage = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
        if vid == 1118 || usagePage == 1 && (usage == 4 || usage == 5) || vid == 14295 || vid == 0x04B4 {
            print(String(format: "  vid=0x%04x pid=0x%04x usage=%d/%d GC-supported=%@ %@",
                         vid, pid, usagePage, usage, GCController.supportsHIDDevice(d) ? "yes" : "no", name))
        }
    }
}
