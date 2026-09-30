/* flydigi_rx.h — 飞智 Vader 5 Pro 接收器（2.4G）接口 1：扩展输入帧解析 + 命令序列
 *
 * 依据（一手）：
 *   docs/02 §3/§7（本项目实测）
 *   参考实现原文 Chillsmeit/vader5pro-remap-driver/docs/protocol.md（已拉取，见 ~/.hermes/cache/scratch/proto.md）
 *
 * 通道：接收器接口 1 = HID，EP2 IN 32B / EP6 OUT 32B（配置命令 + 扩展输入）
 * 前提：**扩展输入必须先由主机开 test mode**，否则只有固定状态帧（真机实测，docs/02 §7.2）
 * 扩展输入帧：32 字节，魔数 5a a5 ef
 *   [3..4] LX int16 LE  [5..6] LY  [7..8] RX  [9..10] RY   （中心 ≈ 0）
 *   [11] 标准按键1  [12] 标准按键2  [13] 扩展按键(含 M1–M4)  [14] 扩展按键2
 *   [15] LT  [16] RT  [17..22] 陀螺 XYZ  [23..28] 加速度 XYZ(4096=1g)  [29..31] 保留
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FD_RX_FRAME_BYTES 32
#define FD_CMD_BYTES      32
#define FD_RX_MAGIC0 0x5a
#define FD_RX_MAGIC1 0xa5
#define FD_RX_MAGIC2 0xef

/* The documented Xbox button word uses bits 0..10 and 12..15. Bit 11 is
 * reserved and must not be allowed to synthesize a button state. */
#define FD_XINPUT_KNOWN_BUTTON_MASK 0xf7ffu

/* [13] 扩展按键 —— 背键就在这（真机帧已交叉验证，docs/02 §7.1） */
enum {
    FD_EXT_C  = 1u << 0, FD_EXT_Z  = 1u << 1,
    FD_EXT_M1 = 1u << 2, FD_EXT_M2 = 1u << 3,
    FD_EXT_M3 = 1u << 4, FD_EXT_M4 = 1u << 5,
    FD_EXT_LM = 1u << 6, FD_EXT_RM = 1u << 7,
};

/* [11] EF 帧按键 1（D-pad 位序 U/R/D/L） */
enum {
    /* EF byte[11] uses the protocol parser's U/R/D/L order.  The
     * separate XInput interface is normalized in flydigi_host.c. */
    FD_B1_DPAD_UP = 1u << 0, FD_B1_DPAD_RIGHT = 1u << 1,
    FD_B1_DPAD_DOWN = 1u << 2, FD_B1_DPAD_LEFT = 1u << 3,
    FD_B1_A = 1u << 4, FD_B1_B = 1u << 5, FD_B1_SELECT = 1u << 6, FD_B1_X = 1u << 7,
};

/* [12] 标准按键 2 */
enum {
    FD_B2_Y = 1u << 0, FD_B2_START = 1u << 1, FD_B2_LB = 1u << 2, FD_B2_RB = 1u << 3,
    FD_B2_L3 = 1u << 6, FD_B2_R3 = 1u << 7,
};

/* [14] 扩展按键 2
 * bit3=Home；同步真机采样确认本 2.4G 接收器按下为 0x08、松开为 0x00。
 * 参考 docs/02 §7.3。 */
enum { FD_EXT2_O = 1u << 0, FD_EXT2_HOME = 1u << 3 };

typedef struct {
    int16_t lx, ly, rx, ry;      /* int16 LE，中心 ≈ 0（量程上限待实测） */
    uint8_t b1, b2, ext1, ext2;  /* 原始位域 */
    bool guide;                  /* 合成后的 Guide/Home（XInput 或 EF 实测电平） */
    uint8_t lt, rt;              /* 0..255 */
    int16_t gyro[3];
    int16_t accel[3];            /* 4096 = 1g */
} flydigi_rx_state_t;

/* 解析扩展输入帧；魔数不符或长度不足返回 false（此时 out 不被修改） */
bool flydigi_rx_parse(const uint8_t *buf, size_t len, flydigi_rx_state_t *out);

/* Decode a receiver device-info battery reply. Invalid replies leave outputs
 * untouched; the host expires the last valid reading on its own timer. */
bool flydigi_rx_battery_status(const uint8_t *buf, size_t len,
                               uint8_t *percent, uint8_t *power_state);

/* Reject the receiver's observed all-idle XInput packet with both triggers
 * high when a recent EF frame confirms they are released. */
bool flydigi_rx_xinput_idle_spike(const uint8_t *buf, size_t len,
                                 const flydigi_rx_state_t *ext, bool ext_recent);

/* Normalize the Xbox/XInput button word into the EF frame's shared
 * U/R/D/L + standard button bit fields.  XInput encodes the D-pad as
 * U/D/L/R (bits 0..3), while the EF protocol uses U/R/D/L. */
void flydigi_xinput_normalize_buttons(uint16_t xinput_buttons,
                                      uint8_t *b1, uint8_t *b2);

/* Reject the receiver's undefined XInput button bit before merging it with
 * the extended stream. */
bool flydigi_xinput_buttons_valid(uint16_t xinput_buttons);

/* EF owns buttons, triggers and IMU while fresh. The optional stick mode
 * overlays only a newer XInput stick sample; it never imports trigger spikes. */
bool flydigi_rx_merge(const flydigi_rx_state_t *base, bool base_fresh, uint32_t base_age_us,
                      const flydigi_rx_state_t *ext, bool ext_fresh, uint32_t ext_age_us,
                      bool fresh_xinput_sticks, flydigi_rx_state_t *out);

/* M1..M4（m = 1..4） */
static inline bool flydigi_m_pressed(const flydigi_rx_state_t *s, unsigned m)
{
    return m >= 1 && m <= 4 && (s->ext1 & (uint8_t)(1u << (1 + m))) != 0;
}

/* ---- 配置命令（32 字节，魔数 5a a5；发往接口 1 的 EP6 OUT） ---- */
typedef struct {
    const char *name;
    uint8_t payload[11];   /* 有效前缀，其余补 0 */
    uint8_t len;
} flydigi_cmd_t;

/* 基础命令表：4 条连接握手 + test mode 开/关；host 还会按抓包顺序插入 0x10 状态查询。 */
extern const flydigi_cmd_t FD_CMDS[];
extern const size_t FD_CMDS_COUNT;

/* 把命令左对齐放进 32 字节命令帧，尾部补 0 */
void flydigi_cmd_pack(const flydigi_cmd_t *c, uint8_t out[FD_CMD_BYTES]);
