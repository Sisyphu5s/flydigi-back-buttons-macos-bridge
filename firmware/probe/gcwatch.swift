// gcwatch.swift — 实时观察 GameController 元素变化（用于端到端验证：板子→Mac）
// 用法: ./gcwatch [秒数]
import Foundation
import GameController

let dur = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 6 : 6
GCController.shouldMonitorBackgroundEvents = true

var target: GCController?
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: nil) { n in
    if target == nil { target = n.object as? GCController }
}

let deadline = Date().addingTimeInterval(3)
while target == nil && Date() < deadline {
    RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    if let c = GCController.controllers().first { target = c }
}
guard let c = target else { print("没有手柄"); exit(2) }

print("手柄: \(c.vendorName ?? "-")  类别: \(c.productCategory)  profile: \(type(of: c.physicalInputProfile))")
let xb = c.extendedGamepad as? GCXboxGamepad
print("GCXboxGamepad=\(xb != nil)  paddleButton1..4 = \(xb?.paddleButton1 != nil) \(xb?.paddleButton2 != nil) \(xb?.paddleButton3 != nil) \(xb?.paddleButton4 != nil)")
print("元素数: \(c.physicalInputProfile.elements.count)")
print("haptics=\(c.haptics != nil) battery=\(c.battery != nil)")
if let battery = c.battery {
    print("battery level=\(battery.batteryLevel) state=\(battery.batteryState.rawValue)")
}
print("--- 监听 \(Int(dur)) 秒（只打印变化的元素）---")

func snap(_ c: GCController) -> [String: String] {
    var d: [String: String] = [:]
    for (k, e) in c.physicalInputProfile.elements {
        if let b = e as? GCControllerButtonInput { d[k] = String(format: "%.2f", b.value) }
        else if let a = e as? GCControllerAxisInput { d[k] = String(format: "%.2f", a.value) }
        else if let dp = e as? GCControllerDirectionPad {
            d[k] = String(format: "x=%.2f y=%.2f", dp.xAxis.value, dp.yAxis.value)
        }
    }
    if let xb = xb {
        if let b = xb.paddleButton1 { d["XBOX_PADDLE_1"] = String(format: "%.2f", b.value) }
        if let b = xb.paddleButton2 { d["XBOX_PADDLE_2"] = String(format: "%.2f", b.value) }
        if let b = xb.paddleButton3 { d["XBOX_PADDLE_3"] = String(format: "%.2f", b.value) }
        if let b = xb.paddleButton4 { d["XBOX_PADDLE_4"] = String(format: "%.2f", b.value) }
    }
    return d
}

var prev = snap(c)
var changes = 0
let t0 = Date()
c.physicalInputProfile.valueDidChangeHandler = { _, _ in
    let cur = snap(c)
    var line: [String] = []
    for (k, v) in cur.sorted(by: { $0.key < $1.key }) where prev[k] != v {
        line.append("\(k) \(prev[k] ?? "-")→\(v)")
    }
    prev = cur
    if !line.isEmpty { changes += 1; print(String(format: "[%.1fs] ", Date().timeIntervalSince(t0)) + line.joined(separator: "  ")) }
}
while Date().timeIntervalSince(t0) < dur {
    RunLoop.current.run(until: Date().addingTimeInterval(0.01))
}
c.physicalInputProfile.valueDidChangeHandler = nil
print("--- 变化事件 \(changes) 次 ---")
