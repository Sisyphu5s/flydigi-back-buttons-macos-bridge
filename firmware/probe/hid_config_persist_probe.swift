// hid_config_persist_probe.swift
// Verify a non-default WebHID config survives a real RP2350 reboot.
// Usage: swift hid_config_persist_probe.swift [--apple]
import Foundation
import IOKit.hid

let apple = CommandLine.arguments.contains("--apple")
let wantedVID = apple ? 0x04b4 : 0x1209
let wantedPID = apple ? 0x2412 : 0x0001
let cfgSize = 59
let reportSize = 63
let commandID = 0x10
let responseID = 0x11

func findConfigDevice() -> (IOHIDManager, IOHIDDevice)? {
    let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
    IOHIDManagerSetDeviceMatching(manager, nil)
    guard IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess,
          let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>,
          let device = devices.first(where: { d in
              let vid = (IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int) ?? -1
              let pid = (IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int) ?? -1
              let page = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsagePageKey as CFString) as? Int) ?? -1
              let usage = (IOHIDDeviceGetProperty(d, kIOHIDPrimaryUsageKey as CFString) as? Int) ?? -1
              return vid == wantedVID && pid == wantedPID && page == 0xff00 && usage == 1
          }) else {
        IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        return nil
    }
    return (manager, device)
}

func crc32(_ bytes: [UInt8]) -> UInt32 {
    var crc: UInt32 = 0xffffffff
    for byte in bytes {
        crc ^= UInt32(byte)
        for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0) }
    }
    return ~crc
}

func responsePayload(_ bytes: [UInt8], _ length: Int) -> [UInt8]? {
    let offset = bytes.first == UInt8(responseID) ? 1 : 0
    guard length >= offset + 2 else { return nil }
    return Array(bytes[offset..<length])
}

func command(_ device: IOHIDDevice, _ code: UInt8, payload: [UInt8] = []) -> [UInt8]? {
    var request = [UInt8](repeating: 0, count: reportSize)
    request[0] = code
    for (index, value) in payload.prefix(reportSize - 1).enumerated() { request[index + 1] = value }
    let token = UInt16.random(in: 1...UInt16.max)
    request[61] = UInt8(truncatingIfNeeded: token)
    request[62] = UInt8(truncatingIfNeeded: token >> 8)
    let setResult = request.withUnsafeBufferPointer { p in
        IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, CFIndex(commandID),
                              p.baseAddress!, request.count)
    }
    guard setResult == kIOReturnSuccess else { return nil }
    usleep(25_000)
    var response = [UInt8](repeating: 0, count: 64)
    var length = response.count
    let getResult = response.withUnsafeMutableBufferPointer { p in
        IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, CFIndex(responseID),
                              p.baseAddress!, &length)
    }
    guard getResult == kIOReturnSuccess,
          let payload = responsePayload(response, length), payload.count >= 63 else { return nil }
    let echoed = UInt16(payload[61]) | UInt16(payload[62]) << 8
    guard echoed == token else { return nil }
    return payload
}

func config(_ response: [UInt8]?) -> [UInt8]? {
    guard let response, response.count >= 2 + cfgSize, response[0] == 0 else { return nil }
    return Array(response[2..<(2 + cfgSize)])
}

func sendReboot() throws {
    let ports = try FileManager.default.contentsOfDirectory(atPath: "/dev")
        .filter { $0.hasPrefix("cu.usbmodem") }
        .sorted()
    guard let port = ports.first else { throw NSError(domain: "persist", code: 1,
                                                       userInfo: [NSLocalizedDescriptionKey: "CDC port not found"]) }
    let handle = try FileHandle(forWritingTo: URL(fileURLWithPath: "/dev/" + port))
    try handle.write(contentsOf: Data([0x6e])) // main.c: 'n' = watchdog reboot
    try handle.close()
}

func withDevice<T>(_ body: (IOHIDManager, IOHIDDevice) throws -> T) throws -> T {
    guard let (manager, device) = findConfigDevice() else {
        throw NSError(domain: "persist", code: 2,
                      userInfo: [NSLocalizedDescriptionKey: "WebHID config interface not found"])
    }
    defer { IOHIDDeviceClose(device, IOOptionBits(kIOHIDOptionsTypeNone));
            IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone)) }
    guard IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else {
        throw NSError(domain: "persist", code: 3,
                      userInfo: [NSLocalizedDescriptionKey: "config interface open failed"])
    }
    return try body(manager, device)
}

do {
    let original = try withDevice { _, device -> [UInt8] in
        guard let cfg = config(command(device, 0x02)) else { throw NSError(domain: "persist", code: 4) }
        return cfg
    }
    var changed = original
    let originalDeadzone = UInt16(original[8]) | UInt16(original[9]) << 8
    let changedDeadzone: UInt16 = originalDeadzone == 1234 ? 1235 : 1234
    changed[8] = UInt8(truncatingIfNeeded: changedDeadzone)
    changed[9] = UInt8(truncatingIfNeeded: changedDeadzone >> 8)
    changed[6] ^= 0x02 // left radial-deadzone mode
    let checksum = crc32(Array(changed.prefix(55)))
    for i in 0..<4 { changed[55 + i] = UInt8(truncatingIfNeeded: checksum >> (8 * i)) }

    try withDevice { _, device in
        guard let set = command(device, 0x03, payload: changed), set[0] == 0,
              config(command(device, 0x02)) == changed else {
            throw NSError(domain: "persist", code: 5,
                          userInfo: [NSLocalizedDescriptionKey: "temporary config was not applied"])
        }
        guard let save = command(device, 0x05), save[0] == 0 else {
            throw NSError(domain: "persist", code: 6,
                          userInfo: [NSLocalizedDescriptionKey: "save request was rejected"])
        }
    }
    usleep(250_000)
    try sendReboot()
    sleep(2)

    let afterReboot = try withDevice { _, device -> [UInt8] in
        guard let cfg = config(command(device, 0x02)) else { throw NSError(domain: "persist", code: 7) }
        return cfg
    }
    guard afterReboot == changed else {
        throw NSError(domain: "persist", code: 8,
                      userInfo: [NSLocalizedDescriptionKey: "changed config did not survive reboot"])
    }
    print("persistent config after reboot: PASS (left deadzone \(changedDeadzone), flags 0x\(String(format: "%04x", UInt16(changed[6]) | UInt16(changed[7]) << 8)))")

    try withDevice { _, device in
        guard let set = command(device, 0x03, payload: original), set[0] == 0,
              let save = command(device, 0x05), save[0] == 0 else {
            throw NSError(domain: "persist", code: 9,
                          userInfo: [NSLocalizedDescriptionKey: "original config restore failed"])
        }
    }
    try sendReboot()
    sleep(2)
    let restored = try withDevice { _, device -> [UInt8] in
        guard let cfg = config(command(device, 0x02)) else { throw NSError(domain: "persist", code: 10) }
        return cfg
    }
    guard restored == original else { throw NSError(domain: "persist", code: 11) }
    print("persistent config restore: PASS")
} catch {
    fputs("persistent config: FAIL (\(error))\n", stderr)
    exit(1)
}
