// gc_elements.swift — 问系统：认到哪些手柄、每个手柄暴露的"元素标识符"是什么
// 用途：验证冒充身份是否让 macOS 露出 BUTTON_M1..M4
// 运行：swift gc_elements.swift
import Foundation
import GameController

func kind(_ e: GCControllerElement) -> String {
    if e is GCControllerButtonInput { return "btn " }
    if e is GCControllerDirectionPad { return "dpad" }
    if e is GCControllerAxisInput { return "axis" }
    return "??  "
}

let cs = GCController.controllers()
print("=== GCController.controllers(): \(cs.count) 个 ===")
for (i, c) in cs.enumerated() {
    print("\n[\(i)] vendor=\(c.vendorName ?? "-")  category=\(c.productCategory)")
    if #available(macOS 11.0, *) {
        print("    localizedName(if any)=\(c.physicalInputProfile.elements.count) elements")
    }
    let p = c.physicalInputProfile
    print("    elements: \(p.elements.count)")
    for k in p.elements.keys.sorted() {
        if let e = p.elements[k] { print("      \(kind(e)) \(k)") }
    }
    print("    .buttons: \(p.buttons.keys.sorted())")
    print("    .axes   : \(p.axes.keys.sorted())")
    print("    .dpads  : \(p.dpads.keys.sorted())")
    print("    extendedGamepad: \(c.extendedGamepad != nil ? "有" : "无")")
    if let eg = c.extendedGamepad {
        var names: [String] = []
        let ops: [(String, GCControllerElement?)] = [
            ("buttonA", eg.buttonA), ("buttonB", eg.buttonB), ("buttonX", eg.buttonX), ("buttonY", eg.buttonY),
            ("leftShoulder", eg.leftShoulder), ("rightShoulder", eg.rightShoulder),
            ("leftTrigger", eg.leftTrigger), ("rightTrigger", eg.rightTrigger),
            ("leftThumbstick", eg.leftThumbstick), ("rightThumbstick", eg.rightThumbstick),
            ("dpad", eg.dpad), ("menu", eg.buttonMenu), ("home", eg.buttonHome),
            ("options", eg.buttonOptions),
        ]
        for (n, o) in ops where o != nil { names.append(n) }
        print("    extendedGamepad 非 nil 的元素: \(names.joined(separator: ", "))")
    }
}

print("\n=== 按键实时读数（按 M1–M4 各一次，2 秒内观察）===")
if let c = cs.first {
    let p = c.physicalInputProfile
    for k in p.elements.keys.sorted() {
        if let b = p.elements[k] as? GCControllerButtonInput {
            b.pressedChangedHandler = { _, value, pressed in
                if pressed { print("  ↓ \(k)") } else { print("  ↑ \(k)") }
            }
        }
    }
    RunLoop.current.run(until: Date().addingTimeInterval(2.5))
}
