// hid_config_probe.swift — exercise the same Feature Report path used by WebHID.
// Usage: swift hid_config_probe.swift [--apple|--scuf|--stadia] [--rumble] [--rumble-timing] [--sensor|--sensor-stale-bias] [--status] [--activity|--activity-triggered] [--wait-seconds=N] [--config-roundtrip] [--reject-invalid] [--save]
import Foundation
import IOKit.hid

let doRumble = CommandLine.arguments.contains("--rumble")
let doRumbleTiming = CommandLine.arguments.contains("--rumble-timing")
let doSensorStaleBias = CommandLine.arguments.contains("--sensor-stale-bias")
let doSensor = doSensorStaleBias || CommandLine.arguments.contains("--sensor")
let doStatus = CommandLine.arguments.contains("--status")
let triggeredActivity = CommandLine.arguments.contains("--activity-triggered")
let doActivity = triggeredActivity || CommandLine.arguments.contains("--activity")
let triggerWaitSeconds = CommandLine.arguments
    .first(where: { $0.hasPrefix("--wait-seconds=") })
    .flatMap { Double($0.dropFirst("--wait-seconds=".count)) }
    .map { min(300, max(1, $0)) } ?? 60
let doConfigRoundtrip = CommandLine.arguments.contains("--config-roundtrip")
let doRejectInvalid = CommandLine.arguments.contains("--reject-invalid")
let doSave = CommandLine.arguments.contains("--save")
let appleIdentity = CommandLine.arguments.contains("--apple")
let scufIdentity = CommandLine.arguments.contains("--scuf")
let stadiaIdentity = CommandLine.arguments.contains("--stadia")
let targetVID = stadiaIdentity ? 0x18d1 : scufIdentity ? 0x1b1c : appleIdentity ? 0x04b4 : 0x1209
let targetPID = stadiaIdentity ? 0x9400 : scufIdentity ? 0x3a28 : appleIdentity ? 0x2412 : 0x0001
var expectedSaveSeq: UInt32?
var saveOutcome: UInt8?
var tokenSupported = false
var overlayTelemetrySupported = false
var rumbleAckSupported = false
var rumbleCounts: [UInt32] = []
var rumbleAcks: [(UInt8, UInt8)] = []
struct ActivitySnapshot {
    let time: TimeInterval
    let battery: UInt8
    let receiver: UInt8
    let extLive: UInt8
    let extFrames: UInt32
    let hidReports: UInt32
    let xinputReports: UInt32
    let inputChanges: UInt32
    let overlays: UInt32
    let ageLast: UInt32
    let ageMin: UInt32
    let ageMax: UInt32
}
var activityFirst: ActivitySnapshot?
var activityLast: ActivitySnapshot?
let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, nil)
IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
guard let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else {
    print("no HID devices")
    exit(1)
}

let target = devices.first { d in
    let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
    let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
    let page = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
    let usage = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
    return vid == targetVID && pid == targetPID && page == 0xff00 && usage == 1
}
guard let device = target else { print("WebHID config interface not found"); exit(2) }
let openResult = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
guard openResult == kIOReturnSuccess else { print("open failed: \(openResult)"); exit(3) }
defer { IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone)) }

func hex(_ bytes: UnsafePointer<UInt8>, _ count: Int) -> String {
    Data(bytes: bytes, count: count).map { String(format: "%02x", $0) }.joined(separator: " ")
}

@discardableResult
func command(_ code: UInt8, payload: [UInt8] = []) -> [UInt8]? {
    var request = [UInt8](repeating: 0, count: 63)
    request[0] = code
    for (i, value) in payload.prefix(request.count - 1).enumerated() { request[i + 1] = value }
    let token = UInt16.random(in: 1...UInt16.max)
    request[61] = UInt8(truncatingIfNeeded: token)
    request[62] = UInt8(truncatingIfNeeded: token >> 8)
    let setResult = request.withUnsafeBufferPointer { p in
        IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, CFIndex(0x10), p.baseAddress!, request.count)
    }
    print(String(format: "set code=0x%02x result=0x%08x", code, setResult))
    if setResult != kIOReturnSuccess { return nil }
    usleep(20_000)
    var response = [UInt8](repeating: 0, count: 64)
    var length = response.count
    let getResult = response.withUnsafeMutableBufferPointer { p in
        IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, CFIndex(0x11), p.baseAddress!, &length)
    }
    print(String(format: "get result=0x%08x length=%d data=%@", getResult, length,
                 response.withUnsafeBufferPointer { hex($0.baseAddress!, min(length, response.count)) }))
    guard getResult == kIOReturnSuccess else { return nil }
    let reportOffset = response[0] == 0x11 ? 1 : 0
    if length >= reportOffset + 63 {
        let echoed = UInt16(response[reportOffset + 61]) | UInt16(response[reportOffset + 62]) << 8
        if (tokenSupported || echoed != 0) && echoed != token {
            print("feature response token mismatch")
            return nil
        }
    } else if tokenSupported {
        print("feature response too short for token")
        return nil
    }
    if code == 0x01 && length > reportOffset + 13 {
        tokenSupported = response[reportOffset + 13] == 1
        overlayTelemetrySupported = length > reportOffset + 15 &&
            response[reportOffset + 15] & 2 != 0
        rumbleAckSupported = length > reportOffset + 15 &&
            response[reportOffset + 15] & 8 != 0
        print("request token support: \(tokenSupported)")
    }
    if code == 0x02 && getResult == kIOReturnSuccess {
        let offset = response[0] == 0x11 ? 3 : 2
        if length >= offset + 55 {
            let flags = UInt16(response[offset + 6]) | UInt16(response[offset + 7]) << 8
            let mapping = response[(offset + 30)..<(offset + 55)]
            print(String(format: "config flags=0x%04x map=%@", flags,
                         mapping.map { String($0) }.joined(separator: ",")))
        }
    }
    return response
}

func configBytes(_ response: [UInt8]?) -> [UInt8]? {
    guard let response = response else { return nil }
    let offset = response[0] == 0x11 ? 3 : 2
    guard response.count >= offset + 59 else { return nil }
    return Array(response[offset..<(offset + 59)])
}

func crc32(_ bytes: [UInt8]) -> UInt32 {
    var crc: UInt32 = 0xffffffff
    for byte in bytes {
        crc ^= UInt32(byte)
        for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0) }
    }
    return ~crc
}

print("=== \(stadiaIdentity ? "Stadia haptics experiment" : scufIdentity ? "SCUF identity" : appleIdentity ? "Apple identity" : "Generic") WebHID Feature probe ===")
command(0x01)
command(0x02)
if doRumble { command(0x07, payload: [180, 180]) }
if doConfigRoundtrip {
    guard let original = configBytes(command(0x02)) else { print("roundtrip: GET_CONFIG failed"); exit(4) }
    var changed = original
    let alternate: UInt16 = UInt16(original[8]) | UInt16(original[9]) << 8 == 1234 ? 1235 : 1234
    changed[8] = UInt8(truncatingIfNeeded: alternate)
    changed[9] = UInt8(truncatingIfNeeded: alternate >> 8)
    changed[6] ^= 0x0a  // left radial deadzone and fresh XInput sticks
    let checksum = crc32(Array(changed.prefix(55)))
    for i in 0..<4 { changed[55 + i] = UInt8(truncatingIfNeeded: checksum >> (8 * i)) }
    let setResponse = command(0x03, payload: changed)
    let applied = configBytes(command(0x02)) == changed
    let restoreResponse = command(0x03, payload: original)
    let restored = configBytes(command(0x02)) == original
    let statusOffset = setResponse?.first == 0x11 ? 1 : 0
    let restoreOffset = restoreResponse?.first == 0x11 ? 1 : 0
    let ok = setResponse?[statusOffset] == 0 && restoreResponse?[restoreOffset] == 0 && applied && restored
    print("config roundtrip: \(ok ? "PASS" : "FAIL") (temporary deadzone \(alternate), left radial and XInput stick source toggled, restored \(restored))")
    if !ok { exit(5) }
}
if doRejectInvalid {
    guard let original = configBytes(command(0x02)) else { print("invalid config: GET_CONFIG failed"); exit(8) }
    var invalid = original
    invalid[8] = 0xff
    invalid[9] = 0xff
    var sanitized = invalid
    sanitized[8] = 0xff
    sanitized[9] = 0x7f
    let checksum = crc32(Array(sanitized.prefix(55)))
    for i in 0..<4 { invalid[55 + i] = UInt8(truncatingIfNeeded: checksum >> (8 * i)) }
    let response = command(0x03, payload: invalid)
    let offset = response?.first == 0x11 ? 1 : 0
    let unchanged = configBytes(command(0x02)) == original
    let ok = response?[offset] == 1 && unchanged
    print("invalid config rejection: \(ok ? "PASS" : "FAIL") (status \(response?[offset] ?? 255), unchanged \(unchanged))")
    if !ok { exit(9) }
}
if doSensor {
    var originalConfig: [UInt8]?
    var applied = true
    if doSensorStaleBias {
        guard let original = configBytes(command(0x02)) else {
            print("stale sensor probe: GET_CONFIG failed")
            exit(12)
        }
        originalConfig = original
        var changed = original
        changed.replaceSubrange(18..<24, with: [0xd2, 0x04, 0xc9, 0xfd, 0x7a, 0x03])
        let checksum = crc32(Array(changed.prefix(55)))
        for i in 0..<4 { changed[55 + i] = UInt8(truncatingIfNeeded: checksum >> (8 * i)) }
        let response = command(0x03, payload: changed)
        let offset = response?.first == 0x11 ? 1 : 0
        applied = response?[offset] == 0 && configBytes(command(0x02)) == changed
        usleep(50_000)
    }
    var sensorZero = true
    for usage in (doSensorStaleBias ? [0x76] : [0x76, 0x73]) {
        guard let sensor = devices.first(where: { d in
            let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
            let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
            let page = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
            let primary = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
            return vid == targetVID && pid == targetPID && page == 0x20 && primary == usage
        }) else {
            print(String(format: "sensor usage 0x%02x not found", usage))
            if doSensorStaleBias { sensorZero = false }
            continue
        }
        let result = IOHIDDeviceOpen(sensor, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else {
            print("sensor open failed: \(result)")
            if doSensorStaleBias { sensorZero = false }
            continue
        }
        for id in [1, 2] {
            var report = [UInt8](repeating: 0, count: 7)
            var length = report.count
            let status = report.withUnsafeMutableBufferPointer { p in
                IOHIDDeviceGetReport(sensor, kIOHIDReportTypeInput, CFIndex(id), p.baseAddress!, &length)
            }
            print(String(format: "sensor usage=0x%02x id=%d result=0x%08x length=%d data=%@",
                         usage, id, status, length,
                         report.withUnsafeBufferPointer { hex($0.baseAddress!, min(length, report.count)) }))
            if doSensorStaleBias && (status != kIOReturnSuccess || length != 7 ||
                                     report[0] != id || report[1...6].contains(where: { $0 != 0 })) {
                sensorZero = false
            }
        }
        IOHIDDeviceClose(sensor, IOOptionBits(kIOHIDOptionsTypeNone))
    }
    if let original = originalConfig {
        let response = command(0x03, payload: original)
        let offset = response?.first == 0x11 ? 1 : 0
        let restored = response?[offset] == 0 && configBytes(command(0x02)) == original
        let ok = applied && sensorZero && restored
        print("stale sensor with temporary gyro bias: \(ok ? "PASS" : "FAIL") (restored \(restored))")
        if !ok { exit(12) }
    }
}
if doStatus || doActivity || doSave || doRumbleTiming {
    let input = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
    defer { input.deallocate() }
    IOHIDDeviceRegisterInputReportCallback(device, input, 64, { _, result, _, _, id, bytes, length in
        guard result == kIOReturnSuccess else { return }
        if !doActivity || doStatus {
            print(String(format: "status id=0x%02x length=%d data=%@", id, length, hex(bytes, length)))
        }
        let offset = length == 64 && bytes[0] == 0x12 ? 1 : 0
        func word(_ index: Int) -> UInt32 {
            (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[offset + index + $1]) << (8 * $1) }
        }
        if doActivity && id == 0x12 && length >= offset + 60 {
            let overlays = overlayTelemetrySupported && length >= offset + 63 ?
                UInt32(bytes[offset + 60]) | UInt32(bytes[offset + 61]) << 8 |
                UInt32(bytes[offset + 62]) << 16 : 0
            let snapshot = ActivitySnapshot(
                time: ProcessInfo.processInfo.systemUptime,
                battery: bytes[offset], receiver: bytes[offset + 1], extLive: bytes[offset + 2],
                extFrames: word(8), hidReports: word(12), xinputReports: word(36),
                inputChanges: word(56), overlays: overlays,
                ageLast: word(44), ageMin: word(48), ageMax: word(52))
            if triggeredActivity {
                if activityFirst == nil, let previous = activityLast,
                   snapshot.inputChanges != previous.inputChanges {
                    activityFirst = previous
                    print("input change detected; recording 8 seconds")
                }
            } else if activityFirst == nil {
                activityFirst = snapshot
            }
            activityLast = snapshot
        }
        if id == 0x12 && length >= offset + 20 {
            let count = (0..<4).reduce(UInt32(0)) {
                $0 | UInt32(bytes[offset + 16 + $1]) << (8 * $1)
            }
            rumbleCounts.append(count)
            if rumbleAckSupported && length >= offset + 8 {
                rumbleAcks.append((bytes[offset + 6], bytes[offset + 7]))
            }
        }
        if id == 0x12 && length >= offset + 60 {
            let ages = (0..<4).map { field in
                (0..<4).reduce(UInt32(0)) { value, byte in
                    value | UInt32(bytes[offset + 44 + field * 4 + byte]) << (8 * byte)
                }
            }
            if !doActivity || doStatus {
                print("bridge input age: last=\(ages[0]) min=\(ages[1]) max=\(ages[2]) us samples=\(ages[3])")
            }
        }
        if id == 0x12 && length >= offset + 33, let expected = expectedSaveSeq {
            let seq = (0..<4).reduce(UInt32(0)) { $0 | (UInt32(bytes[offset + 29 + $1]) << (8 * $1)) }
            if seq == expected && bytes[offset + 28] >= 2 { saveOutcome = bytes[offset + 28] }
        }
    }, nil)
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
    var rumbleTimingPassed = true
    var rumbleConfigRestored = false
    if doRumbleTiming {
        func runRumbleTiming() -> Bool {
            guard let original = configBytes(command(0x02)) else {
                print("rumble timing: original config unavailable")
                return false
            }
            defer {
                _ = command(0x07, payload: [0, 0])
                let restored = command(0x03, payload: original)
                let offset = restored?.first == 0x11 ? 1 : 0
                rumbleConfigRestored = restored?[offset] == 0 &&
                    configBytes(command(0x02)) == original
                print("rumble timing config restored: \(rumbleConfigRestored)")
            }
            var temporary = original
            temporary[24] = 255; temporary[25] = 255
            temporary[26] = 0xe8; temporary[27] = 0x03  // 1000 ms minimum interval
            temporary[28] = 0xfa; temporary[29] = 0x00  // 250 ms watchdog
            let checksum = crc32(Array(temporary.prefix(55)))
            for i in 0..<4 { temporary[55 + i] = UInt8(truncatingIfNeeded: checksum >> (8 * i)) }
            guard let applied = command(0x03, payload: temporary) else { return false }
            let offset = applied[0] == 0x11 ? 1 : 0
            guard applied[offset] == 0, configBytes(command(0x02)) == temporary else {
                print("rumble timing: temporary config not accepted")
                return false
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.4))
            guard let baseline = rumbleCounts.last else {
                print("rumble timing: no status report")
                return false
            }
            let ackStart = rumbleAcks.count
            guard let started = command(0x07, payload: [180, 180]) else { return false }
            let startOffset = started[0] == 0x11 ? 1 : 0
            guard started[startOffset] == 0 else { return false }
            RunLoop.current.run(until: Date().addingTimeInterval(0.75))
            let count = rumbleCounts.last ?? baseline
            let recentAcks = rumbleAcks.dropFirst(ackStart)
            let startIndex = recentAcks.firstIndex { $0.0 == 180 && $0.1 == 180 }
            let stopped = startIndex.map { index in
                recentAcks.dropFirst(recentAcks.distance(from: recentAcks.startIndex, to: index) + 1)
                    .contains { $0.0 == 0 && $0.1 == 0 }
            } ?? false
            let passed = count >= baseline + 2 && (!rumbleAckSupported || stopped)
            print("rumble timing: \(passed ? "PASS" : "FAIL") (sent \(baseline) -> \(count), OUT start/stop \(rumbleAckSupported ? stopped.description : "unsupported"), interval 1000 ms, watchdog 250 ms)")
            return passed
        }
        rumbleTimingPassed = runRumbleTiming()
        rumbleTimingPassed = rumbleTimingPassed && rumbleConfigRestored
    }
    if doSave {
        guard let response = command(0x05) else { print("save request failed"); exit(6) }
        let offset = response[0] == 0x11 ? 1 : 0
        guard response.count >= offset + 6 && response[offset] == 0 else {
            print("save request rejected"); exit(6)
        }
        expectedSaveSeq = (0..<4).reduce(UInt32(0)) { $0 | (UInt32(response[offset + 2 + $1]) << (8 * $1)) }
        print("save sequence: \(expectedSaveSeq!)")
    }
    if !doRumbleTiming {
        if triggeredActivity {
            print(String(format: "waiting up to %.0f seconds for input change", triggerWaitSeconds))
            let deadline = Date().addingTimeInterval(triggerWaitSeconds)
            while activityFirst == nil && Date() < deadline {
                RunLoop.current.run(until: min(deadline, Date().addingTimeInterval(0.25)))
            }
            if activityFirst != nil {
                RunLoop.current.run(until: Date().addingTimeInterval(8.0))
            }
        } else {
            RunLoop.current.run(until: Date().addingTimeInterval(doActivity ? 8.0 : 1.2))
        }
    }
    IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
    if doActivity, let first = activityFirst, let last = activityLast {
        let elapsed = max(0.001, last.time - first.time)
        let rate = { (start: UInt32, end: UInt32) -> String in
            String(format: "%.1f", Double(end &- start) / elapsed)
        }
        print(String(format: "activity window %.2fs receiver=%d EF-live=%d battery=%@",
                     elapsed, last.receiver, last.extLive,
                     last.battery == 0xff ? "unknown" : "\(last.battery)%"))
        print("EF \(rate(first.extFrames, last.extFrames)) Hz | XInput \(rate(first.xinputReports, last.xinputReports)) Hz | USB HID \(rate(first.hidReports, last.hidReports)) Hz | input changes \(rate(first.inputChanges, last.inputChanges)) Hz")
        if overlayTelemetrySupported {
            let overlays = (last.overlays &- first.overlays) & 0x00ffffff
            print(String(format: "XInput stick overlays %.1f Hz (%u reports)",
                         Double(overlays) / elapsed, overlays))
        }
        print("input age last/min/max \(last.ageLast)/\(last.ageMin)/\(last.ageMax) us; change samples +\(last.inputChanges &- first.inputChanges)")
    } else if doActivity {
        print(triggeredActivity ? "activity: no input change before timeout" :
                                  "activity: no complete status reports")
        exit(11)
    }
    if !rumbleTimingPassed { exit(10) }
    if doSave {
        print("flash save: \(saveOutcome == 2 ? "PASS" : "FAIL")")
        if saveOutcome != 2 { exit(7) }
    }
}
