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
#include "flydigi_cmd.h"
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
static bool s_acquire_auto_done;            /* 开机自动声明接管（高帧率流）只做一次 */
static bool s_sleep_auto_done;              /* 每挂载自愈一次（读到非 0 才写） */
static bool s_gcfg_auto_done;               /* 每挂载自动读一次全局配置 */
static uint32_t s_acquire_keepalive_ms;     /* 上次 acquire 发送时刻（30s 保活） */
static volatile bool s_gcfg_pending;        /* 按需启动全局配置读取（0xA3 读 RAM1） */
static bool s_gcfg_active;                  /* 读取进行中 */
static bool s_gcfg_waiting;                 /* 有分块请求在途 */
static uint8_t s_gcfg_chunk;                /* 下一分块索引 */
static uint8_t s_gcfg_buf[96];              /* 6×16B 拼装缓冲（覆盖 82B 需求） */
static uint32_t s_gcfg_last_ms;             /* 超时基准 */
static uint32_t s_gcfg_read_ms;             /* 上次读取完成时刻（A1 推送自动重读的速率限制） */
static volatile bool s_gcfg_stale;          /* A1 推送：设备端配置已变化，待重读 */
static bool s_fp_awaiting_reply;            /* 刚发过 0xA1 查询（区分应答与推送） */
static bool s_acquire_pending, s_acquire_value;
static uint32_t s_retry_ms;
static uint8_t s_recover_streak;            /* 连续断流恢复次数（输入恢复时归零，用于退避） */
static uint8_t s_poll_streak;               /* 连续未回应的设备信息轮询数（收到 01 应答归零） */
static uint32_t s_dev_info_last_ms;         /* 上次收到 01 应答时刻（上线检测用，0=从未） */
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

/* ===================== 命令帧队列 =====================
 * 写入类命令（0xA0/0x0F/0xA1/0xA2/0xA3 元数据/0xA7/0xAF/0x34/0x11）统一先建帧入队，
 * 启动序列完成后由 fd_send_cmds 按 FIFO 发出；满则丢弃并 g_fd.q_drops++。
 * 队列由 core1 的发送路径消费，但 push 也可能来自 core0（控制台/WebHID），
 * 故 push/pop/depth 沿用 s_state_lock（与 s_acquire_* 相同模式），避免跨核撕裂帧。 */
#define FD_CMDQ_DEPTH 8u

/* 发送提交前按 tag 施加的副作用（与旧发送分支“分支内先置副作用再提交”顺序一致） */
enum {
    TAG_NONE = 0,
    TAG_FP_QUERY,       /* 0xA1 指纹查询：置 s_fp_awaiting_reply（区分应答与推送） */
    TAG_SLOT_SWITCH,    /* 0xA2 切槽：slot_switches++ */
    TAG_CALIB_START,    /* 0x34 开始：calib_starts++ */
    TAG_CALIB_FINISH,   /* 0x34 结束：calib_finishes++ */
    TAG_STATUS_SET,     /* 0x11 SET_STATUS：status_set_sent++ */
};

typedef struct {
    uint8_t frame[FD_CMD_BYTES];
    uint8_t tag;
} fd_cmdq_entry_t;

typedef struct {
    fd_cmdq_entry_t e[FD_CMDQ_DEPTH];
    uint8_t head;                   /* 出队位置 */
    uint8_t count;                  /* 当前元素数 */
} fd_cmdq_t;

static fd_cmdq_t s_cmdq;

static bool fd_cmdq_push(const uint8_t frame[FD_CMD_BYTES], uint8_t tag)
{
    critical_section_enter_blocking(&s_state_lock);
    bool const ok = s_cmdq.count < FD_CMDQ_DEPTH;
    if (ok) {
        fd_cmdq_entry_t *e = &s_cmdq.e[(s_cmdq.head + s_cmdq.count) % FD_CMDQ_DEPTH];
        memcpy(e->frame, frame, FD_CMD_BYTES);
        e->tag = tag;
        s_cmdq.count++;
    } else {
        g_fd.q_drops++;
    }
    critical_section_exit(&s_state_lock);
    return ok;
}

static bool fd_cmdq_pop(uint8_t out[FD_CMD_BYTES], uint8_t *tag)
{
    critical_section_enter_blocking(&s_state_lock);
    bool const ok = s_cmdq.count > 0;
    if (ok) {
        fd_cmdq_entry_t const *e = &s_cmdq.e[s_cmdq.head];
        memcpy(out, e->frame, FD_CMD_BYTES);
        if (tag != NULL) *tag = e->tag;
        s_cmdq.head = (uint8_t)((s_cmdq.head + 1u) % FD_CMDQ_DEPTH);
        s_cmdq.count--;
    }
    critical_section_exit(&s_state_lock);
    return ok;
}

static uint32_t fd_cmdq_depth(void)
{
    critical_section_enter_blocking(&s_state_lock);
    uint32_t const n = s_cmdq.count;
    critical_section_exit(&s_state_lock);
    return n;
}

uint32_t flydigi_host_cmdq_depth(void)
{
    return fd_cmdq_depth();
}

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
    s_recover_streak = 0;
    s_poll_streak = 0;
    s_dev_info_last_ms = 0;
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
    /* 断连时丢弃排队未发的写入命令（等价于旧实现清零全部 pending 标志）。 */
    s_cmdq.head = 0;
    s_cmdq.count = 0;
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
    g_fd.dev_info_valid = false;
    s_acquire_auto_done = false;
    s_sleep_auto_done = false;
    s_gcfg_auto_done = false;
    s_gcfg_pending = false;
    s_gcfg_active = false;
    s_gcfg_waiting = false;
    s_gcfg_stale = false;
    s_fp_awaiting_reply = false;
    g_fd.sku.valid = false;
    g_fd.fp.valid = false;
    g_fd.meta.replies = 0;
    g_fd.dev_ext.valid = false;
    g_fd.gcfg_valid = false;
    g_fd.gcfg_reads = 0;
    g_fd.gcfg_errors = 0;
    g_fd.gcfg_stale_replies = 0;
    g_fd.a0_replies = 0;
    g_fd.a0_last_status = 0;
    g_fd.a0_last_field = 0;
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

/* ---------- 应答分发：5a a5 <cmd> …（逐条迁移自旧 if 链） ---------- */

/* 0x01 设备信息（型号/连接/固件）与电量 nibble（后者自带 cmd 校验）。 */
static void fd_reply_01(const uint8_t *b, uint32_t n)
{
    (void)n;
    flydigi_dev_info_t ex;
    if (flydigi_rx_device_info_ex(b, FD_RX_FRAME_BYTES, &ex)) {
        g_fd.dev_ext.ex = ex;
        g_fd.dev_ext.valid = true;
        g_fd.dev_ext.replies++;
        s_poll_streak = 0;              /* 收到 01 应答：轮询退避归零 */
        /* 手柄重新上线检测（2026-10-18）：长时间无 01 应答后重新收到 →
         * 重置恢复退避并立即安排一次快速恢复（重发启动序列 test-mode + acquire），
         * 同时补一次配置重读。避免退避到 30s 后手柄回归要等半分钟才恢复 EF。 */
        uint32_t const now = now_ms();
        if (s_dev_info_last_ms != 0 &&
            (int32_t)(now - s_dev_info_last_ms) > 10000) {
            s_recover_streak = 0;
            s_retry_ms = 0;
            s_gcfg_stale = true;
        }
        s_dev_info_last_ms = now;
        /* 旧扁平字段由全量解析同步填充（同字节位：model[5]/conn[6]/fw[15..16]）。 */
        g_fd.dev_model = ex.model;
        g_fd.dev_connection = ex.connection;
        g_fd.dev_fw_version = ex.fw_main;
        g_fd.dev_info_valid = true;
    }
    uint8_t battery_percent, power_state;
    if (flydigi_rx_battery_status(b, n, &battery_percent, &power_state)) {
        g_fd.battery_valid = true;
        g_fd.battery_percent = battery_percent;
        g_fd.battery_state = power_state;
        g_fd.battery_updates++;
        s_battery_us = now_us();
    }
}

/* 0x0F SKU 名称应答（官方 Rn 同布局）：payload[0]=value，[1..20]=名称。 */
static void fd_reply_0f(const uint8_t *b)
{
    uint8_t val;
    if (flydigi_rx_sku(b, FD_RX_FRAME_BYTES, &val, (char *)g_fd.sku.name)) {
        g_fd.sku.value = val;
        g_fd.sku.valid = true;
        g_fd.sku.replies++;
    }
}

/* 0x10 接管状态应答：takeover_enabled。 */
static void fd_reply_10(const uint8_t *b)
{
    g_fd.takeover_status_replies++;
    g_fd.takeover_enabled = b[9];
}

/* 0x1C acquire 应答：state/reason。 */
static void fd_reply_1c(const uint8_t *b)
{
    g_fd.acquire_replies++;
    g_fd.acquire_state = b[5];
    g_fd.acquire_reason = b[6];
}

/* 0xA0 字段写应答（v1 布局）：5A A5 A0 pkgCount pkgIndex status fieldId CS */
static void fd_reply_a0(const uint8_t *b)
{
    g_fd.a0_replies++;
    g_fd.a0_last_status = b[5];
    g_fd.a0_last_field = b[6];
}

/* 0xA1 配置指纹：活动槽 + 4×槽 CRC16 + 全局 CRC16。
 * 查询应答与「设备端配置变更」主动推送共用本命令（官方 Dn 监听同名命令），
 * 靠 s_fp_awaiting_reply 区分。 */
static void fd_reply_a1(const uint8_t *b)
{
    uint8_t aslot;
    uint16_t crcs[4], gcrc;
    if (flydigi_rx_fingerprint(b, FD_RX_FRAME_BYTES, &aslot, crcs, &gcrc)) {
        g_fd.fp.valid = true;
        g_fd.fp.active_slot = aslot;
        for (unsigned i = 0; i < 4u; i++) g_fd.fp.slot_crc[i] = crcs[i];
        g_fd.fp.global_crc = gcrc;
        g_fd.fp.replies++;
        if (s_fp_awaiting_reply) {
            s_fp_awaiting_reply = false;   /* 我方查询的应答 */
        } else {
            /* 主动推送：设备端配置已变化 → 触发自动重读 */
            g_fd.fp.push_count++;
            s_gcfg_stale = true;
        }
    }
}

/* 0xA2 切槽应答：与指纹同布局，单独计数。 */
static void fd_reply_a2(const uint8_t *b)
{
    uint8_t aslot;
    uint16_t crcs[4], gcrc;
    if (flydigi_rx_fingerprint(b, FD_RX_FRAME_BYTES, &aslot, crcs, &gcrc)) {
        g_fd.fp.valid = true;
        g_fd.fp.active_slot = aslot;
        for (unsigned i = 0; i < 4u; i++) g_fd.fp.slot_crc[i] = crcs[i];
        g_fd.fp.global_crc = gcrc;
        g_fd.fp.slot_replies++;
    }
}

/* 0xA3：全局配置分块续读（读取中）与子操作 1 RAM 元数据。
 * 旧实现两个 if 都可命中（元数据帧在读配置期间到达会先记一次 gcfg_errors），
 * 这里保持同序：先分块状态机，再元数据解析。 */
static void fd_reply_a3(const uint8_t *b)
{
    if (s_gcfg_active && s_gcfg_waiting) {
        /* v2.1 应答：payload 自 [3] 起 = subop,status,ramId,activeSlot,
         * totalLen.u16,chunkIdx,chunkSize,data…（官方 core pt/te 布局）。
         * 健壮性（2026-10-18）：只对“期望块号”做真错误判定；陈旧/重复/越序的
         * 分块应答（接收器重复转发或上一轮读取的遗留）静默忽略并计数，等待中的
         * 真应答不受影响；2s 超时是最终兜底。其他子操作（如元数据）不触碰读取状态机。 */
        uint8_t const subop = b[3];
        if (subop == 0x00) {
            uint8_t const status = b[4];
            uint8_t const cidx = b[9];
            uint8_t const csize = b[10];
            if (cidx == s_gcfg_chunk) {
                if (status == 0 && csize <= 16u) {
                    s_gcfg_waiting = false;
                    memcpy(&s_gcfg_buf[(uint32_t)cidx * 16u], &b[11], csize);
                    s_gcfg_last_ms = now_ms();
                    s_gcfg_chunk++;
                    uint32_t const got = (uint32_t)cidx * 16u + csize;
                    if (got >= 82u) {           /* 已覆盖 slice(2,82) 所需 80B 数据 */
                        flydigi_gcfg_t parsed;
                        if (flydigi_gcfg_parse(&s_gcfg_buf[2], &parsed)) {
                            g_fd.gcfg = parsed;
                            g_fd.gcfg_valid = true;
                            g_fd.gcfg_reads++;
                            /* 自愈：睡眠定时器非 0 → 自动写一次“永不”（每挂载一次） */
                            if (!s_sleep_auto_done && parsed.sleep_minutes != 0) {
                                s_sleep_auto_done = true;
                                flydigi_host_request_field(FD_FIELD_SLEEP, 0);
                            }
                        } else {
                            g_fd.gcfg_errors++;
                        }
                        s_gcfg_read_ms = now_ms();
                        s_gcfg_active = false;
                    } else if (s_gcfg_chunk >= 6u) {
                        g_fd.gcfg_errors++;     /* 数据不足 82B */
                        s_gcfg_read_ms = now_ms();
                        s_gcfg_active = false;
                    }
                } else {
                    s_gcfg_waiting = false;
                    g_fd.gcfg_errors++;         /* 期望块的错误状态 */
                    s_gcfg_read_ms = now_ms();
                    s_gcfg_active = false;
                }
            } else {
                /* 陈旧/重复/越序分块：忽略（保持 waiting，等真应答或超时）。 */
                g_fd.gcfg_stale_replies++;
            }
        }
    }
    if (b[3] == 0x01) {
        /* RAM 元数据（子操作 1）：状态/ramId/槽/长度/CRC32。 */
        uint8_t mstatus, mram, mslot;
        uint16_t mlen;
        uint32_t mcrc;
        if (flydigi_rx_ram_meta(b, FD_RX_FRAME_BYTES, &mstatus, &mram, &mslot, &mlen, &mcrc)) {
            g_fd.meta.status = mstatus;
            g_fd.meta.ram = mram;
            g_fd.meta.slot = mslot;
            g_fd.meta.len = mlen;
            g_fd.meta.crc32 = mcrc;
            g_fd.meta.replies++;
        }
    }
}

/* 0xA7 保存 / 0xAF 重置 / 0x34 校准应答：仅计数。 */
static void fd_reply_a7(void) { g_fd.ops.save_replies++; }
static void fd_reply_af(void) { g_fd.ops.reset_replies++; }
static void fd_reply_34(void) { g_fd.ops.calib_replies++; }

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
        switch (slot->in_buf[2]) {
        case 0x01: fd_reply_01(slot->in_buf, xferred_bytes); break;
        case 0x0f: fd_reply_0f(slot->in_buf); break;
        case 0x10: fd_reply_10(slot->in_buf); break;
        case 0x1c: fd_reply_1c(slot->in_buf); break;
        case 0xa0: fd_reply_a0(slot->in_buf); break;
        case 0xa1: fd_reply_a1(slot->in_buf); break;
        case 0xa2: fd_reply_a2(slot->in_buf); break;
        case 0xa3: fd_reply_a3(slot->in_buf); break;
        case 0xa7: fd_reply_a7(); break;
        case 0xaf: fd_reply_af(); break;
        case 0x34: fd_reply_34(); break;
        default: break;             /* 0x03（硬件状态）已随死代码移除 */
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
            s_recover_streak = 0;       /* 输入恢复：重置断流恢复退避 */
            memcpy((void *)g_fd.last_ext_raw, slot->in_buf, FD_RX_FRAME_BYTES);
            /* 拼接帧嫌疑检测（2026-10-18 发现）：接收器偶发把 XInput 载荷拼进
             * EF 帧；错位版被校验拒绝，对齐版会漏过校验。这里只做识别与计数：
             *  a) 零 IMU：校验通过的帧但陀螺仪+加速度全零（真机不可能全零）；
             *  b) 摇杆吻合：EF [13..20] 四 int16 近似等于最近 XInput 摇杆值。 */
            {
                const uint8_t *b = slot->in_buf;
                bool imu_zero = true;
                for (unsigned i = 17; i < 29u; i++) {
                    if (b[i] != 0) { imu_zero = false; break; }
                }
                if (imu_zero) {
                    g_fd.glitch.zero_imu++;
                    memcpy((void *)g_fd.glitch.last_zero_imu_raw, b, FD_RX_FRAME_BYTES);
                }
                if (s_base_us != 0) {
                    const int32_t xsum = (int32_t)s_base.lx + s_base.ly + s_base.rx + s_base.ry;
                    if (xsum > 1024 || xsum < -1024) {
                        int16_t const f0 = (int16_t)(b[13] | ((uint16_t)b[14] << 8));
                        int16_t const f1 = (int16_t)(b[15] | ((uint16_t)b[16] << 8));
                        int16_t const f2 = (int16_t)(b[17] | ((uint16_t)b[18] << 8));
                        int16_t const f3 = (int16_t)(b[19] | ((uint16_t)b[20] << 8));
                        int32_t const d0 = (int32_t)f0 - s_base.lx, d1 = (int32_t)f1 - s_base.ly;
                        int32_t const d2 = (int32_t)f2 - s_base.rx, d3 = (int32_t)f3 - s_base.ry;
                        if (d0 < 256 && d0 > -256 && d1 < 256 && d1 > -256 &&
                            d2 < 256 && d2 > -256 && d3 < 256 && d3 > -256) {
                            g_fd.glitch.splice++;
                            memcpy((void *)g_fd.glitch.last_splice_raw, b, FD_RX_FRAME_BYTES);
                        }
                    }
                }
            }
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

/* 依次发命令：4 条初始化握手 + 开 test mode；启动完成后 = 帧队列（写入类命令）
 * + 引擎（acquire 声明 / 全局配置分块读 / 设备信息兜底 poll）。 */
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
    /* A1 推送：设备端配置发生变更（含我们自己的成功写入）→ 自动重读一次，
     * 速率限制 1.5s，避免推送风暴时反复读 6 块。 */
    if (s_gcfg_stale && !s_gcfg_pending && !s_gcfg_active &&
        s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT &&
        (int32_t)(now_ms() - s_gcfg_read_ms) > 1500) {
        s_gcfg_stale = false;
        s_gcfg_pending = true;
    }
    bool const queued = fd_cmdq_depth() > 0;
    /* 有命令/读取在等（或仍在启动序列）→ 立即发送；否则按 cmd_next_ms 节奏兜底 poll。
     * s_gcfg_pending 必须计入工作判定，否则自动读取会被 500ms/5s 节奏拖慢。 */
    if ((!acquire_pending && !queued && !s_gcfg_pending &&
         !(s_gcfg_active && !s_gcfg_waiting)) ||
        s_ex.cmd_idx < FD_STARTUP_CMD_COUNT) {
        if ((int32_t)(now_ms() - s_ex.cmd_next_ms) < 0) return;
    }
    if (!usbh_edpt_claim(s_ex.daddr, s_ex.ep_out)) { g_fd.cmd_claim_fail++; return; }

    memset(s_ex.cmd_buf, 0, FD_CMD_BYTES);
    uint8_t tag = TAG_NONE;
    if (acquire_pending && s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        /* SDL's unnumbered HID write omits its leading report selector 0x03. */
        flydigi_frame_acquire(s_ex.cmd_buf, acquire_value);
    } else if (s_ex.cmd_idx < FD_STARTUP_CMD_COUNT) {
        if (s_ex.cmd_idx == 4u) {
            /* Captured receiver startup sequence includes this status request
             * between config-data and the test-mode command. */
            static const uint8_t status_cmd[] = { 0x5a, 0xa5, 0x10, 0x02, 0x12 };
            memcpy(s_ex.cmd_buf, status_cmd, sizeof status_cmd);
        } else {
            uint8_t const table_idx = s_ex.cmd_idx > 4u ? 4u : s_ex.cmd_idx;
            flydigi_cmd_pack(&FD_CMDS[table_idx], s_ex.cmd_buf);
        }
    } else if (fd_cmdq_pop(s_ex.cmd_buf, &tag)) {
        /* 队列帧已就位；提交前按 tag 施加副作用，与旧分支“先置副作用再提交”一致。 */
        switch (tag) {
        case TAG_FP_QUERY:    s_fp_awaiting_reply = true; break;
        case TAG_SLOT_SWITCH: g_fd.fp.switches++; break;
        case TAG_CALIB_START: g_fd.ops.calib_starts++; break;
        case TAG_CALIB_FINISH: g_fd.ops.calib_finishes++; break;
        case TAG_STATUS_SET:  g_fd.ops.status_set_sent++; break;
        default: break;
        }
    } else if (s_gcfg_pending || (s_gcfg_active && !s_gcfg_waiting)) {
        /* 全局配置读取：0xA3 READ_CHUNK（ramId=1，16B/块），应答驱动连读；
         * pending 在真正发块时转 active 并重置游标（等价旧分支内转换，
         * 未转 active 前不启动 2s 超时计时）。 */
        if (s_gcfg_pending) {
            s_gcfg_pending = false;
            s_gcfg_active = true;
            s_gcfg_chunk = 0;
            memset(s_gcfg_buf, 0, sizeof s_gcfg_buf);
        }
        flydigi_frame_ram_read_chunk(s_ex.cmd_buf, 1, s_gcfg_chunk);
        s_gcfg_waiting = true;
        s_gcfg_last_ms = now_ms();
    } else {
        /* 兜底 poll：设备信息查询（5a a5 01 02 03，同抓包 poll_cmd），
         * 保持电池/连接信息新鲜（原按需 cmd 03 读取已随死代码移除）。 */
        flydigi_frame_query(s_ex.cmd_buf, 0x01);
        if (s_poll_streak < 255u) s_poll_streak++;
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
        s_acquire_keepalive_ms = now_ms();
        s_ex.cmd_next_ms = now_ms() + 1000u;
        g_fd.cmds_sent++;
        return;
    }
    if (s_ex.cmd_idx < FD_STARTUP_CMD_COUNT) s_ex.cmd_idx++;
    g_fd.cmds_sent++;
    if (s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT) {
        bool const battery_valid = g_fd.battery_valid;
        /* 设备信息轮询节奏：电池有效 → 5s 常规；否则 500ms 快速重试，但连续
         * 无应答（手柄离线等）超 ~20s 后退避到 5s，避免 2Hz 空转（每次命令
         * 还会让接收器 stall ~18ms）。收到 01 应答即归零。 */
        uint32_t const poll_ms = battery_valid ? FD_DEVICE_INFO_POLL_MS
            : (s_poll_streak <= 40u ? FD_DEVICE_INFO_RETRY_MS : FD_DEVICE_INFO_POLL_MS);
        s_ex.cmd_next_ms = now_ms() + poll_ms;
        /* 开机自动读一次全局配置（填充 [CFG]；睡眠非 0 时会在读取完成后自愈） */
        if (!s_gcfg_auto_done) {
            s_gcfg_auto_done = true;
            flydigi_host_request_global_config();
        }
        /* SDL 式接管声明（0x1C）：接收器据此切到高帧率流（EF 实测 97→716 Hz）。
         * 每次挂载/断流恢复只声明一次；接收器会记录占用者（此处为 "BRG"）。 */
        if (!s_acquire_auto_done) {
            s_acquire_auto_done = true;
            critical_section_enter_blocking(&s_state_lock);
            s_acquire_value = true;
            s_acquire_pending = true;
            critical_section_exit(&s_state_lock);
        }
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
    /* 高帧率流保活：每 30 秒重声明一次 acquire（对齐 SDL 的心跳习惯）。
     * 接收器的接管会话可能被其它应用释放，重声明后 EF 立即恢复 ~700 Hz。 */
    if (s_ex.cmd_idx >= FD_STARTUP_CMD_COUNT &&
        (int32_t)(ms - s_acquire_keepalive_ms) >= 30000) {
        critical_section_enter_blocking(&s_state_lock);
        s_acquire_value = true;
        s_acquire_pending = true;
        critical_section_exit(&s_state_lock);
    }
    /* 全局配置读取超时保护：2 秒无应答即终止并计数 */
    if (s_gcfg_active && (int32_t)(ms - s_gcfg_last_ms) >= 2000) {
        s_gcfg_active = false;
        s_gcfg_waiting = false;
        g_fd.gcfg_errors++;
    }
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
        /* 断流恢复退避：前两次快速重试（射频瞬断，~2s），持续无输入（手柄关机等）
         * 则降到 30s 慢速（等于 acquire 保活节奏），避免恢复风暴（实测手柄离线时
         * 每 ~2s 重启整套启动序列+sent 计数空转）。输入恢复时 streak 归零；
         * 手动 'r' 重排始终快速且重置退避。 */
        if (resend_requested) {
            s_recover_streak = 0;
            s_retry_ms = ms + 2000u;
        } else {
            s_recover_streak++;
            s_retry_ms = ms + (s_recover_streak > 2 ? 30000u : 2000u);
        }
        /* 断流恢复后重新声明接管（高帧率流） */
        s_acquire_auto_done = false;
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

/* 睡眠定时器分钟数（0xA0 field 32）：建帧入队；0 = 永不。 */
void flydigi_host_request_sleep_minutes(uint8_t minutes)
{
    flydigi_host_request_field(FD_FIELD_SLEEP, minutes);
}

/* 报告率 Hz（0xA0 field 33，u16LE）：建帧入队。 */
void flydigi_host_request_report_rate_hz(uint16_t hz)
{
    flydigi_host_request_field(FD_FIELD_RATE, hz);
}

void flydigi_host_request_sku(void)
{
    /* 0x0F SKU 名称查询：5A A5 0F 02 11 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_query(frame, 0x0f);
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_fingerprint(void)
{
    /* 0xA1 配置指纹查询：5A A5 A1 02 A3 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_query(frame, 0xa1);
    fd_cmdq_push(frame, TAG_FP_QUERY);
}

void flydigi_host_request_ram_meta(uint8_t ram_id)
{
    /* 0xA3 子操作 1：RAM 元数据（长度 + CRC32）。 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_ram_meta(frame, ram_id);
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_switch_slot(uint8_t slot)
{
    /* 0xA2 切换板载配置槽。 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_slot_switch(frame, (uint8_t)(slot & 0x03u));
    fd_cmdq_push(frame, TAG_SLOT_SWITCH);
}

void flydigi_host_request_save(uint8_t slot)
{
    /* 0xA7 保存到板载（payload [01, cfgId]）。 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_save(frame, (uint8_t)(slot & 0x03u));
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_reset(uint8_t cfg_id)
{
    /* 0xAF 重置配置段：0-3 用户槽 / 128 全局 / 129 出厂 / 255 全部。 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_reset(frame, cfg_id);
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_calibration(bool start)
{
    /* 摇杆校准（32B 特殊帧）；开始/结束用 tag 区分计数。 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_calib(frame, start);
    fd_cmdq_push(frame, start ? TAG_CALIB_START : TAG_CALIB_FINISH);
}

bool flydigi_host_field_writable(uint8_t field_id)
{
    switch (field_id) {
    case FD_FIELD_QUICK_SWITCH:
    case FD_FIELD_LOCAL_MAP:
    case FD_FIELD_JS_CENTER_AUTO:
    case FD_FIELD_SLEEP:
    case FD_FIELD_RATE:
    case FD_FIELD_JS_RESOLUTION:
    case FD_FIELD_XINPUT_HOME:
        return true;
    default:
        /* FD_FIELD_THIRDPARTY(37) 会切断 acquire；FD_FIELD_NAME(1) 走文本入口。 */
        return false;
    }
}

void flydigi_host_request_field(uint8_t field_id, uint16_t value)
{
    if (!flydigi_host_field_writable(field_id)) return;
    uint8_t frame[FD_CMD_BYTES];
    if (field_id == FD_FIELD_RATE)
        flydigi_frame_field_u16le(frame, field_id, value);   /* 0xA0 载荷 u16LE */
    else
        flydigi_frame_field_u8(frame, field_id, (uint8_t)value);   /* 0xA0 载荷 u8 */
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_name(const char *text)
{
    if (text == NULL) return;
    unsigned n = 0;
    while (n < 16u && text[n] != '\0') n++;      /* 长度上限 16 不变 */
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_field_text(frame, FD_FIELD_NAME, text, n);
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_status_set(uint8_t xinput, uint8_t dinput, uint8_t mouse)
{
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_status_set(frame, xinput, dinput, mouse);
    fd_cmdq_push(frame, TAG_STATUS_SET);
}

/* 恢复 allowThirdparty=1（0xAF 重置实验的保底路径；acquire 依赖此位）。
 * 37 不在白名单，直接建帧（等价旧 field 分支：5A A5 A0 04 25 01 CS）。 */
void flydigi_host_request_thirdparty_restore(void)
{
    uint8_t frame[FD_CMD_BYTES];
    flydigi_frame_field_u8(frame, FD_FIELD_THIRDPARTY, 1);
    fd_cmdq_push(frame, TAG_NONE);
}

void flydigi_host_request_global_config(void)
{
    /* 读取进行中则忽略本次请求（在途结果即最新；配置变更推送机制会在
     * 完成后按需自动补读）。旧实现会无条件重启读取，导致上一轮的
     * 在途分块应答被判为乱序并 abort+err（2026-10-18 修复）。 */
    if (s_gcfg_active) return;
    s_gcfg_pending = true;
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

    critical_section_enter_blocking(&s_state_lock);

    bool const base_fresh = s_base_us != 0 && (t - s_base_us) < FD_EXT_FRESH_US;
    bool const ext_fresh  = s_ext_us  != 0 && (t - s_ext_us)  < FD_EXT_FRESH_US;
    flydigi_rx_merge(&s_base, base_fresh, &s_ext, ext_fresh, &r);
    if (sample) {
        uint32_t const source_us = ext_fresh ? s_ext_us : base_fresh ? s_base_us : 0;
        sample->sticks_us = source_us;
        sample->other_us = source_us;
        sample->sensor_us = ext_fresh ? s_ext_us : 0;
        sample->xinput_overlay = false;
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
