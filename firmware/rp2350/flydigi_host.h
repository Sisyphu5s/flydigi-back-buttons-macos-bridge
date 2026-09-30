/* flydigi_host.h — 飞智 2.4G 接收器 host 侧（两个自定义 TinyUSB 类驱动）
 *
 *   接口 0：vendor 0xFF/0x5D（XInput），EP IN 20B / EP OUT 8B
 *           → 摇杆、扳机、标准按键（主机 1 kHz 轮询）
 *   接口 1：HID，EP IN 32B / EP OUT 32B
 *           → 配置命令（5a a5 …，开 test mode）+ 扩展输入帧（5a a5 ef，含 M1–M4）
 *
 * 合成规则：新鲜 EF 帧优先提供完整状态；可选地用更晚到达的 XInput
 *           摇杆轴覆盖，扳机、按键和 IMU 仍由 EF 提供。EF 断流后回退 XInput。
 *
 * 依据：docs/02 §1/§3/§7（一手实测）；refs/Flydigi5Pico（同板 host 栈）
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "flydigi_rx.h"
#include "bridge_timing.h"

/* 扩展帧新鲜度窗口：超时则视为扩展键全部释放 */
#define FD_EXT_FRESH_US 250000u

/* The receiver's XInput endpoint carries one complete 20-byte report. */
#define FD_XINPUT_REPORT_BYTES 20u

/* A device-info command stalls the receiver's input endpoints for roughly
 * 18 ms. Keep the last validated battery value through a few query periods;
 * the host clears it on disconnect and retries quickly until the first valid
 * reply arrives. */
#define FD_BATTERY_FRESH_US 10000000u

/* Once a battery reply is available, query it infrequently so the command
 * channel cannot add a visible periodic input pause. */
#define FD_DEVICE_INFO_POLL_MS 5000u
#define FD_DEVICE_INFO_RETRY_MS 500u

/* 命令间隔（毫秒）：接收器在两次命令之间需要回话时间 */
#define FD_CMD_GAP_MS 40u

/* The receiver latches a non-zero XInput rumble packet.  Refreshing the
 * request keeps an active effect alive; silence after this interval sends a
 * zero packet so a one-shot effect cannot run forever. */
#define FD_RUMBLE_WATCHDOG_US 250000u

#define FD_INPUT_EVENT_CAPACITY 64u
typedef struct {
    uint32_t sequence, time_us;
    uint8_t source; /* 0=XInput, 1=EF */
    uint8_t b1, b2, ext1, ext2, guide, lt, rt;
    int16_t lx, ly, rx, ry;
    uint16_t raw_buttons;
} fd_input_event_t;

typedef struct {
    /* 连接与计数（只读，供日志/控制台） */
    bool     receiver_mounted;      /* 接收器已被 host 栈配置完成 */
    bool     xinput_up;             /* 接口 0 已开 */
    bool     ext_up;                /* 接口 1 已开 */
    bool     ext_frame_seen;        /* 收到过 5a a5 ef 扩展帧（= test mode 生效） */
    uint16_t vid, pid;
    uint8_t  xitf_num, ext_itf_num;
    uint8_t  xep_in, ext_ep_in, ext_ep_out;
    uint16_t xep_in_size, ext_ep_in_size, ext_ep_out_size;
    uint32_t xinput_reports;        /* 接口 0 收到的报文数 */
    uint32_t xinput_invalid_buttons; /* 含保留按钮位而被丢弃的报文数 */
    uint32_t ext_reports;           /* 接口 1 收到的报文数（含非 EF） */
    uint32_t ext_frames;            /* 其中 5a a5 ef 帧数 */
    uint32_t ext_bad_checksum;      /* EF 魔数正确但尾字节校验失败 */
    uint32_t cmds_sent;             /* 已发出的配置命令条数 */
    uint32_t acquire_sent, acquire_replies;
    uint8_t  acquire_state, acquire_reason;
    uint32_t takeover_status_replies;
    uint8_t  takeover_enabled;
    uint32_t mounts, umounts;       /* 挂载/卸载事件次数（诊断“USB-A 上到底插没插东西”） */
    uint8_t  last_umount_daddr;
    uint32_t ext_us_stale;          /* EF 帧距今微秒（诊断用，> FD_EXT_FRESH_US 表示过期） */
    uint8_t  last_ext_raw[FD_RX_FRAME_BYTES];
    uint8_t  last_bad_ext_raw[FD_RX_FRAME_BYTES];
    uint16_t last_bad_ext_len;       /* 实际完成长度，用于区分短包与内容损坏 */
    uint8_t  last_bad_ext_ep;
    uint8_t  last_reply_raw[FD_RX_FRAME_BYTES];
    uint8_t  last_xinput_raw[20];
    uint8_t  last_dual_trigger_xinput_raw[FD_XINPUT_REPORT_BYTES];
    uint8_t  last_dual_trigger_ext_lt, last_dual_trigger_ext_rt;
    uint32_t last_dual_trigger_ext_age_us;
    uint32_t xinput_dual_trigger_reports;
    bool     battery_valid;
    uint8_t  battery_percent, battery_state;
    uint32_t battery_updates, rumble_sent, rumble_fail;
    uint8_t  rumble_ack_left, rumble_ack_right; /* last successful OUT transfer */
    uint32_t xinput_min_interval_us, xinput_max_interval_us;
    uint32_t ext_min_interval_us, ext_max_interval_us;

    /* 枚举诊断：记录应用类驱动实际看到的接口，避免仅凭 TinyUSB 日志判断。 */
    uint32_t xi_open_calls, xi_open_ok, xi_open_ep_fail;
    uint32_t ext_open_calls, ext_open_ok, ext_open_no_hid, ext_open_busy;
    uint32_t ext_open_ep_fail, ext_open_size_fail;
    uint8_t  xi_last_itf, xi_last_class, xi_last_subclass, xi_last_protocol, xi_last_eps;
    uint8_t  ext_last_itf, ext_last_class, ext_last_subclass, ext_last_protocol, ext_last_eps;
    uint8_t  ext_trace_itf[4], ext_trace_in_ep[4], ext_trace_out_ep[4];
    uint16_t ext_trace_in_size[4], ext_trace_out_size[4];
    uint32_t xi_poll_submit, xi_poll_claim_fail, xi_poll_xfer_fail;
    uint32_t xi_xfer_cb, xi_xfer_ok;
    uint32_t xi_reports_rejected;
    uint32_t xinput_spikes_filtered;
    uint32_t ext_poll_submit, ext_poll_claim_fail, ext_poll_xfer_fail;
    uint32_t ext_xfer_cb, ext_xfer_ok, ext_out_xfer_ok;
    uint32_t cmd_claim_fail, cmd_xfer_fail;
    bool     xi_in_flight, ext_in_flight;
    uint32_t hid_idle_sent, hid_idle_queue_fail, hid_idle_complete;
    uint8_t  hid_idle_last_itf;
    uint8_t  hid_idle_last_result;
    uint32_t hid_report_sent, hid_report_queue_fail, hid_report_complete;
    uint8_t  hid_report_last_itf;
    uint8_t  hid_report_last_result;
    uint32_t hid_proto_sent, hid_proto_queue_fail, hid_proto_complete;
    uint8_t  hid_proto_last_itf;
    uint8_t  hid_proto_last_result;
} fd_host_status_t;

extern volatile fd_host_status_t g_fd;

/* Initialize the cross-core mailbox before launching core1. */
void flydigi_host_init(void);

/* 循环里调用：推命令序列 + 轮询 IN 端点（1 kHz） */
void flydigi_host_task(void);

/* 取合成后的输入状态及各字段来源时间戳。 */
void flydigi_host_get(flydigi_rx_state_t *out, fd_host_status_t *out_st,
                      bridge_input_sample_t *sample);

/* Incremented after each accepted input frame or receiver reset.  Core0 can
 * use it to attempt a HID update without waiting for its next 1 ms tick. */
uint32_t flydigi_host_input_generation(void);

/* Copy the newest input changes in chronological order.  The ring remains
 * available after the physical button has been released. */
size_t flydigi_host_get_events(fd_input_event_t *out, size_t capacity);

/* 重排命令序列（控制台 'r'）：从头再发一遍握手、状态查询和 test mode */
void flydigi_host_resend_cmds(void);

/* One-shot SDL-style acquire/release probe on the receiver's command endpoint. */
void flydigi_host_acquire_probe(bool acquire);

/* Latest requested motor strengths, 0..255; submitted by core1. */
void flydigi_host_rumble(uint8_t left, uint8_t right);

/* 接收器是否在线且接口 0 最近有数据 */
bool flydigi_host_ready(void);
