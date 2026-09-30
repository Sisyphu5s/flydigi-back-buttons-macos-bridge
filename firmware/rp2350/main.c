/* main.c — RP2350 飞智桥固件
 *
 *   USB-A（PIO 软 USB host）← 飞智 2.4G 接收器
 *   USB-C（原生 USB device）→ Mac，冒充 Flydigi Vader2Pro.MobileUSB
 *
 * 双栈分工照官方示例 Pico-PIO-USB/examples/host_hid_to_device_cdc：
 *   - host 独占 core1（tuh_task 是“等事件”语义，独占核才不互相饿死）
 *   - device 在 core0（USB IRQ 装在调用 tud_init 的那个核上）
 * 另有三处必须照做的实现细节：
 *   - 本仓库 TinyUSB 是 0.20 系：tud_task() = 等事件阻塞版，core0 必须用 tud_task_ext(0,false)
 *   - 挂载前必须做 DP/DM 强拉低复位（绕过板上 R13），否则接收器不挂载
 *   - 日志走自定义 CDC（接口 2/3），不用 SDK stdio
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "pio_usb.h"
#include "tusb.h"
#include "flydigi_host.h"
#include "bridge_map.h"
#include "bridge_config.h"
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

#define PIN_USB_DP 12
#define PIN_USB_DM 13

#ifdef CORSAIR_APPLE_IDENTITY
#ifdef CORSAIR_KNOWN_USB_ID
#define BRIDGE_USB_VID 0x04b4
#define BRIDGE_USB_PID 0x2412
#define BRIDGE_USB_BCD 0x0500
#else
#define BRIDGE_USB_VID 0x1b1c
#define BRIDGE_USB_PID 0x3a28
#define BRIDGE_USB_BCD 0x0100
#endif
#define BRIDGE_GAMEPAD_DESC kDescScufOmega
#elif defined(GENERIC_PID_PROBE)
#define BRIDGE_USB_VID 0x1209
#define BRIDGE_USB_PID 0x0001
#define BRIDGE_USB_BCD 0x0101
#define BRIDGE_GAMEPAD_DESC kDescPidProbe
#elif defined(CHROMIUM_STADIA_PROBE)
#define BRIDGE_USB_VID 0x18d1
#define BRIDGE_USB_PID 0x9400
#define BRIDGE_USB_BCD 0x0100
#define BRIDGE_GAMEPAD_DESC kDescStadiaHapticProbe
#elif defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY)
/* pid.codes reserves 1209:0001 for private testing, not redistributed devices. */
#define BRIDGE_USB_VID 0x1209
#define BRIDGE_USB_PID 0x0001
#define BRIDGE_USB_BCD 0x0100
#define BRIDGE_GAMEPAD_DESC kDescGenericGamepad
#elif defined(GENERIC_APPLE_IDENTITY)
#define BRIDGE_USB_VID 0x04B4
#define BRIDGE_USB_PID 0x2412
#define BRIDGE_USB_BCD 0x0500
#define BRIDGE_GAMEPAD_DESC kDescGenericGamepad
#else
#define BRIDGE_USB_VID 0x04B4
#define BRIDGE_USB_PID 0x2412
#define BRIDGE_USB_BCD 0x0500
#define BRIDGE_GAMEPAD_DESC kDescVader2Pro
#endif

#define HID_REPORT_INTERVAL_US 1000u    /* 1 kHz 桥接节拍 */
#define HID_KEEPALIVE_US       1000u    /* 无变化时也保持 1 kHz USB 输出 */

#ifdef BRINGUP_BOOTSEL_FALLBACK_MS
#define BRINGUP_MS BRINGUP_BOOTSEL_FALLBACK_MS
#endif

static bool     s_verbose;
static bool     s_banner_done;
static uint32_t s_reports_sent;
static uint32_t s_last_sent_us;
static uint8_t  s_last_rep[VADER2PRO_REPORT_BYTES];
static uint32_t s_bridge_age_last_us, s_bridge_age_min_us, s_bridge_age_max_us;
static uint32_t s_bridge_age_samples;
static uint32_t s_xinput_stick_reports;
static alarm_pool_t *s_host_alarm_pool;   /* 给 PIO-USB 库的 SOF 定时器用 */

/* WebHID configuration state.  The response is kept in RAM so a browser can
 * issue a command and then read it back with receiveFeatureReport(). */
static uint8_t s_cfg_response[BRIDGE_CONFIG_REPORT_BYTES];
static volatile bool s_cfg_save_pending;
static uint8_t s_cfg_save_state; /* 0=none, 1=pending, 2=saved, 3=failed */
static uint32_t s_cfg_save_seq;
static uint32_t s_cfg_status_reports;
static void brg_log(const char *fmt, ...);

#ifdef APPLE_PERSISTENT_CONFIG
#define BRIDGE_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)
#else
#define BRIDGE_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#endif
static uint8_t s_flash_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

enum {
    CFG_CMD_GET_INFO = 0x01,
    CFG_CMD_GET_CONFIG = 0x02,
    CFG_CMD_SET_CONFIG = 0x03,
    CFG_CMD_RESET = 0x04,
    CFG_CMD_SAVE = 0x05,
    CFG_CMD_SET_GYRO_BIAS = 0x06,
    CFG_CMD_TEST_RUMBLE = 0x07,
};

static bool config_load_flash(void)
{
    const bridge_config_t *stored = (const bridge_config_t *)(XIP_BASE + BRIDGE_CONFIG_FLASH_OFFSET);
    if (!bridge_config_validate(stored)) return false;
#if defined(APPLE_PERSISTENT_CONFIG)
    return bridge_config_set(stored);
#elif defined(GENERIC_APPLE_IDENTITY) || defined(CORSAIR_APPLE_IDENTITY) || defined(GENERIC_PID_PROBE) || defined(CHROMIUM_STADIA_PROBE)
    /* The experimental Apple-order map must not interpret a saved Generic
     * browser-order mapping as native buttons. Leave Flash untouched. */
    return false;
#elif defined(GENERIC_GAMEPAD)
    bridge_config_t loaded = *stored;
    if (bridge_config_upgrade_generic_defaults(&loaded)) s_cfg_save_pending = true;
    return bridge_config_set(&loaded);
#else
    return bridge_config_set(stored);
#endif
}

static void config_flash_write_cb(void *param)
{
    const bridge_config_t *cfg = (const bridge_config_t *)param;
    for (size_t i = 0; i < sizeof s_flash_page; i++) s_flash_page[i] = 0xff;
    memcpy(s_flash_page, cfg, sizeof *cfg);
    flash_range_erase(BRIDGE_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(BRIDGE_CONFIG_FLASH_OFFSET, s_flash_page, FLASH_PAGE_SIZE);
}

static bool config_save_flash(void)
{
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    bridge_config_finalize(&cfg);
    int const rc = flash_safe_execute(config_flash_write_cb, &cfg, 2000);
    if (rc != PICO_OK) {
        brg_log("[CFG] flash_safe_execute rc=%d\n", rc);
        return false;
    }
    const bridge_config_t *stored = (const bridge_config_t *)(XIP_BASE + BRIDGE_CONFIG_FLASH_OFFSET);
    if (!bridge_config_validate(stored)) {
        brg_log("[CFG] flash readback CRC invalid\n");
        return false;
    }
    if (memcmp(stored, &cfg, sizeof cfg) != 0) {
        brg_log("[CFG] flash readback mismatch\n");
        return false;
    }
    return true;
}

static void cfg_response_clear(void)
{
    memset(s_cfg_response, 0, sizeof s_cfg_response);
}

static void cfg_response_status(uint8_t status)
{
    cfg_response_clear();
    s_cfg_response[0] = status;
    s_cfg_response[1] = BRIDGE_CONFIG_VERSION;
}

#if defined(APPLE_PERSISTENT_CONFIG) || (!defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY) && !defined(GENERIC_PID_PROBE) && !defined(CHROMIUM_STADIA_PROBE))
static void cfg_response_save_queued(void)
{
    s_cfg_save_pending = true;
    s_cfg_save_state = 1;
    s_cfg_save_seq++;
    cfg_response_status(0);
    for (unsigned i = 0; i < 4; i++)
        s_cfg_response[2 + i] = (uint8_t)(s_cfg_save_seq >> (8 * i));
}
#endif

static void cfg_response_info(void)
{
    fd_host_status_t st;
    flydigi_host_get(NULL, &st, NULL);
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    cfg_response_status(0);
    s_cfg_response[2] = (uint8_t)sizeof cfg;
    s_cfg_response[3] = (uint8_t)BRIDGE_CONFIG_REPORT_BYTES;
    s_cfg_response[4] = st.battery_valid ? 1u : 0u;
    s_cfg_response[5] = st.battery_valid ? st.battery_percent : 0xffu;
    s_cfg_response[6] = st.receiver_mounted ? 1u : 0u;
    s_cfg_response[7] = (uint8_t)BRIDGE_INPUT_BUTTONS;
    s_cfg_response[8] = 1; /* gyro sensor interface */
    s_cfg_response[9] = 1; /* dual motor output */
    s_cfg_response[12] = (uint8_t)BRIDGE_OUTPUT_BUTTONS;
    s_cfg_response[13] = 1; /* request token echo supported */
    s_cfg_response[15] = 15; /* XInput sticks, overlay telemetry, raw buttons, rumble OUT */
#ifdef APPLE_PERSISTENT_CONFIG
    s_cfg_response[15] |= 16; /* profile-specific Flash config */
#endif
#ifdef CORSAIR_APPLE_IDENTITY
    s_cfg_response[14] = 2; /* experimental SCUF-order 29-button profile */
#elif defined(GENERIC_APPLE_IDENTITY)
    s_cfg_response[14] = 1; /* experimental Apple-order 26-button profile */
#elif defined(CHROMIUM_STADIA_PROBE)
    s_cfg_response[14] = 3; /* isolated Chromium haptics probe */
#endif
    s_cfg_response[10] = (uint8_t)(st.xinput_min_interval_us > 255u ? 255u : st.xinput_min_interval_us);
    s_cfg_response[11] = (uint8_t)(st.xinput_max_interval_us > 255u ? 255u : st.xinput_max_interval_us);
}

uint16_t bridge_config_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen)
{
    if (buffer == NULL || report_id != BRIDGE_CONFIG_RESPONSE_REPORT_ID) return 0;
    uint16_t n = reqlen < BRIDGE_CONFIG_REPORT_BYTES ? reqlen : BRIDGE_CONFIG_REPORT_BYTES;
    memcpy(buffer, s_cfg_response, n);
    return n;
}

void bridge_config_set_report(uint8_t report_id, uint8_t report_type,
                              const uint8_t *buffer, uint16_t bufsize)
{
    if (report_id != BRIDGE_CONFIG_CMD_REPORT_ID || buffer == NULL || bufsize == 0 ||
        report_type != HID_REPORT_TYPE_FEATURE)
        return;
    const uint8_t cmd = buffer[0];
    if (cmd == CFG_CMD_GET_INFO) {
        cfg_response_info();
    } else if (cmd == CFG_CMD_GET_CONFIG) {
        bridge_config_t cfg;
        bridge_config_get(&cfg);
        cfg_response_status(0);
        memcpy(&s_cfg_response[2], &cfg,
               sizeof cfg < sizeof s_cfg_response - 2 ? sizeof cfg : sizeof s_cfg_response - 2);
    } else if (cmd == CFG_CMD_SET_CONFIG && bufsize >= 1u + sizeof(bridge_config_t)) {
        bridge_config_t cfg;
        memcpy(&cfg, &buffer[1], sizeof cfg);
        if (bridge_config_set(&cfg)) cfg_response_status(0);
        else cfg_response_status(1);
    } else if (cmd == CFG_CMD_RESET) {
#if (defined(GENERIC_APPLE_IDENTITY) && !defined(APPLE_PERSISTENT_CONFIG)) || defined(CORSAIR_APPLE_IDENTITY) || defined(GENERIC_PID_PROBE) || defined(CHROMIUM_STADIA_PROBE)
        cfg_response_status(2); /* do not overwrite the normal Generic map */
#else
        bridge_config_t cfg;
        bridge_config_defaults(&cfg);
        bridge_config_set(&cfg);
        cfg_response_save_queued();
#endif
    } else if (cmd == CFG_CMD_SAVE) {
#if (defined(GENERIC_APPLE_IDENTITY) && !defined(APPLE_PERSISTENT_CONFIG)) || defined(CORSAIR_APPLE_IDENTITY) || defined(GENERIC_PID_PROBE) || defined(CHROMIUM_STADIA_PROBE)
        cfg_response_status(2);
#else
        cfg_response_save_queued();
#endif
    } else if (cmd == CFG_CMD_SET_GYRO_BIAS && bufsize >= 7u) {
        bridge_config_t cfg;
        bridge_config_get(&cfg);
        memcpy(cfg.gyro_bias, &buffer[1], sizeof cfg.gyro_bias);
        bridge_config_finalize(&cfg);
        bridge_config_set(&cfg);
        cfg_response_status(0);
    } else if (cmd == CFG_CMD_TEST_RUMBLE && bufsize >= 3u) {
        flydigi_host_rumble(buffer[1], buffer[2]);
        cfg_response_status(0);
    } else {
        cfg_response_status(2);
    }
    s_cfg_response[BRIDGE_CONFIG_TOKEN_OFFSET] =
        bufsize > BRIDGE_CONFIG_TOKEN_OFFSET ? buffer[BRIDGE_CONFIG_TOKEN_OFFSET] : 0;
    s_cfg_response[BRIDGE_CONFIG_TOKEN_OFFSET + 1] =
        bufsize > BRIDGE_CONFIG_TOKEN_OFFSET + 1 ? buffer[BRIDGE_CONFIG_TOKEN_OFFSET + 1] : 0;
}

static void config_task(void)
{
    if (s_cfg_save_pending) {
        s_cfg_save_pending = false;
        s_cfg_save_state = config_save_flash() ? 2 : 3;
    }
    /* A small asynchronous status packet makes a WebHID UI responsive without
     * polling the gamepad endpoint. */
    static uint32_t next_status_us;
    static uint32_t next_scan_us;
    static uint32_t last_buttons;
    static uint8_t last_rumble_left, last_rumble_right;
    uint32_t t = time_us_32();
    if ((int32_t)(t - next_scan_us) < 0) return;
    next_scan_us = t + 5000u;
    flydigi_rx_state_t rx;
    fd_host_status_t st;
    flydigi_host_get(&rx, &st, NULL);
    uint32_t const buttons = bridge_source_buttons(&rx);
    if (((int32_t)(t - next_status_us) < 0 && buttons == last_buttons &&
         st.rumble_ack_left == last_rumble_left &&
         st.rumble_ack_right == last_rumble_right) ||
        !tud_hid_n_ready(2)) return;
    uint8_t status[BRIDGE_CONFIG_REPORT_BYTES] = {0};
    status[0] = st.battery_valid ? st.battery_percent : 0xffu;
    status[1] = st.receiver_mounted ? 1u : 0u;
    status[2] = st.ext_frame_seen && st.ext_us_stale < FD_EXT_FRESH_US;
    status[3] = (uint8_t)(st.xinput_max_interval_us > 255u ? 255u : st.xinput_max_interval_us);
    status[4] = (uint8_t)(st.ext_max_interval_us > 255u ? 255u : st.ext_max_interval_us);
    status[5] = (uint8_t)(buttons >> 24);
    status[6] = st.rumble_ack_left;
    status[7] = st.rumble_ack_right;
    const uint32_t counters[] = {st.ext_frames, s_reports_sent, st.rumble_sent,
                                 st.rumble_fail, st.ext_us_stale};
    for (unsigned i = 0; i < sizeof counters / sizeof counters[0]; i++) {
        uint32_t value = counters[i];
        for (unsigned b = 0; b < 4; b++) status[8 + 4 * i + b] = (uint8_t)(value >> (8 * b));
    }
    status[28] = s_cfg_save_state;
    for (unsigned i = 0; i < 4; i++)
        status[29 + i] = (uint8_t)(s_cfg_save_seq >> (8 * i));
    for (unsigned b = 0; b < 3; b++)
        status[33 + b] = (uint8_t)(buttons >> (8 * b));
    for (unsigned i = 0; i < 4; i++)
        status[36 + i] = (uint8_t)(st.xinput_reports >> (8 * i));
    for (unsigned i = 0; i < 4; i++)
        status[40 + i] = (uint8_t)(st.ext_bad_checksum >> (8 * i));
    const uint32_t ages[] = {s_bridge_age_last_us, s_bridge_age_min_us,
                             s_bridge_age_max_us, s_bridge_age_samples};
    for (unsigned i = 0; i < sizeof ages / sizeof ages[0]; i++)
        for (unsigned b = 0; b < 4; b++)
            status[44 + 4 * i + b] = (uint8_t)(ages[i] >> (8 * b));
    for (unsigned b = 0; b < 3; b++)
        status[60 + b] = (uint8_t)(s_xinput_stick_reports >> (8 * b));
    if (tud_hid_n_report(2, BRIDGE_CONFIG_STATUS_REPORT_ID, status, sizeof status)) {
        last_buttons = buttons;
        last_rumble_left = st.rumble_ack_left;
        last_rumble_right = st.rumble_ack_right;
        next_status_us = t + 250000u;
        s_cfg_status_reports++;
    }
}

static void sensor_pack_s16(const int16_t values[3], uint8_t out[SENSOR_DESC_REPORT_BYTES])
{
    for (unsigned i = 0; i < 3; i++) {
        uint16_t v = (uint16_t)values[i];
        out[2 * i] = (uint8_t)v;
        out[2 * i + 1] = (uint8_t)(v >> 8);
    }
}

/* core1 自备栈（放主 RAM）：SDK 默认把两个栈塞进 4 KB 的 SCRATCH，core1 跑
 * TinyUSB host + PIO-USB 初始化实测溢出（探针抓到 sp 越过 __StackOneTop）。
 * 见 multicore_launch_core1_with_stack()。 */
#define CORE1_STACK_WORDS (16 * 1024 / 4)
static uint32_t s_core1_stack[CORE1_STACK_WORDS] __attribute__((aligned(8)));

/* ---- 运行时 PC 探针：core1 停住时看它到底在哪 ----
 * 报警器回调在中断上下文跑，把当前 PC/LR/SP 抓进 RAM，core0 读出来反查符号。 */
volatile uint32_t g_probe_pc, g_probe_lr, g_probe_sp, g_probe_n;
static repeating_timer_t s_probe_timer;

/* Keep resource-claim failures visible when they happen on core1.  The stock
 * panic path stops only the faulting core, so core0 can still service CDC. */
volatile uint32_t g_panic_n;
volatile uint32_t g_panic_core;
volatile uintptr_t g_panic_pc;
volatile char g_panic_fmt[96];

void __attribute__((noreturn)) brg_panic(const char *fmt)
{
    g_panic_core = get_core_num();
    g_panic_pc = (uintptr_t)__builtin_return_address(0);
    for (size_t i = 0; i + 1 < sizeof g_panic_fmt && fmt != NULL && fmt[i] != '\0'; i++) {
        g_panic_fmt[i] = fmt[i];
        g_panic_fmt[i + 1] = '\0';
    }
    g_panic_n++;
    while (true) tight_loop_contents();
}

static bool probe_cb(repeating_timer_t *rt)
{
    (void)rt;
    /* 关键：读“被中断打断的那段代码”的 PC，而不是中断处理器的 PC。
     * Cortex-M 进异常时硬件把 {r0,r1,r2,r3,r12,lr,pc,xpsr} 压到当前 SP 上，
     * 这里从 MSP 取回第 6 个字（pc）。报警器中断用的是 MSP。 */
    uint32_t msp;
    __asm volatile("mrs %0, msp" : "=r"(msp));
    const uint32_t *frame = (const uint32_t *)msp;
    g_probe_pc = frame[6];      /* 被打断处的 PC */
    g_probe_lr = frame[5];      /* 被打断处的 LR */
    g_probe_sp = msp;
    g_probe_n++;
    return true;
}

static void probe_start(void)
{
    /* 保持默认池（由 core1 的 SDK 惰性创建）：不主动抢报警器，避免和库冲突。
     * 2000 us 周期足够密，抓到的是“停住时”的 PC。 */
    add_repeating_timer_us(-2000, probe_cb, NULL, &s_probe_timer);
}
static volatile uint32_t s_core1_alive;   /* core1 一进函数就自增：判“有没有起来” */
static volatile uint32_t s_core1_ticks;   /* core1 循环心跳（诊断用） */
static volatile uint32_t s_core1_stage;   /* 1=进函数 2=tuh_configure 返回 3=tuh_init 返回 */
static volatile uint32_t s_core1_cycles;  /* tuh_task 返回次数 */

static void brg_log(const char *fmt, ...);   /* 前置声明：下面的环打印要用 */

/* ---- 诊断用：TinyUSB 日志的 RAM 环 + PIO-USB 库内部位置标记 ---- */
#define DBG_RING_SZ 16384
static volatile char     s_dbg_ring[DBG_RING_SZ];
static volatile uint32_t s_dbg_w;
volatile uint32_t g_dbg_pio[8];           /* 库内标记（pio_usb_host_init 各步骤）*/
/* Temporary host-side transaction counters.  Indexed by endpoint number
 * (EP81 is index 1, EP83 is index 3) so a NAK can be distinguished from a
 * data-toggle mismatch without a logic analyzer. */
volatile uint32_t g_pio_in_attempts[16];
volatile uint32_t g_pio_in_naks[16];
volatile uint32_t g_pio_in_mismatch[16];
volatile uint32_t g_pio_in_success[16];

int brg_tu_log_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int const n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && i < (int)sizeof buf; i++) {
        s_dbg_ring[s_dbg_w & (DBG_RING_SZ - 1)] = buf[i];
        s_dbg_w++;
    }
    return n;
}

static void log_tu_ring(void)
{
    uint32_t w = s_dbg_w;
    uint32_t start = (w > DBG_RING_SZ - 1) ? (w - (DBG_RING_SZ - 1)) : 0;
    if (!tud_cdc_connected()) return;
    char head[96];
    int hn = snprintf(head, sizeof head, "[TU] ring %lu B (from %lu):\n",
                      (unsigned long)w, (unsigned long)start);
    tud_cdc_write(head, (uint32_t)hn);
    for (uint32_t i = start; i < w; ) {
        char chunk[128];
        uint32_t n = 0;
        while (i < w && n < sizeof chunk) {
            char c = s_dbg_ring[i++ & (DBG_RING_SZ - 1)];
            if (c != 0) chunk[n++] = c;
        }
        if (n != 0) tud_cdc_write(chunk, n);
    }
    tud_cdc_write_flush();
    brg_log("[PIO] pool=%p dbg_pio = %lu %lu %lu %lu %lu %lu %lu %lu\n", (void *)s_host_alarm_pool,

            (unsigned long)g_dbg_pio[0], (unsigned long)g_dbg_pio[1], (unsigned long)g_dbg_pio[2],
            (unsigned long)g_dbg_pio[3], (unsigned long)g_dbg_pio[4], (unsigned long)g_dbg_pio[5],
            (unsigned long)g_dbg_pio[6], (unsigned long)g_dbg_pio[7]);
}

/* ===================== 日志（CDC，非阻塞） ===================== */
static void brg_log(const char *fmt, ...)
{
    if (!tud_cdc_connected()) return;
    if (tud_cdc_write_available() < 160) return;      /* 满了就丢，绝不阻塞 */
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int const n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        tud_cdc_write(buf, (uint32_t)(n < (int)sizeof buf ? n : (int)sizeof buf));
        tud_cdc_write_flush();
    }
}

/* ===================== R13 绕过：强拉低 D+/D- 200ms =====================
 * 依据：refs/Flydigi5Pico/Flydigi5Pico.cpp（“接收器可正常挂载的关键”）
 */
static void usb_force_reset_bus(void)
{
    gpio_init(PIN_USB_DP);
    gpio_set_dir(PIN_USB_DP, GPIO_OUT);
    gpio_init(PIN_USB_DM);
    gpio_set_dir(PIN_USB_DM, GPIO_OUT);
    gpio_set_drive_strength(PIN_USB_DP, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PIN_USB_DM, GPIO_DRIVE_STRENGTH_12MA);
    gpio_put(PIN_USB_DP, 0);
    gpio_put(PIN_USB_DM, 0);
    sleep_ms(200);
    gpio_set_dir(PIN_USB_DP, GPIO_IN);
    gpio_set_dir(PIN_USB_DM, GPIO_IN);
}

/* ===================== 状态行 ===================== */
static void log_status(void)
{
    fd_host_status_t st;
    flydigi_host_get(NULL, &st, NULL);
    brg_log("[IDLE] sent=%lu queuefail=%lu done=%lu last=%u result=%u\n",
            (unsigned long)st.hid_idle_sent, (unsigned long)st.hid_idle_queue_fail,
            (unsigned long)st.hid_idle_complete, st.hid_idle_last_itf,
            st.hid_idle_last_result);
    brg_log("[BRG] core1 alive=%lu stage=%lu ticks=%lu tuh_ret=%lu | mounts=%lu umounts=%lu(last daddr=%u) xi=%d ex=%d\n",
            (unsigned long)s_core1_alive, (unsigned long)s_core1_stage,
            (unsigned long)s_core1_ticks, (unsigned long)s_core1_cycles,
            (unsigned long)st.mounts, (unsigned long)st.umounts, st.last_umount_daddr,
            (int)st.xinput_up, (int)st.ext_up);
    brg_log("[BRG] recv=%d vid=%04x pid=%04x itf(x=%u e=%u) ep(xin=%02x/%u ein=%02x/%u eout=%02x/%u)\n",
         (int)st.receiver_mounted, st.vid, st.pid,
         st.xitf_num, st.ext_itf_num,
         st.xep_in, st.xep_in_size, st.ext_ep_in, st.ext_ep_in_size,
         st.ext_ep_out, st.ext_ep_out_size);
    brg_log("[BRG] xr=%lu xi_bad=%lu er=%lu ef=%lu ef_bad=%lu cmd=%lu fresh(ext)=%s stale_us=%lu sent=%lu\n",
         (unsigned long)st.xinput_reports, (unsigned long)st.xinput_invalid_buttons,
         (unsigned long)st.ext_reports,
         (unsigned long)st.ext_frames, (unsigned long)st.ext_bad_checksum,
         (unsigned long)st.cmds_sent,
            st.ext_us_stale != 0 && st.ext_us_stale < FD_EXT_FRESH_US ? "yes" : "no",
            (unsigned long)st.ext_us_stale, (unsigned long)s_reports_sent);
    brg_log("[POWER] valid=%d level=%u state=%u updates=%lu rumble=%lu/%lu spikes=%lu xi_dt=%lu..%lu ext_dt=%lu..%lu us\n",
            (int)st.battery_valid, st.battery_percent, st.battery_state,
            (unsigned long)st.battery_updates, (unsigned long)st.rumble_sent,
            (unsigned long)st.rumble_fail, (unsigned long)st.xinput_spikes_filtered,
            (unsigned long)st.xinput_min_interval_us,
            (unsigned long)st.xinput_max_interval_us, (unsigned long)st.ext_min_interval_us,
            (unsigned long)st.ext_max_interval_us);
    brg_log("[OPEN] xi=%lu/%lu epfail=%lu last=%u:%02x/%02x/%02x eps=%u | ext=%lu/%lu hid=%lu busy=%lu epfail=%lu sizefail=%lu last=%u:%02x/%02x/%02x eps=%u\n",
            (unsigned long)st.xi_open_calls, (unsigned long)st.xi_open_ok,
            (unsigned long)st.xi_open_ep_fail, st.xi_last_itf, st.xi_last_class,
            st.xi_last_subclass, st.xi_last_protocol, st.xi_last_eps,
            (unsigned long)st.ext_open_calls, (unsigned long)st.ext_open_ok,
            (unsigned long)st.ext_open_no_hid, (unsigned long)st.ext_open_busy,
            (unsigned long)st.ext_open_ep_fail, (unsigned long)st.ext_open_size_fail,
            st.ext_last_itf, st.ext_last_class, st.ext_last_subclass,
            st.ext_last_protocol, st.ext_last_eps);
    brg_log("[EP] e0=%u:%02x/%u,%02x/%u e1=%u:%02x/%u,%02x/%u e2=%u:%02x/%u,%02x/%u\n",
            st.ext_trace_itf[0], st.ext_trace_in_ep[0], st.ext_trace_in_size[0],
            st.ext_trace_out_ep[0], st.ext_trace_out_size[0],
            st.ext_trace_itf[1], st.ext_trace_in_ep[1], st.ext_trace_in_size[1],
            st.ext_trace_out_ep[1], st.ext_trace_out_size[1],
            st.ext_trace_itf[2], st.ext_trace_in_ep[2], st.ext_trace_in_size[2],
            st.ext_trace_out_ep[2], st.ext_trace_out_size[2]);
    brg_log("[XFER] xi sub=%lu claim=%lu fail=%lu cb=%lu ok=%lu reject=%lu busy=%d | ext sub=%lu claim=%lu fail=%lu cb=%lu ok=%lu out=%lu busy=%d | cmd claim=%lu fail=%lu\n",
            (unsigned long)st.xi_poll_submit, (unsigned long)st.xi_poll_claim_fail,
            (unsigned long)st.xi_poll_xfer_fail, (unsigned long)st.xi_xfer_cb,
            (unsigned long)st.xi_xfer_ok, (unsigned long)st.xi_reports_rejected,
            (int)st.xi_in_flight,
            (unsigned long)st.ext_poll_submit, (unsigned long)st.ext_poll_claim_fail,
            (unsigned long)st.ext_poll_xfer_fail, (unsigned long)st.ext_xfer_cb,
            (unsigned long)st.ext_xfer_ok, (unsigned long)st.ext_out_xfer_ok,
            (int)st.ext_in_flight, (unsigned long)st.cmd_claim_fail,
            (unsigned long)st.cmd_xfer_fail);
    brg_log("[IDLE] sent=%lu queuefail=%lu done=%lu last=%u result=%u\n",
            (unsigned long)st.hid_idle_sent, (unsigned long)st.hid_idle_queue_fail,
            (unsigned long)st.hid_idle_complete, st.hid_idle_last_itf,
            st.hid_idle_last_result);
    if (g_panic_n) {
        brg_log("[PANIC] n=%lu core=%lu pc=%08lx fmt=%s\n",
                (unsigned long)g_panic_n, (unsigned long)g_panic_core,
                (unsigned long)g_panic_pc, (const char *)g_panic_fmt);
    }
}

/* ===================== 桥：host 状态 → HID 报文 ===================== */
static void bridge_task(void)
{
    static uint32_t next_us;
    static uint32_t seen_input_generation;
    uint32_t const t = time_us_32();
    uint32_t const input_generation = flydigi_host_input_generation();
    bool const timer_due = (int32_t)(t - next_us) >= 0;
    if (!timer_due && input_generation == seen_input_generation) return;
    if (timer_due) next_us = t + HID_REPORT_INTERVAL_US;
    if (!tud_hid_ready()) return;

    flydigi_rx_state_t rx;
    bridge_input_sample_t sample;
    flydigi_host_get(&rx, NULL, &sample);

    uint8_t rep[VADER2PRO_REPORT_BYTES];
    bridge_pack(&rx, rep);
    seen_input_generation = input_generation;

    bool const sticks_changed = memcmp(rep, s_last_rep, 4) != 0;
    bool const other_changed = memcmp(rep + 4, s_last_rep + 4, sizeof rep - 4) != 0;
    bool const changed = sticks_changed || other_changed;
    if (!changed && (t - s_last_sent_us) < HID_KEEPALIVE_US) return;

#ifdef GENERIC_PID_PROBE
    uint8_t pid_rep[PID_PROBE_INPUT_BYTES];
    pid_probe_pack_input(rep, pid_rep);
    bool const sent = tud_hid_report(PID_PROBE_INPUT_REPORT_ID, pid_rep, sizeof pid_rep);
#elif defined(CHROMIUM_STADIA_PROBE)
    bool const sent = tud_hid_report(1, rep, sizeof rep);
#else
    bool const sent = tud_hid_report(0, rep, sizeof rep);
#endif
    if (sent) {
        memcpy(s_last_rep, rep, sizeof rep);
        vader2pro_set_last_report(rep);
        s_last_sent_us = t;
        s_reports_sent++;
        /* This is receiver callback to USB queue age, not radio or Mac latency. */
        uint32_t age_us;
        if (bridge_changed_age_us(time_us_32(), &sample, sticks_changed,
                                  other_changed, &age_us) && age_us < FD_EXT_FRESH_US) {
            s_bridge_age_last_us = age_us;
            if (s_bridge_age_samples == 0 || age_us < s_bridge_age_min_us)
                s_bridge_age_min_us = age_us;
            if (age_us > s_bridge_age_max_us) s_bridge_age_max_us = age_us;
            s_bridge_age_samples++;
        }
        if (sticks_changed && sample.xinput_overlay) s_xinput_stick_reports++;
        if (s_verbose && changed) {
            brg_log("[HID]");
            for (unsigned i = 0; i < sizeof rep; i++) brg_log(" %02x", rep[i]);
            brg_log("\n");
        }
    }
}

/* The standard Gamepad API has no gyro fields.  Keep the Vader2Pro gamepad
 * report intact and publish the IMU on a separate HID Sensor interface.
 * Alternate gyro/accelerometer reports at 500 Hz each so the sensor endpoint
 * remains interrupt-driven without reducing the gamepad's 1 kHz cadence. */
static void sensor_task(void)
{
    static uint32_t next_us;
    static bool gyro_turn;
    uint32_t const t = time_us_32();
    if ((int32_t)(t - next_us) < 0) return;
    next_us = t + HID_REPORT_INTERVAL_US;
    if (!tud_hid_n_ready(1)) return;

    flydigi_rx_state_t rx;
    bridge_input_sample_t sample;
    flydigi_host_get(&rx, NULL, &sample);
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    int16_t values[3];
    uint8_t report[SENSOR_DESC_REPORT_BYTES];
    uint8_t report_id;
    if (gyro_turn) {
        bridge_sensor_values(rx.gyro, &cfg, true, sample.sensor_us != 0, values);
        report_id = SENSOR_GYRO_REPORT_ID;
    } else {
        bridge_sensor_values(rx.accel, &cfg, false, sample.sensor_us != 0, values);
        report_id = SENSOR_ACCEL_REPORT_ID;
    }
    sensor_pack_s16(values, report);
    if (tud_hid_n_report(1, report_id, report, sizeof report)) {
        vader_sensor_set_last_report(report_id, report);
        gyro_turn = !gyro_turn;
    }
}

#if defined(APPLE_BATTERY_PROBE) || defined(APPLE_BATTERY_INTERFACE_PROBE)
/* Publish the receiver's last validated percentage as a standard HID Battery
 * Strength report.  It is deliberately low-rate and independent of the
 * 1 kHz gamepad report so it cannot perturb button or axis timing. */
static void battery_task(void)
{
    static uint32_t next_us;
    static uint8_t last_level;
    static bool have_level;
    uint32_t const t = time_us_32();
    bool ready;
#ifdef APPLE_BATTERY_INTERFACE_PROBE
    ready = tud_hid_n_ready(3);
#else
    ready = tud_hid_ready();
#endif
    if ((int32_t)(t - next_us) < 0 || !ready) return;
    next_us = t + 1000000u;
    fd_host_status_t st;
    flydigi_host_get(NULL, &st, NULL);
    if (st.battery_valid) {
        last_level = st.battery_percent;
        have_level = true;
    } else if (!have_level) {
        /* Do not publish 0% while the receiver is still completing its first
         * device-info query; HID Battery Strength has no explicit unknown
         * value. */
        return;
    }
    uint8_t const level = last_level;
#ifdef APPLE_BATTERY_INTERFACE_PROBE
    vader_battery_interface_set_last_report(level);
    (void)tud_hid_n_report(3, BATTERY_REPORT_ID, &level, BATTERY_REPORT_BYTES);
#else
    vader_battery_set_last_report(last_level);
    if (tud_hid_n_report(0, APPLE_BATTERY_REPORT_ID, &level,
                         APPLE_BATTERY_REPORT_BYTES))
        vader_battery_set_last_report(level);
#endif
}
#endif

/* ===================== 控制台 ===================== */
static void log_banner(void)
{
    brg_log("\n=== Flydigi Bridge (RP2350) ===\n");
    brg_log("identity: VID=%04x PID=%04x bcdDevice=%04x  HID report=%u B, %u bytes\n",
         BRIDGE_USB_VID, BRIDGE_USB_PID, BRIDGE_USB_BCD,
         (unsigned)sizeof(BRIDGE_GAMEPAD_DESC), VADER2PRO_REPORT_BYTES);
    brg_log("cmds: i=status q=PIO A=acquire U=release r=resend R=rumble x=raw e=events a=verbose n=reboot b=BOOTSEL\n");
}

static void log_raw(void)
{
    fd_host_status_t st;
    flydigi_rx_state_t merged;
    bridge_input_sample_t sample;
    flydigi_host_get(&merged, &st, &sample);
    brg_log("[RAW] last XInput report:");
    for (unsigned i = 0; i < sizeof st.last_xinput_raw; i++) brg_log(" %02x", st.last_xinput_raw[i]);
    brg_log("  buttons(bit10 guide)=%d\n", (int)(((uint16_t)st.last_xinput_raw[2] |
                                                    ((uint16_t)st.last_xinput_raw[3] << 8)) & (1u << 10)) != 0);
    if (st.xinput_dual_trigger_reports) {
        brg_log("[RAW] XInput dual trigger #%lu (EF LT=%u RT=%u age=%lu us):",
                st.xinput_dual_trigger_reports, st.last_dual_trigger_ext_lt,
                st.last_dual_trigger_ext_rt, st.last_dual_trigger_ext_age_us);
        for (unsigned i = 0; i < sizeof st.last_dual_trigger_xinput_raw; i++)
            brg_log(" %02x", st.last_dual_trigger_xinput_raw[i]);
        brg_log("\n");
    }
    brg_log("[RAW] last EF frame:");
    for (unsigned i = 0; i < FD_RX_FRAME_BYTES; i++) brg_log(" %02x", st.last_ext_raw[i]);
    brg_log("\n");
    uint32_t const raw_now = time_us_32();
    uint32_t const stick_age = sample.sticks_us ? raw_now - sample.sticks_us : 0;
    uint32_t const other_age = sample.other_us ? raw_now - sample.other_us : 0;
    uint32_t const sensor_age = sample.sensor_us ? raw_now - sample.sensor_us : 0;
    brg_log("[MERGED] b1=%02x b2=%02x ext1=%02x ext2=%02x guide=%d lt=%u rt=%u "
            "ages(stick=%lu other=%lu sensor=%lu) overlay=%d\n",
            merged.b1, merged.b2, merged.ext1, merged.ext2, (int)merged.guide,
            merged.lt, merged.rt, (unsigned long)stick_age,
            (unsigned long)other_age, (unsigned long)sensor_age,
            (int)sample.xinput_overlay);
    uint8_t merged_rep[VADER2PRO_REPORT_BYTES];
    bridge_pack(&merged, merged_rep);
    brg_log("[MERGED] HID:");
    for (unsigned i = 0; i < sizeof merged_rep; i++) brg_log(" %02x", merged_rep[i]);
    brg_log("\n");
    brg_log("[RAW] last rejected EF (EP%02x, %u B):", st.last_bad_ext_ep,
            st.last_bad_ext_len);
    for (unsigned i = 0; i < FD_RX_FRAME_BYTES; i++) brg_log(" %02x", st.last_bad_ext_raw[i]);
    brg_log("\n");
    brg_log("[RAW] last reply    :");
    for (unsigned i = 0; i < FD_RX_FRAME_BYTES; i++) brg_log(" %02x", st.last_reply_raw[i]);
    brg_log("\n");
    brg_log("[RAW] last HID rep :");
    for (unsigned i = 0; i < VADER2PRO_REPORT_BYTES; i++) brg_log(" %02x", s_last_rep[i]);
    brg_log("\n");
}

static void log_events(void)
{
    fd_input_event_t events[FD_INPUT_EVENT_CAPACITY];
    size_t const count = flydigi_host_get_events(events, FD_INPUT_EVENT_CAPACITY);
    brg_log("[EVENTS] last %u input changes (X=XInput, E=EF)\n", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        const fd_input_event_t *event = &events[i];
        brg_log("[%lu] %c t=%lu raw=%04x b=%02x/%02x ext=%02x/%02x home=%u "
                "lt/rt=%u/%u stick=%d,%d,%d,%d\n",
                (unsigned long)event->sequence,
                event->source == 0 ? 'X' : 'E',
                (unsigned long)event->time_us,
                event->raw_buttons, event->b1, event->b2,
                event->ext1, event->ext2, (unsigned)event->guide,
                (unsigned)event->lt, (unsigned)event->rt,
                (int)event->lx, (int)event->ly,
                (int)event->rx, (int)event->ry);
    }
}

static void log_pio_in(void)
{
    fd_host_status_t st;
    flydigi_host_get(NULL, &st, NULL);
    brg_log("[PIO-IN] ep81 a=%lu n=%lu m=%lu s=%lu | ep82 a=%lu n=%lu m=%lu s=%lu | ep83 a=%lu n=%lu m=%lu s=%lu | ep84 a=%lu n=%lu m=%lu s=%lu\n",
            (unsigned long)g_pio_in_attempts[1], (unsigned long)g_pio_in_naks[1],
            (unsigned long)g_pio_in_mismatch[1], (unsigned long)g_pio_in_success[1],
            (unsigned long)g_pio_in_attempts[2], (unsigned long)g_pio_in_naks[2],
            (unsigned long)g_pio_in_mismatch[2], (unsigned long)g_pio_in_success[2],
            (unsigned long)g_pio_in_attempts[3], (unsigned long)g_pio_in_naks[3],
            (unsigned long)g_pio_in_mismatch[3], (unsigned long)g_pio_in_success[3],
            (unsigned long)g_pio_in_attempts[4], (unsigned long)g_pio_in_naks[4],
            (unsigned long)g_pio_in_mismatch[4], (unsigned long)g_pio_in_success[4]);
    brg_log("[ACQ] sent=%lu replies=%lu state=%u reason=%u status=%lu enabled=%u\n",
            (unsigned long)st.acquire_sent, (unsigned long)st.acquire_replies,
            st.acquire_state, st.acquire_reason,
            (unsigned long)st.takeover_status_replies, st.takeover_enabled);
    brg_log("[HID-REPORT] sent=%lu queuefail=%lu done=%lu itf=%u result=%u\n",
            (unsigned long)st.hid_report_sent, (unsigned long)st.hid_report_queue_fail,
            (unsigned long)st.hid_report_complete, st.hid_report_last_itf,
            st.hid_report_last_result);
    brg_log("[HID-PROTO] sent=%lu queuefail=%lu done=%lu itf=%u result=%u\n",
            (unsigned long)st.hid_proto_sent, (unsigned long)st.hid_proto_queue_fail,
            (unsigned long)st.hid_proto_complete, st.hid_proto_last_itf,
            st.hid_proto_last_result);
    brg_log("[EPTRACE] e0=%u in=%02x/%u out=%02x/%u | e1=%u in=%02x/%u out=%02x/%u | e2=%u in=%02x/%u out=%02x/%u\n",
            st.ext_trace_itf[0], st.ext_trace_in_ep[0], st.ext_trace_in_size[0],
            st.ext_trace_out_ep[0], st.ext_trace_out_size[0],
            st.ext_trace_itf[1], st.ext_trace_in_ep[1], st.ext_trace_in_size[1],
            st.ext_trace_out_ep[1], st.ext_trace_out_size[1],
            st.ext_trace_itf[2], st.ext_trace_in_ep[2], st.ext_trace_in_size[2],
            st.ext_trace_out_ep[2], st.ext_trace_out_size[2]);
    brg_log("[XFER] xi=%lu/%lu ext=%lu/%lu out_ok=%lu cmd=%lu/%lu\n",
            (unsigned long)st.xi_xfer_cb, (unsigned long)st.xi_xfer_ok,
            (unsigned long)st.ext_xfer_cb, (unsigned long)st.ext_xfer_ok,
            (unsigned long)st.ext_out_xfer_ok,
            (unsigned long)st.cmd_claim_fail, (unsigned long)st.cmd_xfer_fail);
}

static void console_task(void)
{
    int32_t c;
    while ((c = tud_cdc_read_char()) >= 0) {
        switch ((char)c) {
        case 'i': log_status(); break;
        case 'r': flydigi_host_resend_cmds(); brg_log("[CMD] 命令序列已重排\n"); break;
        case 'A': flydigi_host_acquire_probe(true); brg_log("[CMD] acquire queued\n"); break;
        case 'U': flydigi_host_acquire_probe(false); brg_log("[CMD] release queued\n"); break;
        case 'R': flydigi_host_rumble(255, 255); brg_log("[CMD] rumble test queued\n"); break;
        case 'x': log_raw(); break;
        case 'e': log_events(); break;
        case 'q': log_pio_in(); break;
        case 't': log_tu_ring(); break;
        case 'p':
            brg_log("[PC] n=%lu pc=%08lx lr=%08lx sp=%08lx\n",
                    (unsigned long)g_probe_n, (unsigned long)g_probe_pc,
                    (unsigned long)g_probe_lr, (unsigned long)g_probe_sp);
            break;
        case 'a': s_verbose = !s_verbose; brg_log("[LOG] verbose=%d\n", (int)s_verbose); break;
        case 'v': brg_log("\n\n\n"); break;
        case 'b':
            brg_log("[CMD] reset to BOOTSEL\n");
            sleep_ms(50);
            reset_usb_boot(0, 0);
            break;
        case 'n':
            brg_log("[CMD] reboot application\n");
            sleep_ms(50);
            watchdog_reboot(0, 0, 0);
            break;
        default: break;
        }
    }
}

/* ===================== core1：host（PIO-USB） =====================
 * host 栈必须与自己的 task 同核；DP/DM 复位在 main 里已经做过了。
 */
static void host_init(void)
{
    s_core1_stage = 1;
    static pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_DP;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;
    /* 报警池预先在 core0 上建好再交给库：
     * 库默认 skip_alarm_pool=false 且 alarm_pool=NULL → 它会在 host 初始化的那个核上
     * 调 alarm_pool_create(2,1)。报警器 0/1/2 是每核共享的硬件资源，资源不可用时
     * SDK 走 hard_assert → panic → 复位（这就是 core1 写标记后又“消失”的机制）。
     * 官方示例跑在 RP2040 上没踩到，RP2350 上必须显式给池。 */
    if (!s_host_alarm_pool) s_host_alarm_pool = alarm_pool_create(2, 2);
    pio_cfg.alarm_pool = s_host_alarm_pool;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    s_core1_stage = 2;
    tuh_init(1);
    s_core1_stage = 3;
}

static void core1_host(void)
{
    s_core1_alive = 1;
    flash_safe_execute_core_init();
    probe_start();          /* 先起探针：卡在哪都能被看见 */
    host_init();
    s_core1_alive = 3;

    while (true) {
        /* 有界等待（最多 1 ms）：既能等事件，又保证循环心跳可观测。
         * 用阻塞版 tuh_task() 的话，没设备时它永远不返回，就分不清“空转”和“挂了”。 */
        tuh_task_ext(1000, false);
        s_core1_cycles++;
        flydigi_host_task();                           /* 1 kHz 轮询 + 发配置命令（端点操作须在本核） */
        s_core1_ticks++;
    }
}

/* ===================== core0：device（USB-C → Mac） ===================== */
int main(void)
{
    set_sys_clock_khz(120000, true);                   /* PIO-USB 要求 ≥120 MHz */
    flydigi_host_init();
    flash_safe_execute_core_init();
    bridge_config_t boot_cfg;
    bridge_config_defaults(&boot_cfg);
    bridge_config_set(&boot_cfg);
    (void)config_load_flash();
    cfg_response_info();

#ifndef SKIP_USB_FORCE_RESET
    usb_force_reset_bus();                             /* ← 接收器能挂载的前提 */
    sleep_ms(200);
#endif

#ifndef HOST_INIT_ON_CORE0
    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_host, s_core1_stack, sizeof s_core1_stack);
#endif

    static const tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tud_rhport_init(0, &dev_init);                     /* 与 tud_task 同核 */

#ifdef HOST_INIT_ON_CORE0
    /* 诊断实验：host 初始化改在 core0 上、开机 2.5 s 后再做。
     * 状态线在那一刻停住 = tuh_init 卡死坐实；状态线继续且 stage=3 = 卡死与核无关。 */
    static bool s_host_inited;
#endif

    uint32_t next_log_ms = 0;
    while (true) {
        tud_task_ext(0, false);                        /* 非阻塞（tud_task() 会等事件） */
#ifdef HOST_INIT_ON_CORE0
        if (!s_host_inited && to_ms_since_boot(get_absolute_time()) >= 2500) {
            s_host_inited = true;
            host_init();
        }
        if (s_host_inited) {
            tuh_task_ext(1000, false);
            s_core1_cycles++;
            flydigi_host_task();
            s_core1_ticks++;
        }
#endif
        bridge_task();
        sensor_task();
#if defined(APPLE_BATTERY_PROBE) || defined(APPLE_BATTERY_INTERFACE_PROBE)
        battery_task();
#endif
        config_task();
        console_task();

        uint32_t const ms = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(ms - next_log_ms) >= 0) {
            next_log_ms = ms + 2000;
            if (tud_cdc_connected()) {
                if (!s_banner_done) { s_banner_done = true; log_banner(); }
                log_status();
            }
        }
#ifdef BRINGUP_MS
        /* bring-up 诊断：一直没被主机挂上就退回 BOOTSEL —— 用来区分
         * “固件跑挂了”（什么都不会发生）与“描述符不被接受”（自己跳回 BOOTSEL）。 */
        if (!tud_mounted() && ms > BRINGUP_MS) reset_usb_boot(0, 0);
#endif
    }
    return 0;
}
