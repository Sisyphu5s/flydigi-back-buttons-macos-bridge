/* usb_descriptors.c — 设备侧（USB-C → Mac）
 *
 * 身份：冒充 Flydigi Vader2Pro.MobileUSB
 *   匹配键（一手，docs/11 §2）：idVendor 0x04B4 / idProduct 0x2412 / bcdDevice 0x0500
 *   模型：GameControllers-Custom.bundle/Personalities/Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist
 *   → 苹果据此把 M1–M4 暴露成 BUTTON_M1..M4（描述符里是按钮 15..18）
 *
 * 复合设备的接口顺序：0 = HID（手柄本体，放最前，与真实设备一致）
 *                       1 = HID Sensors（gyro/accel，供 WebHID）
 *                       2 = vendor HID Feature Reports（WebHID 配置）
 *                       3/4 = CDC（控制台，只为可观测性）
 */
#include "tusb.h"
#include "pico/unique_id.h"
#include "vader2pro_hid.h"
#include "desc_vader2pro.h"
#ifdef GENERIC_GAMEPAD
#include "desc_generic_gamepad.h"
#endif
#ifdef APPLE_BATTERY_INTERFACE_PROBE
#include "desc_battery.h"
#endif
#ifdef GENERIC_PID_PROBE
#include "desc_pid_probe.h"
#include "pid_probe_report.h"
#endif
#ifdef CHROMIUM_STADIA_PROBE
#include "desc_stadia_haptic_probe.h"
#endif
#ifdef CORSAIR_APPLE_IDENTITY
#include "desc_scuf_omega.h"
#endif
#include "desc_sensor.h"
#include "desc_config.h"
#include "usb_descriptors.h"
#include "flydigi_host.h"
#include "rumble_report.h"

/* 字符串索引 */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
};

/* 接口号 */
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_SENSOR,
    ITF_NUM_CONFIG,
#ifdef APPLE_BATTERY_INTERFACE_PROBE
    ITF_NUM_BATTERY,
#endif
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL,
};

/* 端点 */
#define EPNUM_HID_OUT   0x01
#define EPNUM_HID_IN    0x81
#define EPNUM_SENSOR_IN 0x84
#define EPNUM_CONFIG_IN 0x85
#ifdef APPLE_BATTERY_INTERFACE_PROBE
#define EPNUM_BATTERY_IN 0x86
#endif
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x83

/* ---------------- 设备描述符 ---------------- */
#ifdef CORSAIR_APPLE_IDENTITY
#ifdef CORSAIR_KNOWN_USB_ID
#define USB_VID 0x04b4
#define USB_PID 0x2412
#define USB_BCD 0x0500
#else
#define USB_VID 0x1b1c
#define USB_PID 0x3a28
#define USB_BCD 0x0100
#endif
#define ACTIVE_GAMEPAD_DESC kDescScufOmega
#elif defined(GENERIC_PID_PROBE)
#define USB_VID 0x1209
#define USB_PID 0x0001
#define USB_BCD 0x0101
#define ACTIVE_GAMEPAD_DESC kDescPidProbe
#elif defined(CHROMIUM_STADIA_PROBE)
#define USB_VID 0x18d1
#define USB_PID 0x9400
#define USB_BCD 0x0100
#define ACTIVE_GAMEPAD_DESC kDescStadiaHapticProbe
#elif defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY)
/* pid.codes reserves 1209:0001 for private testing, not redistributed devices. */
#define USB_VID 0x1209
#define USB_PID 0x0001
#define USB_BCD 0x0100
#define ACTIVE_GAMEPAD_DESC kDescGenericGamepad
#elif defined(GENERIC_APPLE_IDENTITY)
#define USB_VID 0x04B4
#define USB_PID 0x2412
#define USB_BCD 0x0500
#define ACTIVE_GAMEPAD_DESC kDescGenericGamepad
#else
#define USB_VID 0x04B4
#define USB_PID 0x2412
#define USB_BCD 0x0500
#define ACTIVE_GAMEPAD_DESC kDescVader2Pro
#endif

tusb_desc_device_t const desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB          = 0x0200,
    .bDeviceClass    = 0x00,          /* 按接口定义 */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor        = USB_VID,
    .idProduct       = USB_PID,
    .bcdDevice       = USB_BCD,
    .iManufacturer   = STRID_MANUFACTURER,
    .iProduct        = STRID_PRODUCT,
    .iSerialNumber   = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* ---------------- 配置描述符（含 CDC 的 IAD） ---------------- */
/* TUD_CDC_DESC_LEN(66) 已含 8 字节 IAD，勿再手工加 */
#ifdef APPLE_BATTERY_INTERFACE_PROBE
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + \
                         TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + \
                         TUD_CDC_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + \
                         TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#endif

uint8_t const desc_configuration[] = {
    /* config: 接口数 / 配置号 / 字符串 / 总长 / 属性(自供电? 0=总线供电) / 电流 100mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

    /* --- 接口 0：HID 手柄 --- */
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(ACTIVE_GAMEPAD_DESC), EPNUM_HID_OUT, EPNUM_HID_IN,
                             CFG_TUD_HID_EP_BUFSIZE, 1),

    /* --- 接口 1：标准 HID Sensor（gyro report 1 / accel report 2） --- */
    TUD_HID_DESCRIPTOR(ITF_NUM_SENSOR, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(kDescSensor), EPNUM_SENSOR_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),

    /* --- 接口 2：厂商定义 WebHID Feature Reports --- */
    TUD_HID_DESCRIPTOR(ITF_NUM_CONFIG, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(kDescBridgeConfig), EPNUM_CONFIG_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

#ifdef APPLE_BATTERY_INTERFACE_PROBE
    /* Keep the battery collection off the gamepad interface. */
    TUD_HID_DESCRIPTOR(ITF_NUM_BATTERY, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(kDescBattery), EPNUM_BATTERY_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),
#endif

    /* --- 接口 2/3：CDC 控制台（TUD_CDC_DESCRIPTOR 自带 IAD，勿重复手写） --- */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

/* ---------------- HID 报告描述符（输入 10 字节 / Output 8 字节） ---------------- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (instance == 1) return kDescSensor;
    if (instance == 2) return kDescBridgeConfig;
#ifdef APPLE_BATTERY_INTERFACE_PROBE
    if (instance == 3) return kDescBattery;
#endif
    return ACTIVE_GAMEPAD_DESC;
}

/* ---------------- 字符串描述符 ---------------- */
static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   /* 0x0409 = en-US */
    "Flydigi",                      /* 1 */
#ifdef CORSAIR_APPLE_IDENTITY
#ifdef CORSAIR_KNOWN_USB_ID
    "Vader 2 Pro",                  /* 2: descriptor-only probe */
#else
    "SCUF Omega Bridge Experiment", /* 2 */
#endif
#elif defined(GENERIC_PID_PROBE)
    "Flydigi Bridge PID Probe",    /* 2 */
#elif defined(CHROMIUM_STADIA_PROBE)
    "Flydigi Bridge Haptics Probe", /* 2 */
#elif defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY)
    "Flydigi Bridge Generic Game Pad", /* 2 */
#else
    "Vader 2 Pro",                  /* 2 */
#endif
    NULL,                           /* 3 = 序列号，运行期填 */
    "Flydigi Bridge Console",       /* 4 */
};

static uint16_t _desc_str[32];
static char _serial[16 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t desc_id, uint16_t langid)
{
    (void)langid;
    if (desc_id == 0) {
        _desc_str[1] = 0x0409;
        _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * 1 + 2));
        return _desc_str;
    }

    if (desc_id >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;

    char const *str = string_desc_arr[desc_id];
    if (desc_id == STRID_SERIAL) {
        pico_get_unique_board_id_string(_serial, sizeof _serial);
        str = _serial;
    }
    if (str == NULL) return NULL;

    size_t count = 0;
    while (count < sizeof _desc_str / sizeof _desc_str[0] - 1 && str[count] != 0) {
        _desc_str[1 + count] = (uint16_t)(uint8_t)str[count];
        count++;
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return _desc_str;
}

/* ---------------- 设备类回调 ---------------- */
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_configuration; }

/* GET_REPORT 时回最近一帧（macOS 有时会来问） */
static uint8_t s_last_report[VADER2PRO_REPORT_BYTES];
#ifdef GENERIC_PID_PROBE
static uint8_t s_last_pid_report[PID_PROBE_INPUT_BYTES];
#endif
static uint8_t s_last_sensor_gyro[SENSOR_DESC_REPORT_BYTES];
static uint8_t s_last_sensor_accel[SENSOR_DESC_REPORT_BYTES];
#ifdef APPLE_BATTERY_INTERFACE_PROBE
static uint8_t s_last_battery_interface_report[BATTERY_REPORT_BYTES];
#endif
#ifdef APPLE_BATTERY_PROBE
static uint8_t s_last_battery_report[APPLE_BATTERY_REPORT_BYTES];
#endif

void vader2pro_set_last_report(const uint8_t *rep)
{
    for (size_t i = 0; i < VADER2PRO_REPORT_BYTES; i++) s_last_report[i] = rep[i];
#ifdef GENERIC_PID_PROBE
    pid_probe_pack_input(rep, s_last_pid_report);
#endif
}

void vader_sensor_set_last_report(uint8_t report_id, const uint8_t *rep)
{
    if (rep == NULL) return;
    uint8_t *dst = report_id == SENSOR_GYRO_REPORT_ID ? s_last_sensor_gyro :
                   report_id == SENSOR_ACCEL_REPORT_ID ? s_last_sensor_accel : NULL;
    if (dst == NULL) return;
    for (size_t i = 0; i < SENSOR_DESC_REPORT_BYTES; i++) dst[i] = rep[i];
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    if (instance == 2 && report_type == HID_REPORT_TYPE_FEATURE)
        return bridge_config_get_report(report_id, buffer, reqlen);
    if (report_type != HID_REPORT_TYPE_INPUT) return 0;
    if (instance == 1 && (report_id == SENSOR_GYRO_REPORT_ID || report_id == SENSOR_ACCEL_REPORT_ID)) {
        const uint8_t *src = report_id == SENSOR_GYRO_REPORT_ID ? s_last_sensor_gyro : s_last_sensor_accel;
        uint16_t n = reqlen < SENSOR_DESC_REPORT_BYTES ? reqlen : SENSOR_DESC_REPORT_BYTES;
        for (uint16_t i = 0; i < n; i++) buffer[i] = src[i];
        return n;
    }
#ifdef APPLE_BATTERY_INTERFACE_PROBE
    if (instance == 3 && report_id == BATTERY_REPORT_ID) {
        uint16_t n = reqlen < BATTERY_REPORT_BYTES ? reqlen : BATTERY_REPORT_BYTES;
        for (uint16_t i = 0; i < n; i++) buffer[i] = s_last_battery_interface_report[i];
        return n;
    }
#endif
#ifdef APPLE_BATTERY_PROBE
    if (instance == 0 && report_type == HID_REPORT_TYPE_INPUT &&
        report_id == APPLE_BATTERY_REPORT_ID) {
        uint16_t n = reqlen < APPLE_BATTERY_REPORT_BYTES ? reqlen : APPLE_BATTERY_REPORT_BYTES;
        for (uint16_t i = 0; i < n; i++) buffer[i] = s_last_battery_report[i];
        return n;
    }
#endif
    if (instance != 0) return 0;
#ifdef GENERIC_PID_PROBE
    if (report_id != PID_PROBE_INPUT_REPORT_ID) return 0;
    uint16_t n = reqlen < PID_PROBE_INPUT_BYTES ? reqlen : PID_PROBE_INPUT_BYTES;
    for (uint16_t i = 0; i < n; i++) buffer[i] = s_last_pid_report[i];
#else
#ifdef CHROMIUM_STADIA_PROBE
    if (report_id != 1) return 0;
#else
    if (report_id != 0) return 0;
#endif
    uint16_t n = reqlen < VADER2PRO_REPORT_BYTES ? reqlen : VADER2PRO_REPORT_BYTES;
    for (uint16_t i = 0; i < n; i++) buffer[i] = s_last_report[i];
#endif
    return n;
}

#ifdef APPLE_BATTERY_PROBE
void vader_battery_set_last_report(uint8_t level)
{
    s_last_battery_report[0] = level > 100u ? 100u : level;
}
#endif

#ifdef APPLE_BATTERY_INTERFACE_PROBE
void vader_battery_interface_set_last_report(uint8_t level)
{
    s_last_battery_interface_report[0] = level > 100u ? 100u : level;
}
#endif

/* 主机发来的输出报文：转成接收器的 XInput 震动命令。 */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    if (instance == 2) {
        bridge_config_set_report(report_id, report_type, buffer, bufsize);
        return;
    }
    (void)report_id;
    if (instance != 0 || report_type != HID_REPORT_TYPE_OUTPUT || buffer == NULL) return;
#ifdef GENERIC_PID_PROBE
    /* This descriptor-only probe has no PID effect engine. Never interpret a
     * PID Output report as the legacy unnumbered rumble packet. */
    return;
#endif

    uint8_t left = 0, right = 0;
#ifdef CHROMIUM_STADIA_PROBE
    if (flydigi_stadia_haptic_decode(report_id, buffer, bufsize, &left, &right))
#else
    if (flydigi_rumble_decode(buffer, bufsize, &left, &right))
#endif
        flydigi_host_rumble(left, right);
}
