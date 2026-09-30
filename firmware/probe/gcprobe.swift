// gcprobe.swift — 用 GameController 框架问系统：认到了哪些手柄、暴露了哪些元素
// 运行：swift gcprobe.swift   （只读：只查询，不改任何系统配置）
import Foundation
import GameController
import IOKit.hid

print("=== GCController.controllers() ===")
let cs = GCController.controllers()
print("数量: \(cs.count)")

for (i, c) in cs.enumerated() {
    print("\n[\(i)] vendorName=\(c.vendorName ?? "-")  productCategory=\(c.productCategory)  attachedToDevice=\(c.isAttachedToDevice)")
    print("    physicalInputProfile: \(type(of: c.physicalInputProfile))")

    if let xb = c.extendedGamepad as? GCXboxGamepad {
        print("    ★ 是 GCXboxGamepad（Xbox 类别生效）")
        let pads: [(String, GCControllerButtonInput?)] = [
            ("paddleButton1", xb.paddleButton1), ("paddleButton2", xb.paddleButton2),
            ("paddleButton3", xb.paddleButton3), ("paddleButton4", xb.paddleButton4)]
        for (n, b) in pads {
            if let b = b { print("      \(n): localizedName=\(b.localizedName ?? "-") value=\(b.value) pressed=\(b.isPressed)") }
            else { print("      \(n): nil") }
        }
    } else if c.extendedGamepad != nil {
        print("    （不是 GCXboxGamepad，只是通用 extendedGamepad）")
    }

    let els = c.physicalInputProfile.elements
    print("    元素数: \(els.count)")
    for k in els.keys.sorted() {
        let e = els[k]!
        var v = ""
        if let b = e as? GCControllerButtonInput { v = "value=\(String(format: "%.2f", b.value)) pressed=\(b.isPressed)" }
        else if let d = e as? GCControllerDirectionPad { v = "x=\(String(format: "%.2f", d.xAxis.value)) y=\(String(format: "%.2f", d.yAxis.value))" }
        else if let a = e as? GCControllerAxisInput { v = "value=\(String(format: "%.2f", a.value))" }
        print("      \(k.padding(toLength: 32, withPad: " ", startingAt: 0)) name=\((e.localizedName ?? "-").padding(toLength: 22, withPad: " ", startingAt: 0)) \(v)")
    }
    if let m = c.motion { print("    motion: gravity/accel=\(m.hasGravityAndUserAcceleration) gyro=\(m.hasRotationRate)") }
    if let b = c.battery { print("    battery: level=\(b.batteryLevel) state=\(b.batteryState.rawValue)") }
    if let l = c.light { print("    light: \(String(describing: l.color))") }
}

print("\n=== IOHID 层相关设备（手柄类 usage 1/4|1/5|1/8，或 Xbox/飞智 VID）===")
let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, nil)
IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
if let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> {
    for d in devs {
        let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
        let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
        let name = (IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String) ?? "-"
        let up = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
        let u = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
        let isPad = (up == 1 && (u == 4 || u == 5 || u == 8))
        if isPad || vid == 1118 || vid == 14295 || vid == 0x04B4 || pid == 0x2412 {
            print(String(format: "  vid=0x%04x pid=0x%04x primary=%d/%d  %@", vid, pid, up, u, name))
        }
    }
}
