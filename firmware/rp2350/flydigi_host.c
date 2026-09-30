/* flydigi_host.c — 飞智 2.4G 接收器 host 侧实现
 *
 * 两个自定义 TinyUSB 类驱动，按描述符匹配（不信接口号）：
 *   A) vendor 0xFF/0x5D（Protocol 0x81 或 0x01）→ XInput 主通道，20B IN / 8B OUT
 *   B) HID 类且中断 IN 端点 32B              → 扩展/配置通道，32B IN + 32B OUT
 *
 * 关键点（都有一手依据，勿凭直觉改）：
 *   1. 挂载前必须做 DP/DM 强拉低复位（绕过板上 R13），否则接收器不挂载
 *      —— refs/Flydigi5Pico/Flydigi5Pico.cpp 的 usb_force_reset_bus()
 *   2. 不开 test mode 就永远只有固定状态帧，拿不到 5a a5 ef（docs/02 §7.2）
 *   3. 命令走 EP06 中断 OUT（Linux 参考驱动经 hidraw 写同一通道），并持续轮询 device-info
 *   4. 扩展键只在 EF 帧新鲜期内生效，避免断流后按键粘住
 */
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "flydigi_host.h"
#include "bridge_config.h"
#include "rumble_report.h"
#include "pico/critical_section.h"
#include <stdatomic.h>

#define FD_STARTUP_CMD_COUNT 6u  /* handshake x4 + status + test mode on */

volatile fd_host_status_t g_fd;
static _Atomic uint32_t s_input_generation;

/* ===================== 合成状态 ===================== */
static flydigi_rx_state_t s_base;      /* 接口 0（XInput） */
static flydigi_rx_state_t s_ext;       /* 接口 1（EF 帧） */
static uint32_t s_base_us;             /* 最后一次接口 0 有效报文 */
static uint32_t s_ext_us;              /* 最后一次 EF 帧 */
static critical_section_t s_state_lock;
static volatile bool s_resend_requested;
static bool s_acquire_pending, s_acquire_value;
static uint32_t s_retry_ms;
static uint8_t s_rumble_left, s_rumble_right;
static bool s_rumble_pending;
static uint8_t s_rumble_buf[8];
static uint32_t s_rumble_deadline_us;
static uint32_t s_rumble_last_sent_us;
static uint32_t s_rumble_retry_after_us;
static uint8_t s_rumble_last_left, s_rumble_last_right;
static uint32_t s_battery_us;
static fd_input_event_t s_input_events[FD_INPUT_EVENT_CAPACITY];
static flydigi_rx_state_t s_trace_previous[2];
static bool s_trace_valid[2];
static uint32_t s_trace_sequence;
static uint8_t s_trace_count;

static bool axis_moved(int16_t current, int16_t previous)
{
    int32_t const delta = (int32_t)current - previous;
    return delta >= 4096 || delta <= -4096;
}

/* Called with s_state_lock held.  Ignore IMU-only updates and stick jitter. */
static void trace_input(uint8_t source, const flydigi_rx_state_t *state,
                        uint16_t raw_buttons, uint32_t timestamp)
{
    const flydigi_rx_state_t *previous = &s_trace_previous[source];
    bool const changed = !s_trace_valid[source] ||
        state->b1 != previous->b1 || state->b2 != previous->b2 ||
        state->ext1 != previous->ext1 || state->ext2 != previous->ext2 ||
        state->guide != previous->guide || state->lt != previous->lt ||
        state->rt != previous->rt ||
        axis_moved(state->lx, previous->lx) || axis_moved(state->ly, previous->ly) ||
        axis_moved(state->rx, previous->rx) || axis_moved(state->ry, previous->ry);
    if (!changed) return;
    uint32_t const seq = s_trace_sequence++;
    fd_input_event_t *event = &s_input_events[seq % FD_INPUT_EVENT_CAPACITY];
    *event = (fd_input_event_t){
        .sequence = seq, .time_us = timestamp, .source = source,
        .b1 = state->b1, .b2 = state->b2, .ext1 = state->ext1,
        .ext2 = state->ext2, .guide = state->guide,
        .lt = state->lt, .rt = state->rt,
        .lx = state->lx, .ly = state->ly, .rx = state->rx, .ry = state->ry,
        .raw_buttons = raw_buttons,
    };
    s_trace_previous[source] = *state;
    s_trace_valid[source] = true;
    if (s_trace_count < FD_INPUT_EVENT_CAPACITY) s_trace_count++;
}

void flydigi_host_init(void)
{
    critical_section_init(&s_state_lock);
    s_rumble_pending = true;
}

/* ===================== 接口 0 ===================== */
typedef struct {
    uint8_t  daddr, itf, ep_in, ep_out;
    uint16_t ep_in_size, ep_out_size;
    bool     mounted, in_flight;
    uint32_t next_poll_us;
    uint8_t  in_buf[32];
} fd_xi_t;
static fd_xi_t s_xi;

/* ===================== HID 扩展接口 ===================== */
#define FD_MAX_EXT_IN 4u

typedef struct {
    uint8_t  itf, ep_in;
    uint16_t ep_in_size;
    bool     in_flight;
    uint32_t next_poll_us;
    /* Keep the full packet-sized buffer; useful reports are 32 bytes. */
    uint8_t  in_buf[64];
} fd_ext_in_t;

typedef struct {
    uint8_t  daddr, itf, itf_out, ep_out;
    uint16_t ep_out_size;
    bool     mounted, in_flight;
    uint8_t  in_count;
    fd_ext_in_t in[FD_MAX_EXT_IN];
    uint8_t  cmd_buf[FD_CMD_BYTES];
    uint8_t  cmd_idx;                  /* 已发出条数；>= FD_CMDS_COUNT 表示发完 */
    uint32_t cmd_next_ms;
} fd_ext_t;
static fd_ext_t s_ex;

static void fd_clear_device_state(void)
{
    critical_section_enter_blocking(&s_state_lock);
    memset(&s_base, 0, sizeof s_base);
    memset(&s_ext, 0, sizeof s_ext);
    s_base_us = 0;
    s_ext_us = 0;
    s_resend_requested = false;
    s_acquire_pending = false;
    s_acquire_value = false;
    s_retry_ms = 0;
    s_rumble_left = 0;
    s_rumble_right = 0;
    s_rumble_deadline_us = 0;
    s_rumble_last_sent_us = 0;
    s_rumble_retry_after_us = 0;
    s_rumble_last_left = 0;
    s_rumble_last_right = 0;
    /* Keep a zero packet queued so a receiver that latched rumble is stopped
     * after it comes back, even if it disappeared while the endpoint was busy. */
    s_rumble_pending = true;
    s_trace_valid[0] = s_trace_valid[1] = false;
    s_trace_count = 0;
    s_trace_sequence = 0;
    critical_section_exit(&s_state_lock);
    atomic_fetch_add_explicit(&s_input_generation, 1u, memory_order_release);

    memset(&s_xi, 0, sizeof s_xi);
    memset(&s_ex, 0, sizeof s_ex);
    memset((void *)g_fd.last_xinput_raw, 0, sizeof g_fd.last_xinput_raw);
    memset((void *)g_fd.last_dual_trigger_xinput_raw, 0,
           sizeof g_fd.last_dual_trigger_xinput_raw);
    g_fd.last_dual_trigger_ext_lt = 0;
    g_fd.last_dual_trigger_ext_rt = 0;
    g_fd.last_dual_trigger_ext_age_us = 0;
    g_fd.xinput_dual_trigger_reports = 0;
    memset((void *)g_fd.last_ext_raw, 0, sizeof g_fd.last_ext_raw);
    memset((void *)g_fd.last_bad_ext_raw, 0, sizeof g_fd.last_bad_ext_raw);
    g_fd.last_bad_ext_len = 0;
    g_fd.last_bad_ext_ep = 0;
    memset((void *)g_fd.last_reply_raw, 0, sizeof g_fd.last_reply_raw);
    s_battery_us = 0;
    g_fd.battery_valid = false;
    g_fd.battery_percent = 0;
    g_fd.battery_state = 0;
    g_fd.rumble_ack_left = 0;
    g_fd.rumble_ack_right = 0;
    g_fd.ext_frame_seen = false;
    g_fd.ext_us_stale = 0;
    g_fd.xitf_num = 0;
    g_fd.ext_itf_num = 0;
    g_fd.xep_in = 0;
    g_fd.ext_ep_in = 0;
    g_fd.ext_ep_out = 0;
    g_fd.xep_in_size = 0;
    g_fd.ext_ep_in_size = 0;
    g_fd.ext_ep_out_size = 0;
    g_fd.xi_in_flight = false;
    g_fd.ext_in_flight = false;
    memset((void *)g_fd.ext_trace_itf, 0, sizeof g_fd.ext_trace_itf);
    memset((void *)g_fd.ext_trace_in_ep, 0, sizeof g_fd.ext_trace_in_ep);
    memset((void *)g_fd.ext_trace_out_ep, 0, sizeof g_fd.ext_trace_out_ep);
    memset((void *)g_fd.ext_trace_in_size, 0, sizeof g_fd.ext_trace_in_size);
    memset((void *)g_fd.ext_trace_out_size, 0, sizeof g_fd.ext_trace_out_size);
}

/* ===================== 工具 ===================== */
static inline uint32_t now_us(void) { return time_us_32(); }
static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* 打开接口声明的全部端点 */
static bool fd_open_endpoints(uint8_t dev_addr, tusb_desc_interface_t const *itf, uint16_t max_len,
                              uint8_t *ep_in, uint16_t *in_size,
                              uint8_t *ep_out, uint16_t *out_size)
{
    *ep_in = 0; *ep_out = 0; *in_size = 0; *out_size = 0;
    /* The interface descriptor is the first descriptor in this range.  Start
     * with its successor; seeing an interface here means the endpoint list
     * for the current interface is already complete. */
    const uint8_t *p = tu_desc_next((const uint8_t *)itf);
    uint16_t pos = itf->bLength;
    uint8_t found = 0;
    while (pos < max_len && found < itf->bNumEndpoints) {
        uint8_t const type = tu_desc_type(p);
        if (type == TUSB_DESC_INTERFACE) break;          /* 走到下一个接口就收手 */
        if (type == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            if (!tuh_edpt_open(dev_addr, ep)) return false;
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                *ep_in = ep->bEndpointAddress; *in_size = tu_edpt_packet_size(ep);
            } else {
                *ep_out = ep->bEndpointAddress; *out_size = tu_edpt_packet_size(ep);
            }
            found++;
        }
        pos = (uint16_t)(pos + tu_desc_len(p));
        p = tu_desc_next(p);
    }
    return (*ep_in != 0);
}

/* ---------- 接口 0：解析 20 字节 XInput 报文（docs/02 §2） ---------- */
static void fd_parse_xinput(const uint8_t *r, uint32_t n)
{
    if (r == NULL || n < FD_XINPUT_REPORT_BYTES ||
        r[0] != 0x00 || r[1] != 0x14) return;
    uint32_t const raw_n = FD_XINPUT_REPORT_BYTES;
    memset((void *)g_fd.last_xinput_raw, 0, sizeof g_fd.last_xinput_raw);
    memcpy((void *)g_fd.last_xinput_raw, r, raw_n);
    const uint16_t btn = (uint16_t)(r[2] | (uint16_t)(r[3] << 8));
    uint8_t b1 = 0, b2 = 0;
    flydigi_xinput_normalize_buttons(btn, &b1, &b2);

    uint32_t const sample_us = now_us();
    g_fd.xinput_reports++;
    /* Keep the packet in last_xinput_raw for diagnostics, but reject an
     * undefined button bit before it reaches the merged state.  A real
     * receiver packet observed during physical key sampling was 0xff00;
     * bit 11 made it look like ABXY+LB/RB+Guide were all pressed. */
    if (!flydigi_xinput_buttons_valid(btn)) {
        g_fd.xinput_invalid_buttons++;
        return;
    }
    critical_section_enter_blocking(&s_state_lock);
    if (r[4] >= 128 && r[5] >= 128) {
        memcpy((void *)g_fd.last_dual_trigger_xinput_raw, r, FD_XINPUT_REPORT_BYTES);
        g_fd.last_dual_trigger_ext_lt = s_ext.lt;
        g_fd.last_dual_trigger_ext_rt = s_ext.rt;
        g_fd.last_dual_trigger_ext_age_us = s_ext_us ? sample_us - s_ext_us : 0;
        g_fd.xinput_dual_trigger_reports++;
    }
    if (flydigi_rx_xinput_idle_spike(r, n, &s_ext,
            s_ext_us != 0 && sample_us - s_ext_us <= 20000u)) {
        g_fd.xinput_spikes_filtered++;
        critical_section_exit(&s_state_lock);
        return;
    }
    if (s_base_us != 0) {
        uint32_t const dt = sample_us - s_base_us;
        if (g_fd.xinput_min_interval_us == 0 || dt < g_fd.xinput_min_interval_us)
            g_fd.xinput_min_interval_us = dt;
        if (dt > g_fd.xinput_max_interval_us) g_fd.xinput_max_interval_us = dt;
    }
    s_base.b1 = b1;
    s_base.b2 = b2;
    /* Keep the standard XInput Guide bit separate; this receiver's EF Home
     * polarity is resolved in flydigi_host_get() from the measured idle/held
     * transition instead of being assumed here. */
    s_base.guide = (btn & (1u << 10)) != 0;
    s_base.lt = r[4];
    s_base.rt = r[5];
    s_base.lx = (int16_t)(r[6] | (uint16_t)(r[7] << 8));
    s_base.ly = (int16_t)(r[8] | (uint16_t)(r[9] << 8));
    s_base.rx = (int16_t)(r[10] | (uint16_t)(r[11] << 8));
    s_base.ry = (int16_t)(r[12] | (uint16_t)(r[13] << 8));
    s_base_us = sample_us;
    trace_input(0, &s_base, btn, sample_us);
    atomic_fetch_add_explicit(&s_input_generation, 1u, memory_order_release);
    critical_section_exit(&s_state_lock);
}

/* ---------- driver A：XInput ---------- */
static bool fd_xi_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    (void)rhport;
    g_fd.xi_open_calls++;
    g_fd.xi_last_itf = itf->bInterfaceNumber;
    g_fd.xi_last_class = itf->bInterfaceClass;
    g_fd.xi_last_subclass = itf->bInterfaceSubClass;
    g_fd.xi_last_protocol = itf->bInterfaceProtocol;
    g_fd.xi_last_eps = itf->bNumEndpoints;
    if (itf->bInterfaceClass != 0xFF || itf->bInterfaceSubClass != 0x5D) return false;
    if (itf->bInterfaceProtocol != 0x81 && itf->bInterfaceProtocol != 0x01) return false;
    if (s_xi.mounted) return false;

    uint8_t ep_in = 0, ep_out = 0;
    uint16_t in_size = 0, out_size = 0;
    if (!fd_open_endpoints(dev_addr, itf, max_len, &ep_in, &in_size, &ep_out, &out_size)) {
        g_fd.xi_open_ep_fail++;
        return false;
    }

    s_xi.daddr = dev_addr;
    s_xi.itf = itf->bInterfaceNumber;
    s_xi.ep_in = ep_in; s_xi.ep_in_size = in_size;
    s_xi.ep_out = ep_out; s_xi.ep_out_size = out_size;
    s_xi.mounted = true;
    s_xi.in_flight = false;
    s_xi.next_poll_us = now_us();

    g_fd.xitf_num = itf->bInterfaceNumber;
    g_fd.xep_in = ep_in; g_fd.xep_in_size = in_size;
    return true;
}

static bool fd_xi_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    if (itf_num != s_xi.itf) return true;
    g_fd.xinput_up = true;
    g_fd.xi_open_ok++;
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool fd_xi_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    g_fd.xi_xfer_cb++;
    usbh_edpt_release(dev_addr, ep_addr);
    if (!s_xi.mounted) return true;
    if (ep_addr == s_xi.ep_out) {
        if (result == XFER_RESULT_SUCCESS) {
            g_fd.rumble_sent++;
            g_fd.rumble_ack_left = s_rumble_buf[3];
            g_fd.rumble_ack_right = s_rumble_buf[4];
        } else {
            g_fd.rumble_fail++;
            /* xfer() only queues the asynchronous transfer.  Keep the
             * request pending when the device reports a completion failure;
             * a newer request, if any, already owns the strength values. */
            critical_section_enter_blocking(&s_state_lock);
            s_rumble_pending = true;
            s_rumble_retry_after_us = now_us() + 4000u;
            critical_section_exit(&s_state_lock);
        }
    } else if (ep_addr == s_xi.ep_in) {
        s_xi.in_flight = false;
        if (result == XFER_RESULT_SUCCESS) g_fd.xi_xfer_ok++;
        if (result == XFER_RESULT_SUCCESS) {
            if (xferred_bytes >= FD_XINPUT_REPORT_BYTES &&
                s_xi.in_buf[0] == 0x00 && s_xi.in_buf[1] == 0x14) {
                fd_parse_xinput(s_xi.in_buf, xferred_bytes);
            } else {
                g_fd.xi_reports_rejected++;
            }
        }
    }
    g_fd.xi_in_flight = s_xi.in_flight;
    return true;
}

static void fd_xi_close(uint8_t dev_addr)
{
    (void)dev_addr;
    s_xi.mounted = false;
    g_fd.xinput_up = false;
}

/* ---------- driver B：扩展/配置通道 ---------- */
static bool fd_ex_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    (void)rhport;
    g_fd.ext_open_calls++;
    g_fd.ext_last_itf = itf->bInterfaceNumber;
    g_fd.ext_last_class = itf->bInterfaceClass;
    g_fd.ext_last_subclass = itf->bInterfaceSubClass;
    g_fd.ext_last_protocol = itf->bInterfaceProtocol;
    g_fd.ext_last_eps = itf->bNumEndpoints;
    if (itf->bInterfaceClass != TUSB_CLASS_HID) { g_fd.ext_open_no_hid++; return false; }

    uint8_t ep_in = 0, ep_out = 0;
    uint16_t in_size = 0, out_size = 0;
    if (!fd_open_endpoints(dev_addr, itf, max_len, &ep_in, &in_size, &ep_out, &out_size) ) {
        g_fd.ext_open_ep_fail++;
        return false;
    }
    uint32_t const trace = g_fd.ext_open_calls - 1u;
    if (trace < 4u) {
        g_fd.ext_trace_itf[trace] = itf->bInterfaceNumber;
        g_fd.ext_trace_in_ep[trace] = ep_in;
        g_fd.ext_trace_out_ep[trace] = ep_out;
        g_fd.ext_trace_in_size[trace] = in_size;
        g_fd.ext_trace_out_size[trace] = out_size;
    }
    /* The receiver exposes several HID interfaces (normally EP82/EP83/EP84).
     * Keep every interrupt IN endpoint alive: the stock TinyUSB HID driver
     * stores only one ep_in per interface, while this device's useful stream
     * and keyboard-like streams are separate interfaces. */
    if (in_size < FD_RX_FRAME_BYTES) {
        g_fd.ext_open_size_fail++;
        return false;
    }

    if (!s_ex.mounted) {
        s_ex.daddr = dev_addr;
        s_ex.itf = itf->bInterfaceNumber;
        s_ex.itf_out = 0xff;
        s_ex.ep_out = 0; s_ex.ep_out_size = 0;
        s_ex.mounted = true;
        s_ex.in_flight = false;
        s_ex.in_count = 0;
        s_ex.cmd_idx = 0;
        s_ex.cmd_next_ms = now_ms() + 50;      /* 挂载后先让设备稳一下再发命令 */

        g_fd.ext_itf_num = itf->bInterfaceNumber;
    }

    if (ep_out != 0 && s_ex.ep_out == 0) {
        s_ex.ep_out = ep_out;
        s_ex.ep_out_size = out_size;
        s_ex.itf_out = itf->bInterfaceNumber;
        g_fd.ext_itf_num = itf->bInterfaceNumber;
        g_fd.ext_ep_out = ep_out; g_fd.ext_ep_out_size = out_size;
    }

    if (s_ex.in_count >= FD_MAX_EXT_IN) {
        g_fd.ext_open_busy++;
        return false;
    }
    fd_ext_in_t *slot = &s_ex.in[s_ex.in_count++];
    slot->itf = itf->bInterfaceNumber;
    slot->ep_in = ep_in;
    slot->ep_in_size = in_size;
    slot->in_flight = false;
    slot->next_poll_us = now_us();
    if (g_fd.ext_ep_in == 0 || ep_out != 0) {
        g_fd.ext_ep_in = ep_in;
        g_fd.ext_ep_in_size = in_size;
    }
    g_fd.ext_open_ok++;
    return true;
}

/* HID interrupt endpoints on this receiver stay quiet until the host sends
 * the standard SET_IDLE request.  Keep it asynchronous so the USBH
 * configuration state machine can continue through both HID interfaces. */
static void fd_hid_report_complete(tuh_xfer_t *xfer)
{
    uint8_t const itf_num = (uint8_t)tu_le16toh(xfer->setup->wIndex);
    g_fd.hid_report_complete++;
    g_fd.hid_report_last_itf = itf_num;
    g_fd.hid_report_last_result = (uint8_t)xfer->result;
    usbh_driver_set_config_complete(xfer->daddr, itf_num);
}

static void fd_hid_protocol_complete(tuh_xfer_t *xfer)
{
    uint8_t const itf_num = (uint8_t)tu_le16toh(xfer->setup->wIndex);
    g_fd.hid_proto_complete++;
    g_fd.hid_proto_last_itf = itf_num;
    g_fd.hid_proto_last_result = (uint8_t)xfer->result;
    usbh_driver_set_config_complete(xfer->daddr, itf_num);
}

static bool fd_hid_set_protocol(uint8_t dev_addr, uint8_t itf_num)
{
    static tusb_control_request_t request;
    static tuh_xfer_t xfer;

    request = (tusb_control_request_t) {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_OUT,
        },
        .bRequest = 0x0b, /* HID_REQ_CONTROL_SET_PROTOCOL */
        .wValue = tu_htole16(1), /* Report protocol */
        .wIndex = tu_htole16(itf_num),
        .wLength = 0,
    };
    xfer = (tuh_xfer_t) {
        .daddr = dev_addr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = NULL,
        .complete_cb = fd_hid_protocol_complete,
        .user_data = 0,
    };
    g_fd.hid_proto_sent++;
    bool const queued = tuh_control_xfer(&xfer);
    if (!queued) g_fd.hid_proto_queue_fail++;
    return queued;
}

static void fd_hid_idle_complete(tuh_xfer_t *xfer)
{
    uint8_t const itf_num = (uint8_t)tu_le16toh(xfer->setup->wIndex);
    g_fd.hid_idle_complete++;
    g_fd.hid_idle_last_itf = itf_num;
    g_fd.hid_idle_last_result = (uint8_t)xfer->result;

    /* Match the reference host's per-interface setup sequence.  The first
     * HID interface gets its vendor SET_REPORT; the interface carrying the
     * useful EP06/EP83 channel is switched to Report protocol. */
    if (itf_num == s_ex.itf && itf_num != s_ex.itf_out) {
        static tusb_control_request_t report_request;
        static tuh_xfer_t report_xfer;
        static uint8_t const report_payload[10] = {
            0x1c, 0x00, 0x74, 0x07, 0x66,
            0x08, 0xdd, 0xff, 0xff, 0x00,
        };
        report_request = (tusb_control_request_t) {
            .bmRequestType_bit = {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type = TUSB_REQ_TYPE_CLASS,
                .direction = TUSB_DIR_OUT,
            },
            .bRequest = 0x09, /* HID_REQ_CONTROL_SET_REPORT */
            .wValue = tu_htole16((uint16_t)(0x0200u | (itf_num == s_ex.itf ? 1u : 0u))),
            .wIndex = tu_htole16(itf_num),
            .wLength = tu_htole16(sizeof report_payload),
        };
        report_xfer = (tuh_xfer_t) {
            .daddr = xfer->daddr,
            .ep_addr = 0,
            .setup = &report_request,
            .buffer = (void *)report_payload,
            .complete_cb = fd_hid_report_complete,
            .user_data = 0,
        };
        g_fd.hid_report_sent++;
        if (tuh_control_xfer(&report_xfer)) return;
        g_fd.hid_report_queue_fail++;
    }
    if (itf_num == s_ex.itf_out && itf_num != s_ex.itf) {
        if (fd_hid_set_protocol(xfer->daddr, itf_num)) return;
        /* A device may reject SET_PROTOCOL on a non-boot HID interface. */
    }
    usbh_driver_set_config_complete(xfer->daddr, itf_num);
}

static bool fd_hid_set_idle(uint8_t dev_addr, uint8_t itf_num)
{
    static tusb_control_request_t request;
    static tuh_xfer_t xfer;

    request = (tusb_control_request_t) {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_OUT,
        },
        .bRequest = 0x0a, /* HID_REQ_CONTROL_SET_IDLE */
        .wValue = 0,
        .wIndex = tu_htole16(itf_num),
        .wLength = 0,
    };
    xfer = (tuh_xfer_t) {
        .daddr = dev_addr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = NULL,
        .complete_cb = fd_hid_idle_complete,
        .user_data = 0,
    };
    g_fd.hid_idle_sent++;
    bool const queued = tuh_control_xfer(&xfer);
    if (!queued) g_fd.hid_idle_queue_fail++;
    return queued;
}

static bool fd_ex_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    bool claimed = (itf_num == s_ex.itf_out);
    for (uint8_t i = 0; i < s_ex.in_count; i++) {
        if (s_ex.in[i].itf == itf_num) {
            claimed = true;
            break;
        }
    }
    if (!claimed) return false;

    g_fd.ext_up = true;
    if (!fd_hid_set_idle(dev_addr, itf_num)) {
        /* A stalled SET_IDLE is allowed by HID; keep enumeration moving. */
        usbh_driver_set_config_complete(dev_addr, itf_num);
    }
    return true;
}

static bool fd_ex_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    g_fd.ext_xfer_cb++;
    usbh_edpt_release(dev_addr, ep_addr);
    if (!s_ex.mounted) return true;
    if (ep_addr == s_ex.ep_out) {
        if (result == XFER_RESULT_SUCCESS) g_fd.ext_out_xfer_ok++;
        return true;                                 /* OUT 完成：不做事 */
    }

    fd_ext_in_t *slot = NULL;
    for (uint8_t i = 0; i < s_ex.in_count; i++) {
        if (s_ex.in[i].ep_in == ep_addr) {
            slot = &s_ex.in[i];
            break;
        }
    }
    if (slot == NULL) return true;
    slot->in_flight = false;
    if (result == XFER_RESULT_SUCCESS) g_fd.ext_xfer_ok++;
    g_fd.ext_in_flight = false;
    if (result != XFER_RESULT_SUCCESS) return true;

    g_fd.ext_reports++;
    if (xferred_bytes >= FD_RX_FRAME_BYTES && slot->in_buf[0] == 0x5a &&
        slot->in_buf[1] == 0xa5 && slot->in_buf[2] != FD_RX_MAGIC2) {
        memcpy((void *)g_fd.last_reply_raw, slot->in_buf, FD_RX_FRAME_BYTES);
        if (slot->in_buf[2] == 0x1c) {
            g_fd.acquire_replies++;
            g_fd.acquire_state = slot->in_buf[5];
            g_fd.acquire_reason = slot->in_buf[6];
        }
        if (slot->in_buf[2] == 0x10) {
            g_fd.takeover_status_replies++;
            g_fd.takeover_enabled = slot->in_buf[9];
        }
        uint8_t battery_percent, power_state;
        if (flydigi_rx_battery_status(slot->in_buf, xferred_bytes,
                                      &battery_percent, &power_state)) {
            g_fd.battery_valid = true;
            g_fd.battery_percent = battery_percent;
            g_fd.battery_state = power_state;
            g_fd.battery_updates++;
            s_battery_us = now_us();
        }
    }
    if (xferred_bytes >= FD_RX_FRAME_BYTES &&
        slot->in_buf[0] == FD_RX_MAGIC0 && slot->in_buf[1] == FD_RX_MAGIC1 && slot->in_buf[2] == FD_RX_MAGIC2) {
        uint32_t const n = xferred_bytes < FD_RX_FRAME_BYTES ? xferred_bytes : FD_RX_FRAME_BYTES;
        flydigi_rx_state_t parsed;
        if (flydigi_rx_parse(slot->in_buf, n, &parsed)) {
            uint32_t const t = now_us();
            uint32_t const dt = t - s_ext_us;
            if (s_ext_us != 0 && dt < FD_EXT_FRESH_US) {
                if (g_fd.ext_min_interval_us == 0 || dt < g_fd.ext_min_interval_us)
                    g_fd.ext_min_interval_us = dt;
                if (dt > g_fd.ext_max_interval_us) g_fd.ext_max_interval_us = dt;
            }
            critical_section_enter_blocking(&s_state_lock);
            s_ext = parsed;
            s_ext_us = t;
            trace_input(1, &s_ext, (uint16_t)(parsed.b1 | (uint16_t)parsed.b2 << 8), t);
            atomic_fetch_add_explicit(&s_input_generation, 1u, memory_order_release);
            critical_section_exit(&s_state_lock);
            g_fd.ext_frames++;
            g_fd.ext_frame_seen = true;
            memcpy((void *)g_fd.last_ext_raw, slot->in_buf, FD_RX_FRAME_BYTES);
        } else {
            critical_section_enter_blocking(&s_state_lock);
            g_fd.ext_bad_checksum++;
            memcpy((void *)g_fd.last_bad_ext_raw, slot->in_buf, FD_RX_FRAME_BYTES);
            g_fd.last_bad_ext_len = (uint16_t)xferred_bytes;
            g_fd.last_bad_ext_ep = ep_addr;
            critical_section_exit(&s_state_lock);
        }
    }
    return true;
}

static void fd_ex_close(uint8_t dev_addr)
{
    if (dev_addr != s_ex.daddr) return;
    s_ex.mounted = false;
    s_ex.in_count = 0;
    g_fd.ext_up = false;
}

/* ---------- 驱动表 ---------- */
/* TinyUSB invokes every registered application driver's init callback during
 * tuh_rhport_init(), even when the driver has no per-stack state to prepare.
 * Keep this callback explicit instead of leaving it NULL. */
static bool fd_driver_init(void)
{
    return true;
}

static const usbh_class_driver_t FD_XI_DRIVER = {
    .name       = "FD-XInput",
    .init       = fd_driver_init,
    .deinit     = NULL,
    .open       = fd_xi_open,
    .set_config = fd_xi_set_config,
    .xfer_cb    = fd_xi_xfer_cb,
    .close      = fd_xi_close,
};

static const usbh_class_driver_t FD_EX_DRIVER = {
    .name       = "FD-Ext",
    .init       = fd_driver_init,
    .deinit     = NULL,
    .open       = fd_ex_open,
    .set_config = fd_ex_set_config,
    .xfer_cb    = fd_ex_xfer_cb,
    .close      = fd_ex_close,
};

usbh_class_driver_t const *flydigi_app_driver_get_cb(uint8_t *driver_count)
{
    static const usbh_class_driver_t table[2] = { FD_XI_DRIVER, FD_EX_DRIVER };
    /* ⚠️ 参考工程 Flydigi5Pico 此处写死 *driver_count = 1，导致第二个驱动从未注册、
     *    接口 1 的扩展通道永远打不开。这里必须是 2。 */
    *driver_count = 2;
    return table;
}

/* ---------- 挂载/卸载事件 ---------- */
void tuh_mount_cb(uint8_t daddr)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    g_fd.vid = vid;
    g_fd.pid = pid;
    g_fd.receiver_mounted = true;
    g_fd.mounts++;
}

void tuh_umount_cb(uint8_t daddr)
{
    g_fd.last_umount_daddr = daddr;
    g_fd.umounts++;
    g_fd.receiver_mounted = false;
    g_fd.xinput_up = false;
    g_fd.ext_up = false;
    fd_clear_device_state();
}

/* ---------- 轮询 ---------- */
static void fd_poll_ep(uint8_t dev_addr, uint8_t ep, uint8_t *buf, uint16_t len, bool *in_flight,
                       uint32_t *next_us, volatile uint32_t *submit_count,
                       volatile uint32_t *claim_fail, volatile uint32_t *xfer_fail,
                       volatile bool *status_in_flight)
{
    if (*in_flight) { *status_in_flight = true; return; }
    *status_in_flight = false;
    if ((int32_t)(now_us() - *next_us) < 0) return;
    *next_us = now_us() + 1000;                    /* 1 kHz */
    if (!usbh_edpt_claim(dev_addr, ep)) { (*claim_fail)++; return; }
    if (!usbh_edpt_xfer(dev_addr, ep, buf, len)) {
        usbh_edpt_release(dev_addr, ep);
        (*xfer_fail)++;
        return;
    }
    *in_flight = true;
    (*submit_count)++;
    *status_in_flight = true;
}

/* 依次发命令：4 条初始化握手 + 开 test mode */
static void fd_send_cmds(void)
{
    if (!s_ex.mounted) return;
    if (!s_ex.ep_out) { s_ex.cmd_idx = FD_STARTUP_CMD_COUNT; return; }   /* 没有 OUT 端点：放弃 */
    if (usbh_edpt_busy(s_ex.daddr, s_ex.ep_out)) return;
    bool acquire_pending, acquire_value;
    critical_section_enter_blocking(&s_state_lock);
    acquire_pending = s_acquire_pending;
    acquire_value = s_acquire_value;
    critical_section_exit(&s_state_lock);
    if (!acquire_pending || s_ex.cmd_idx < FD_STARTUP_CMD_COUNT) {
        if ((int32_t)(now_ms() - s_ex.cmd_next_ms) < 0) return;
    }
    if (!usbh_edpt_claim(s_ex.daddr, s_ex.ep_out)) { g_fd.cmd_claim_fail++; return; }

    memset(s_ex.cmd_buf, 0, FD_CMD_BYTES);
    if (acquire_pending && s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        /* SDL's unnumbered HID write omits its leading report selector 0x03. */
        static const uint8_t acquire_cmd[] = { 0x5a, 0xa5, 0x1c, 23, 0,
                                               'B', 'R', 'G' };
        memcpy(s_ex.cmd_buf, acquire_cmd, sizeof acquire_cmd);
        s_ex.cmd_buf[4] = acquire_value ? 1u : 0u;
    } else if (s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        /* Keep the battery fresh without turning the device-info request into
         * a 2 Hz latency spike.  The receiver stalls its input endpoints for
         * about 18 ms while handling this command, so after the first valid
         * reply it is enough to query every few seconds. */
        static const uint8_t poll_cmd[] = { 0x5a, 0xa5, 0x01, 0x02, 0x03 };
        memcpy(s_ex.cmd_buf, poll_cmd, sizeof poll_cmd);
    } else if (s_ex.cmd_idx == 4u) {
        /* Captured receiver startup sequence includes this status request
         * between config-data and the test-mode command. */
        static const uint8_t status_cmd[] = { 0x5a, 0xa5, 0x10, 0x02, 0x12 };
        memcpy(s_ex.cmd_buf, status_cmd, sizeof status_cmd);
    } else {
        uint8_t const table_idx = s_ex.cmd_idx > 4u ? 4u : s_ex.cmd_idx;
        flydigi_cmd_pack(&FD_CMDS[table_idx], s_ex.cmd_buf);
    }
    if (!usbh_edpt_xfer(s_ex.daddr, s_ex.ep_out, s_ex.cmd_buf, FD_CMD_BYTES)) {
        usbh_edpt_release(s_ex.daddr, s_ex.ep_out);
        g_fd.cmd_xfer_fail++;
        return;
    }
    if (acquire_pending && s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        critical_section_enter_blocking(&s_state_lock);
        if (s_acquire_pending && s_acquire_value == acquire_value)
            s_acquire_pending = false;
        critical_section_exit(&s_state_lock);
        g_fd.acquire_sent++;
        s_ex.cmd_next_ms = now_ms() + 1000u;
        g_fd.cmds_sent++;
        return;
    }
    if (s_ex.cmd_idx < FD_STARTUP_CMD_COUNT) s_ex.cmd_idx++;
    g_fd.cmds_sent++;
    if (s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        bool const battery_valid = g_fd.battery_valid;
        s_ex.cmd_next_ms = now_ms() +
            (battery_valid ? FD_DEVICE_INFO_POLL_MS : FD_DEVICE_INFO_RETRY_MS);
    } else {
        s_ex.cmd_next_ms = now_ms() + FD_CMD_GAP_MS;
    }
}

void flydigi_host_task(void)
{
    /* Do not queue interrupt transfers while TinyUSB is still configuring
     * later interfaces.  The reference host starts polling only after the
     * device-level mount callback has fired. */
    if (!g_fd.receiver_mounted) return;

    uint32_t const ms = now_ms();
    uint32_t ext_us_snapshot;
    bool resend_requested;
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    uint32_t const min_interval_us = (uint32_t)cfg.rumble_min_interval_ms * 1000u;
    uint32_t const now = now_us();
    critical_section_enter_blocking(&s_state_lock);
    ext_us_snapshot = s_ext_us;
    resend_requested = s_resend_requested;
    if (resend_requested) s_resend_requested = false;
    critical_section_exit(&s_state_lock);
    if (resend_requested || (s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT &&
        (ext_us_snapshot == 0 || now_us() - ext_us_snapshot > 1000000u) &&
        (int32_t)(ms - s_retry_ms) >= 0)) {
        s_ex.cmd_idx = 0;
        s_ex.cmd_next_ms = ms + FD_CMD_GAP_MS;
        s_retry_ms = ms + 2000;
    }

    critical_section_enter_blocking(&s_state_lock);
    uint32_t const t = now_us();
    if (s_rumble_deadline_us != 0 &&
        (int32_t)(t - s_rumble_deadline_us) >= 0 &&
        (s_rumble_left != 0 || s_rumble_right != 0)) {
        s_rumble_left = 0;
        s_rumble_right = 0;
        s_rumble_deadline_us = 0;
        s_rumble_pending = true;
    }
    bool const pending = s_rumble_pending;
    uint8_t const left = s_rumble_left, right = s_rumble_right;
    uint32_t const retry_after_us = s_rumble_retry_after_us;
    critical_section_exit(&s_state_lock);
    bool const interval_ok = flydigi_rumble_send_due(
        left, right, s_rumble_last_left, s_rumble_last_right,
        (uint32_t)(t - s_rumble_last_sent_us), min_interval_us,
        s_rumble_last_sent_us == 0, t, retry_after_us);
    if (pending && interval_ok && s_xi.mounted && s_xi.ep_out && !usbh_edpt_busy(s_xi.daddr, s_xi.ep_out)) {
        /* padctl and Flydigi5Pico use standard XInput rumble on EP05. */
        uint8_t const packet[8] = { 0, 8, 0, left, right, 0, 0, 0 };
        memcpy(s_rumble_buf, packet, sizeof packet);
        bool submitted = false;
        if (usbh_edpt_claim(s_xi.daddr, s_xi.ep_out)) {
            submitted = usbh_edpt_xfer(s_xi.daddr, s_xi.ep_out, s_rumble_buf, sizeof s_rumble_buf);
            if (!submitted) usbh_edpt_release(s_xi.daddr, s_xi.ep_out);
        }
        critical_section_enter_blocking(&s_state_lock);
        if (submitted && s_rumble_pending && s_rumble_left == left && s_rumble_right == right)
            s_rumble_pending = false;
        if (submitted) {
            s_rumble_last_sent_us = now;
            s_rumble_last_left = left;
            s_rumble_last_right = right;
            s_rumble_retry_after_us = 0;
        } else {
            s_rumble_last_sent_us = now;
            s_rumble_last_left = left;
            s_rumble_last_right = right;
            s_rumble_retry_after_us = now + 4000u;
        }
        critical_section_exit(&s_state_lock);
        if (!submitted) g_fd.rumble_fail++;
    }

    if (s_xi.mounted) {
        fd_poll_ep(s_xi.daddr, s_xi.ep_in, s_xi.in_buf, s_xi.ep_in_size, &s_xi.in_flight, &s_xi.next_poll_us,
                   &g_fd.xi_poll_submit, &g_fd.xi_poll_claim_fail, &g_fd.xi_poll_xfer_fail, &g_fd.xi_in_flight);
    }
    if (s_ex.mounted) {
        fd_send_cmds();
        for (uint8_t i = 0; i < s_ex.in_count; i++) {
            fd_ext_in_t *slot = &s_ex.in[i];
            fd_poll_ep(s_ex.daddr, slot->ep_in, slot->in_buf, slot->ep_in_size,
                       &slot->in_flight, &slot->next_poll_us,
                       &g_fd.ext_poll_submit, &g_fd.ext_poll_claim_fail,
                       &g_fd.ext_poll_xfer_fail, &g_fd.ext_in_flight);
        }
    }
    critical_section_enter_blocking(&s_state_lock);
    ext_us_snapshot = s_ext_us;
    critical_section_exit(&s_state_lock);
    g_fd.ext_us_stale = ext_us_snapshot != 0 ? now_us() - ext_us_snapshot : 0;
    if (s_battery_us != 0 && now_us() - s_battery_us > FD_BATTERY_FRESH_US)
        g_fd.battery_valid = false;
}

/* ---------- 重发命令 ---------- */
void flydigi_host_resend_cmds(void)
{
    critical_section_enter_blocking(&s_state_lock);
    s_resend_requested = true;
    critical_section_exit(&s_state_lock);
}

void flydigi_host_acquire_probe(bool acquire)
{
    critical_section_enter_blocking(&s_state_lock);
    s_acquire_value = acquire;
    s_acquire_pending = true;
    critical_section_exit(&s_state_lock);
}

void flydigi_host_rumble(uint8_t left, uint8_t right)
{
    bridge_config_rumble(left, right, &left, &right);
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    critical_section_enter_blocking(&s_state_lock);
    s_rumble_left = left;
    s_rumble_right = right;
    uint32_t watchdog_us = (uint32_t)cfg.rumble_watchdog_ms * 1000u;
    s_rumble_deadline_us = (left != 0 || right != 0) ? now_us() + watchdog_us : 0;
    /* Repeated identical browser reports are common.  They still refresh the
     * watchdog, but a wire packet is emitted at the configured minimum rate. */
    s_rumble_pending = s_rumble_pending || left != s_rumble_last_left ||
                       right != s_rumble_last_right || s_rumble_last_sent_us == 0;
    critical_section_exit(&s_state_lock);
}

/* ---------- 合成输出 ---------- */
void flydigi_host_get(flydigi_rx_state_t *out, fd_host_status_t *out_st,
                      bridge_input_sample_t *sample)
{
    uint32_t const t = now_us();
    flydigi_rx_state_t r;
    bool fresh_xinput_sticks = false;
    if (out != NULL || sample != NULL) {
        bridge_config_t cfg;
        bridge_config_get(&cfg);
        fresh_xinput_sticks = (cfg.flags & BRIDGE_FLAG_FRESH_XINPUT_STICKS) != 0;
    }

    critical_section_enter_blocking(&s_state_lock);

    bool const base_fresh = s_base_us != 0 && (t - s_base_us) < FD_EXT_FRESH_US;
    bool const ext_fresh  = s_ext_us  != 0 && (t - s_ext_us)  < FD_EXT_FRESH_US;
    bool const base_sticks = flydigi_rx_merge(&s_base, base_fresh, t - s_base_us,
                                               &s_ext, ext_fresh, t - s_ext_us,
                                               fresh_xinput_sticks, &r);
    if (sample) {
        sample->sticks_us = base_sticks ? s_base_us :
                            ext_fresh ? s_ext_us : base_fresh ? s_base_us : 0;
        sample->other_us = ext_fresh ? s_ext_us : base_fresh ? s_base_us : 0;
        sample->sensor_us = ext_fresh ? s_ext_us : 0;
        sample->xinput_overlay = base_sticks;
    }
    if (out_st) *out_st = g_fd;
    critical_section_exit(&s_state_lock);
    if (out) *out = r;
}

uint32_t flydigi_host_input_generation(void)
{
    return atomic_load_explicit(&s_input_generation, memory_order_acquire);
}

size_t flydigi_host_get_events(fd_input_event_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) return 0;
    critical_section_enter_blocking(&s_state_lock);
    size_t count = s_trace_count < capacity ? s_trace_count : capacity;
    uint32_t const start = s_trace_sequence - (uint32_t)count;
    for (size_t i = 0; i < count; i++)
        out[i] = s_input_events[(start + (uint32_t)i) % FD_INPUT_EVENT_CAPACITY];
    critical_section_exit(&s_state_lock);
    return count;
}

bool flydigi_host_ready(void)
{
    uint32_t const t = now_us();
    critical_section_enter_blocking(&s_state_lock);
    bool const ready = s_xi.mounted && s_base_us != 0 &&
                       (t - s_base_us) < FD_EXT_FRESH_US;
    critical_section_exit(&s_state_lock);
    return ready;
}
