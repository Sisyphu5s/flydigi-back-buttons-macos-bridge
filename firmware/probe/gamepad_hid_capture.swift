// Usage: swift gamepad_hid_capture.swift [seconds] [--activity-triggered] [--wait-seconds=N]
import Foundation
import GameController
import IOKit.hid

let duration = max(1, Double(CommandLine.arguments.dropFirst().first ?? "20") ?? 20)
let triggered = CommandLine.arguments.contains("--activity-triggered")
let waitSeconds = CommandLine.arguments
    .first(where: { $0.hasPrefix("--wait-seconds=") })
    .flatMap { Double($0.dropFirst("--wait-seconds=".count)) }
    .map { min(300, max(1, $0)) } ?? 60
let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, nil)
guard IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess,
      let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else {
    print("Cannot open IOHIDManager")
    exit(1)
}

let genericNames = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Select", "Start",
                    "L3", "R3", "Up", "Down", "Left", "Right", "Home", "M1", "M2",
                    "M3", "M4", "C", "Z", "LM", "RM", "O"]
let vaderNames = ["A", "B", "C", "X", "Y", "Z", "LB", "RB", "LM", "RM",
                  "Select", "Start", "L3", "R3", "M1", "M2", "M3", "M4", "O", "Home"]
let appleNames = ["A", "B", "unused-3", "X", "Y", "unused-6", "LB", "RB",
                  "unused-9", "unused-10", "Select", "Start", "L3", "R3",
                  "M1", "M2", "M3", "M4", "unused-19", "Home", "C", "Z",
                  "LM", "RM", "O", "unused-26"]
let scufNames = ["A", "B", "unused-3", "X", "Y", "unused-6", "LB", "RB",
                 "unused-9", "unused-10", "Select", "Start", "Home", "L3", "R3",
                 "unused-16", "unused-17", "unused-18", "unused-19", "unused-20",
                 "M1", "M3", "M4", "M2", "C", "Z", "LM", "RM", "O"]
var names: [String] = []
var identity = ""
var previousButtons: UInt32?
var previousHat: Int?
var baselineAxes: [Int]?
var lastPrintedAxes: [Int]?
var lastAxisPrint = 0.0
var reportCount = 0
var buttonChanges = 0
var axisMinimum = Array(repeating: 255, count: 6)
var axisMaximum = Array(repeating: 0, count: 6)
var nonGamepadReportIDs = Set<Int>()
let started = ProcessInfo.processInfo.systemUptime
var captureStarted: TimeInterval? = triggered ? nil : started

func property(_ device: IOHIDDevice, _ key: String) -> Int {
    (IOHIDDeviceGetProperty(device, key as CFString) as? Int) ?? -1
}

guard let device = devices.first(where: { candidate in
    let vid = property(candidate, kIOHIDVendorIDKey)
    let pid = property(candidate, kIOHIDProductIDKey)
    let page = property(candidate, kIOHIDPrimaryUsagePageKey)
    let usage = property(candidate, kIOHIDPrimaryUsageKey)
    return ((vid == 0x1209 && pid == 0x0001) ||
            (vid == 0x04b4 && pid == 0x2412) ||
            (vid == 0x1b1c && pid == 0x3a28)) &&
           page == 1 && (usage == 4 || usage == 5)
}) else {
    print("Flydigi Bridge gamepad HID interface not found")
    exit(2)
}
let buttonUsages: Set<Int> = Set((IOHIDDeviceCopyMatchingElements(device, nil,
    IOOptionBits(kIOHIDOptionsTypeNone)) as? [IOHIDElement] ?? []).compactMap { element in
    guard IOHIDElementGetType(element) == kIOHIDElementTypeInput_Button,
          IOHIDElementGetUsagePage(element) == 9 else { return nil }
    return Int(IOHIDElementGetUsage(element))
})
let buttonCount = buttonUsages.count
let vid = property(device, kIOHIDVendorIDKey)
switch (vid, buttonCount) {
case (0x1209, 26): names = genericNames; identity = "Generic"
case (0x04b4, 20): names = vaderNames; identity = "Vader2Pro"
case (0x04b4, 26): names = appleNames; identity = "Vader2Pro 26-button experiment"
case (0x04b4, 29): names = scufNames; identity = "SCUF report under Vader2Pro ID"
case (0x1b1c, 29): names = scufNames; identity = "SCUF Omega"
default:
    print(String(format: "Unexpected gamepad layout %04x:%04x with %d buttons",
                 vid, property(device, kIOHIDProductIDKey), buttonCount))
    exit(3)
}
guard IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else {
    print("Cannot open gamepad HID interface")
    exit(3)
}
defer { IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone)) }
if #available(macOS 11.0, *) { GCController.shouldMonitorBackgroundEvents = true }
let gcInitial = GCController.controllers().count

let reportBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
defer { reportBuffer.deallocate() }
IOHIDDeviceRegisterInputReportCallback(device, reportBuffer, 64, { _, result, _, _, reportID, bytes, length in
    guard result == kIOReturnSuccess else { return }
    if reportID != 0 {
        nonGamepadReportIDs.insert(Int(reportID))
        return
    }
    guard length >= (names.count == 20 ? 10 : 11) else { return }
    let axes = (0..<6).map { Int(bytes[$0]) }
    if baselineAxes == nil { baselineAxes = axes }
    let hat = Int(bytes[6] & 0x0f)
    var buttons: UInt32 = 0
    for i in 0..<names.count where bytes[7 + i / 8] & (1 << (i % 8)) != 0 {
        buttons |= 1 << i
    }
    if triggered && captureStarted == nil {
        let axisMoved = baselineAxes.map { baseline in
            (0..<6).contains { abs(axes[$0] - baseline[$0]) >= 12 }
        } ?? false
        if axisMoved || (previousHat != nil && hat != previousHat) ||
           (previousButtons != nil && buttons != previousButtons) {
            captureStarted = ProcessInfo.processInfo.systemUptime
            print("input change detected; recording \(Int(duration)) seconds")
        }
    }
    defer {
        previousHat = hat
        previousButtons = buttons
    }
    guard captureStarted != nil else { return }
    reportCount += 1
    for i in 0..<6 {
        axisMinimum[i] = min(axisMinimum[i], axes[i])
        axisMaximum[i] = max(axisMaximum[i], axes[i])
    }
    let now = ProcessInfo.processInfo.systemUptime - started
    if let previous = lastPrintedAxes,
       now - lastAxisPrint >= 0.08,
       (0..<6).contains(where: { abs(axes[$0] - previous[$0]) >= 12 }) {
        print(String(format: "%.3fs axes LX=%d LY=%d RX=%d RY=%d RT=%d LT=%d",
                     now, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]))
        lastAxisPrint = now
        lastPrintedAxes = axes
    } else if lastPrintedAxes == nil {
        lastPrintedAxes = axes
    }
    if let previous = previousHat, hat != previous {
        print(String(format: "%.3fs hat %d -> %d", now, previous, hat))
    }
    if let previous = previousButtons {
        let changed = previous ^ buttons
        let raw = changed == 0 ? "" : (0..<length).map { String(format: "%02x", bytes[$0]) }.joined(separator: " ")
        for i in 0..<names.count where changed & (1 << i) != 0 {
            buttonChanges += 1
            print(String(format: "%.3fs %@ %@ (index %d) report=%@",
                         now,
                         buttons & (1 << i) != 0 ? "down" : "up", names[i], i, raw))
        }
    }
}, nil)
IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
print("Capturing \(identity) gamepad (\(buttonCount) buttons) for \(Int(duration)) seconds")
print("GameController initially \(gcInitial) controller(s)")
if triggered {
    print("waiting up to \(Int(waitSeconds)) seconds for input change")
    let deadline = Date().addingTimeInterval(waitSeconds)
    while captureStarted == nil && Date() < deadline {
        RunLoop.current.run(until: min(deadline, Date().addingTimeInterval(0.25)))
    }
    guard let captureStarted else {
        print("capture: no input change before timeout")
        print("GameController after timeout \(GCController.controllers().count) controller(s)")
        exit(11)
    }
    RunLoop.current.run(until: Date().addingTimeInterval(max(0, duration -
        (ProcessInfo.processInfo.systemUptime - captureStarted))))
} else {
    RunLoop.current.run(until: Date().addingTimeInterval(duration))
}
IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
let elapsed = ProcessInfo.processInfo.systemUptime - (captureStarted ?? started)
print(String(format: "USB reports=%d (%.1f/s), button transitions=%d", reportCount,
             Double(reportCount) / elapsed, buttonChanges))
let nonGamepadIDs = nonGamepadReportIDs.sorted().map(String.init).joined(separator: ", ")
print("Non-gamepad input report IDs: " + (nonGamepadIDs.isEmpty ? "none" : nonGamepadIDs))
print("Axis ranges LX/LY/RX/RY/RT/LT: " +
      (0..<6).map { "\(axisMinimum[$0])..\(axisMaximum[$0])" }.joined(separator: " "))
print("GameController after capture \(GCController.controllers().count) controller(s)")
