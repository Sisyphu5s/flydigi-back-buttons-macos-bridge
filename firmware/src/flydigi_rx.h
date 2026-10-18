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

/* 统一应答视图：应答帧与命令帧同构 5a a5 <cmd> <len> <payload…> CS。
 * p 指向帧 [3] 起（pkgCount/pkgIndex 或子操作/状态…），d 指向帧 [5] 起
 * （数据载荷；与官方 payload 索引一致）。 */
typedef struct {
    uint8_t cmd;        /* 帧 [2] */
    const uint8_t *p;   /* 帧 [3] 起（pkgCount/pkgIndex 或子操作/状态…） */
    size_t pn;          /* min(len,32)-3 */
    const uint8_t *d;   /* 帧 [5] 起（数据载荷；官方 payload 索引与此一致） */
    size_t dn;          /* min(len,32)-5 */
} flydigi_reply_view_t;

/* 校验魔数 5a a5 + 校验和（=1+Σ[0..30]，等价 Σ[2..30]）+ buf[2]==cmd；
 * 通过则填充 view（p/d 指向 buf 内部，buf 需保持存活）返回 true。 */
bool flydigi_reply_view(const uint8_t *buf, size_t len, uint8_t cmd, flydigi_reply_view_t *view);

/* 设备信息全量（0x01 应答，官方 Un() 同布局；版本为 u16 大端 BCD，0x7226 → 7.2.2.6）。 */
typedef struct {
    uint8_t  model;              /* [5] 130 = Vader 5 Pro */
    uint8_t  connection;         /* [6] 1=有线 / 2=无线（新架构相反，本机实测 1=无线） */
    uint32_t mac_le;             /* [7..10] u32LE（本接收器为 0） */
    uint8_t  battery_byte;       /* [11] 高 nibble 充电状态 / 低 nibble 电量档 */
    uint8_t  chip_main, chip_recv;   /* [12] 低/高 nibble */
    uint8_t  gyro_type;          /* [13] 低 nibble */
    uint16_t fw_main, fw_recv, fw_si, fw_trigger, fw_screen, fw_adc, fw_sstar; /* [15..28] */
    uint8_t  features;           /* [29] 能力位，bit 含义见 FD_FEATURE_* */
} flydigi_dev_info_t;

bool flydigi_rx_device_info_ex(const uint8_t *buf, size_t len, flydigi_dev_info_t *out);

#define FD_FEATURE_IMU             0x01u
#define FD_FEATURE_LED             0x02u
#define FD_FEATURE_SWITCH          0x04u
#define FD_FEATURE_SCREEN          0x08u
#define FD_FEATURE_TRIGGER_HAPTIC  0x10u   /* 扳机触觉（VCM） */
#define FD_FEATURE_TRIGGER_VIBRATE 0x20u   /* 扳机震动 */
#define FD_FEATURE_AUDIO           0x40u
#define FD_FEATURE_HAPTIC          0x80u

/* SKU 名称（0x0F 应答）：payload[0]=value，[1..20]=SKU 字符串。 */
bool flydigi_rx_sku(const uint8_t *buf, size_t len, uint8_t *value, char out[21]);

/* 配置指纹（0xA1 指纹查询 / 0xA2 切槽应答）：活动槽 + 4×槽 CRC16 + 全局 CRC16。 */
bool flydigi_rx_fingerprint(const uint8_t *buf, size_t len, uint8_t *active_slot,
                            uint16_t slot_crc[4], uint16_t *global_crc);

/* RAM 元数据（0xA3 子操作 1）：状态 + ramId + 槽 + 长度 + CRC32。 */
bool flydigi_rx_ram_meta(const uint8_t *buf, size_t len, uint8_t *status, uint8_t *ram_id,
                         uint8_t *slot, uint16_t *data_len, uint32_t *crc32);

/* 全局配置（官方 0xA3 读 RAM1 后取偏移 2..82 的 80 字节 blob）字段解码。 */
typedef struct {
    uint16_t report_rate_hz;     /* @41 u16LE：500/250/125 有效，其它值视为 1000 */
    uint8_t  sleep_minutes;      /* @3（0 = 永不） */
    bool     allow_thirdparty;   /* @47 == 1 */
    uint8_t  js_resolution;      /* @43：8..12，否则 12 */
    bool     quick_switch;       /* @4 == 1：FN+ABXY 快速切换板载配置 */
    bool     js_center_auto;     /* @39 == 1：摇杆自动中心校准 */
    bool     local_map;          /* @40 == 1：板载映射启用 */
    bool     xinput_home;        /* @52 == 1：XInput Home 行为 */
    char     name[17];           /* @7..38 UTF-8，取前 16 字节，非可见字符替换为 '.' */
    uint8_t  raw[80];            /* 原始 80 字节（供诊断/差分） */
} flydigi_gcfg_t;

bool flydigi_gcfg_parse(const uint8_t *blob80, flydigi_gcfg_t *out);

/* 全局配置字段写 fieldId（官方 $i 转发表全表，均经 0xA0 SetDeviceConfig）。 */
#define FD_FIELD_NAME             1u   /* displayName：文本 */
#define FD_FIELD_QUICK_SWITCH     20u  /* enableQuickSwitch：u8 */
#define FD_FIELD_LOCAL_MAP        23u  /* enableLocalMap：u8 */
#define FD_FIELD_JS_CENTER_AUTO   25u  /* enableJsCenterAutoAdjust：u8 */
#define FD_FIELD_SLEEP            32u  /* sleepTime：u8（分钟，0=永不） */
#define FD_FIELD_RATE             33u  /* usbReturnRateHz：u16LE */
#define FD_FIELD_JS_RESOLUTION    34u  /* jsResolution：u8（8..12） */
#define FD_FIELD_THIRDPARTY       37u  /* allowThirdparty：u8（勿关：acquire 依赖） */
#define FD_FIELD_XINPUT_HOME      38u  /* enableXinputHome：u8 */

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

/* EF is the single authoritative source while fresh (sticks, triggers, buttons,
 * extended keys, Home and IMU).  XInput is consulted only as a full fallback
 * when the EF stream has stalled; mixed-source merging is intentionally gone. */
void flydigi_rx_merge(const flydigi_rx_state_t *base, bool base_fresh,
                      const flydigi_rx_state_t *ext, bool ext_fresh,
                      flydigi_rx_state_t *out);

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
