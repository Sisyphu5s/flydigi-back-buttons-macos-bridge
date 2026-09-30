/* Standard HID Battery System collection for an isolated interface.
 * Keep this separate from the gamepad report: macOS drops the unnumbered
 * gamepad input callback when another report ID is added to that interface. */
#pragma once
#include <stdint.h>

static const uint8_t kDescBattery[] = {
    0x05, 0x84,       /* Usage Page (Power Device) */
    0x09, 0x10,       /* Usage (Battery System) */
    0xa1, 0x00,       /* Collection (Physical) */
    0x09, 0x12,       /* Usage (Battery) */
    0xa1, 0x00,       /* Collection (Physical) */
    0x05, 0x85,       /* Usage Page (Battery System) */
    0x09, 0x65,       /* Usage (Absolute State Of Charge, percent) */
    0x85, 0x06,       /* Report ID (Battery System battery) */
    0x15, 0x00,
    0x25, 0x64,       /* 0..100 percent */
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0xc0,             /* End Battery collection */
    0xc0,             /* End Battery System collection */
};

#define BATTERY_REPORT_BYTES 1u
#define BATTERY_REPORT_ID 6u
