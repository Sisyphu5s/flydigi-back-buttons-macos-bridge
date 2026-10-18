/* flydigi_rx.c — 见 flydigi_rx.h */
#include "flydigi_rx.h"
#include <string.h>

/* 命令表：前 4 条 = 初始化握手（参考实现要求“连接后必须做，否则无数据”），
 * 第 5/6 条 = test mode 开/关（扩展输入 + IMU）。 */
const flydigi_cmd_t FD_CMDS[] = {
    { "device info ", { 0x5a, 0xa5, 0x01, 0x02, 0x03 }, 5 },
    { "MAC/serial  ", { 0x5a, 0xa5, 0xa1, 0x02, 0xa3 }, 5 },
    { "config read ", { 0x5a, 0xa5, 0x02, 0x02, 0x04 }, 5 },
    { "config data ", { 0x5a, 0xa5, 0x04, 0x02, 0x06 }, 5 },
    { "test mode on", { 0x5a, 0xa5, 0x11, 0x07, 0xff, 0x01, 0xff, 0xff, 0xff, 0x15, 0x00 }, 11 },
    { "test mode off", { 0x5a, 0xa5, 0x11, 0x07, 0xff, 0x00, 0xff, 0xff, 0xff, 0x14, 0x00 }, 11 },
};
const size_t FD_CMDS_COUNT = sizeof FD_CMDS / sizeof FD_CMDS[0];

static int16_t rd16le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool flydigi_rx_parse(const uint8_t *buf, size_t len, flydigi_rx_state_t *out)
{
    if (buf == NULL || out == NULL || len < FD_RX_FRAME_BYTES)
        return false;
    if (buf[0] != FD_RX_MAGIC0 || buf[1] != FD_RX_MAGIC1 || buf[2] != FD_RX_MAGIC2)
        return false;
    uint8_t checksum = 1;
    for (size_t i = 0; i < FD_RX_FRAME_BYTES - 1; i++) checksum += buf[i];
    if (checksum != buf[FD_RX_FRAME_BYTES - 1]) return false;

    int16_t lx = rd16le(buf + 3), ly = rd16le(buf + 5);
    int16_t rx = rd16le(buf + 7), ry = rd16le(buf + 9);
    int16_t g[3], a[3];
    for (unsigned i = 0; i < 3; i++) {
        g[i] = rd16le(buf + 17 + 2 * i);
        a[i] = rd16le(buf + 23 + 2 * i);
    }

    out->lx = lx; out->ly = ly; out->rx = rx; out->ry = ry;
    out->b1 = buf[11]; out->b2 = buf[12]; out->ext1 = buf[13]; out->ext2 = buf[14];
    /* The EF Home polarity is receiver-specific and resolved by the host
     * merge layer; keep the raw frame parser neutral. */
    out->guide = false;
    out->lt = buf[15]; out->rt = buf[16];
    for (unsigned i = 0; i < 3; i++) { out->gyro[i] = g[i]; out->accel[i] = a[i]; }
    return true;
}

bool flydigi_rx_battery_status(const uint8_t *buf, size_t len,
                               uint8_t *percent, uint8_t *power_state)
{
    if (buf == NULL || len < FD_RX_FRAME_BYTES || percent == NULL ||
        power_state == NULL || buf[0] != 0x5a || buf[1] != 0xa5 ||
        buf[2] != 0x01 || buf[5] != 0x82) return false;

    uint8_t checksum = 1;
    for (size_t i = 0; i < FD_RX_FRAME_BYTES - 1; i++) checksum += buf[i];
    if (checksum != buf[FD_RX_FRAME_BYTES - 1]) return false;

    uint8_t const power = buf[11] >> 4;
    uint8_t const level = buf[11] & 0x0f;
    if (power > 2 || level > 10) return false;
    /* Vader 5 Pro 电量 nibble 为 20% 步进；SDL / openvader / tux-invader /
     * openflydigi 四个独立实现一致（10% 步进仅适用于 Apex 6）。现场出现过
     * nibble=10 的满电读数，统一按 *20 并封顶 100。 */
    uint8_t pct = (uint8_t)(level * 20u);
    if (pct > 100u) pct = 100u;
    *percent = power == 2 ? 100 : pct;
    *power_state = power;
    return true;
}

/* 统一应答校验 + 视图切分：魔数 + 校验和（1+Σ[0..30] ≡ Σ[2..30]）+ cmd。
 * p=[3] 起、d=[5] 起；pn/dn 按 32 字节缓冲截断。 */
bool flydigi_reply_view(const uint8_t *buf, size_t len, uint8_t cmd, flydigi_reply_view_t *view)
{
    if (buf == NULL || view == NULL || len < FD_RX_FRAME_BYTES) return false;
    if (buf[0] != FD_RX_MAGIC0 || buf[1] != FD_RX_MAGIC1 || buf[2] != cmd) return false;

    uint8_t checksum = 1;
    for (size_t i = 0; i < FD_RX_FRAME_BYTES - 1; i++) checksum += buf[i];
    if (checksum != buf[FD_RX_FRAME_BYTES - 1]) return false;

    size_t const n = len < FD_RX_FRAME_BYTES ? len : FD_RX_FRAME_BYTES;
    view->cmd = cmd;
    view->p = buf + 3;
    view->pn = n - 3;
    view->d = buf + 5;
    view->dn = n - 5;
    return true;
}

static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

bool flydigi_rx_device_info_ex(const uint8_t *buf, size_t len, flydigi_dev_info_t *out)
{
    flydigi_reply_view_t v;
    if (out == NULL || !flydigi_reply_view(buf, len, 0x01, &v)) return false;

    out->model = v.d[0];
    out->connection = v.d[1];
    out->mac_le = (uint32_t)v.d[2] | ((uint32_t)v.d[3] << 8) |
                  ((uint32_t)v.d[4] << 16) | ((uint32_t)v.d[5] << 24);
    out->battery_byte = v.d[6];
    out->chip_main = v.d[7] & 0x0fu;
    out->chip_recv = (v.d[7] >> 4) & 0x0fu;
    out->gyro_type = v.d[8] & 0x0fu;
    out->fw_main = rd16be(v.d + 10);
    out->fw_recv = rd16be(v.d + 12);
    out->fw_si = rd16be(v.d + 14);
    out->fw_trigger = rd16be(v.d + 16);
    out->fw_screen = rd16be(v.d + 18);
    out->fw_adc = rd16be(v.d + 20);
    out->fw_sstar = rd16be(v.d + 22);
    out->features = v.d[24];
    return true;
}

bool flydigi_rx_sku(const uint8_t *buf, size_t len, uint8_t *value, char out[21])
{
    flydigi_reply_view_t v;
    if (!flydigi_reply_view(buf, len, 0x0f, &v)) return false;

    if (value != NULL) *value = v.d[0];
    if (out != NULL) {
        unsigned n = 0;
        for (; n < 20u && v.d[1 + n] != 0; n++) {
            uint8_t const c = v.d[1 + n];
            out[n] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        out[n] = '\0';
    }
    return true;
}

bool flydigi_rx_fingerprint(const uint8_t *buf, size_t len, uint8_t *active_slot,
                            uint16_t slot_crc[4], uint16_t *global_crc)
{
    flydigi_reply_view_t v;
    /* 0xA1 指纹查询应答 或 0xA2 切槽应答 */
    if (!flydigi_reply_view(buf, len, 0xa1, &v) &&
        !flydigi_reply_view(buf, len, 0xa2, &v)) return false;

    if (active_slot != NULL) *active_slot = v.d[0];
    if (slot_crc != NULL)
        for (unsigned i = 0; i < 4u; i++) slot_crc[i] = rd16le(v.d + 1 + 2 * i);
    if (global_crc != NULL) *global_crc = rd16le(v.d + 9);
    return true;
}

bool flydigi_rx_ram_meta(const uint8_t *buf, size_t len, uint8_t *status, uint8_t *ram_id,
                         uint8_t *slot, uint16_t *data_len, uint32_t *crc32)
{
    flydigi_reply_view_t v;
    /* 应答布局（p 自 [3] 起）：p[0]=子操作(1) p[1]=status p[2]=ramId p[3]=槽
     * p[4..5]=长度 u16LE p[6..9]=CRC32。子操作 0 为 READ_CHUNK，需排除。 */
    if (!flydigi_reply_view(buf, len, 0xa3, &v) || v.p[0] != 0x01) return false;

    if (status != NULL) *status = v.p[1];
    if (ram_id != NULL) *ram_id = v.p[2];
    if (slot != NULL) *slot = v.p[3];
    if (data_len != NULL) *data_len = rd16le(v.p + 4);
    if (crc32 != NULL)
        *crc32 = (uint32_t)v.p[6] | ((uint32_t)v.p[7] << 8) |
                 ((uint32_t)v.p[8] << 16) | ((uint32_t)v.p[9] << 24);
    return true;
}

bool flydigi_gcfg_parse(const uint8_t *blob80, flydigi_gcfg_t *out)
{
    if (blob80 == NULL || out == NULL) return false;

    uint16_t const rate = (uint16_t)(blob80[41] | ((uint16_t)blob80[42] << 8));
    out->report_rate_hz = (rate == 500u || rate == 250u || rate == 125u) ? rate : 1000u;
    out->sleep_minutes = blob80[3];
    out->allow_thirdparty = blob80[47] == 1u;
    out->quick_switch = blob80[4] == 1u;
    out->js_center_auto = blob80[39] == 1u;
    out->local_map = blob80[40] == 1u;
    out->xinput_home = blob80[52] == 1u;
    uint8_t const res = blob80[43];
    out->js_resolution = (res >= 8u && res <= 12u) ? res : 12u;

    unsigned ni = 0;
    for (; ni < 16u && blob80[7 + ni] != 0; ni++) {
        uint8_t const c = blob80[7 + ni];
        out->name[ni] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out->name[ni] = '\0';

    memcpy(out->raw, blob80, sizeof out->raw);
    return true;
}

bool flydigi_rx_xinput_idle_spike(const uint8_t *buf, size_t len,
                                 const flydigi_rx_state_t *ext, bool ext_recent)
{
    if (buf == NULL || len < 20 || ext == NULL || !ext_recent ||
        buf[0] != 0 || buf[1] != 0x14 || buf[4] < 0xc0 || buf[5] < 0xc0 ||
        ext->lt != 0 || ext->rt != 0) return false;
    for (size_t i = 2; i < 20; i++) {
        if (i != 4 && i != 5 && buf[i] != 0) return false;
    }
    return true;
}

void flydigi_xinput_normalize_buttons(uint16_t btn, uint8_t *b1, uint8_t *b2)
{
    if (b1 == NULL || b2 == NULL) return;
    uint8_t one = 0, two = 0;
    /* XInput D-pad order is Up, Down, Left, Right.  The EF shared fields
     * deliberately use Up, Right, Down, Left so the hat mapper is identical
     * for both input streams. */
    if (btn & (1u << 0)) one |= FD_B1_DPAD_UP;
    if (btn & (1u << 1)) one |= FD_B1_DPAD_DOWN;
    if (btn & (1u << 2)) one |= FD_B1_DPAD_LEFT;
    if (btn & (1u << 3)) one |= FD_B1_DPAD_RIGHT;
    if (btn & (1u << 4)) two |= FD_B2_START;
    if (btn & (1u << 5)) one |= FD_B1_SELECT;
    if (btn & (1u << 6)) two |= FD_B2_L3;
    if (btn & (1u << 7)) two |= FD_B2_R3;
    if (btn & (1u << 8)) two |= FD_B2_LB;
    if (btn & (1u << 9)) two |= FD_B2_RB;
    if (btn & (1u << 12)) one |= FD_B1_A;
    if (btn & (1u << 13)) one |= FD_B1_B;
    if (btn & (1u << 14)) one |= FD_B1_X;
    if (btn & (1u << 15)) two |= FD_B2_Y;
    *b1 = one;
    *b2 = two;
}

bool flydigi_xinput_buttons_valid(uint16_t xinput_buttons)
{
    return (xinput_buttons & (uint16_t)~FD_XINPUT_KNOWN_BUTTON_MASK) == 0;
}

void flydigi_rx_merge(const flydigi_rx_state_t *base, bool base_fresh,
                      const flydigi_rx_state_t *ext, bool ext_fresh,
                      flydigi_rx_state_t *out)
{
    if (out == NULL || base == NULL || ext == NULL) return;
    *out = (flydigi_rx_state_t){0};
    /* 输入统一走 EF（飞智厂商通道）：摇杆、扳机、标准键、扩展键、Home、IMU
     * 全部以 EF 为准。XInput 只在 EF 断流时作为整帧兜底，不再混入。 */
    if (ext_fresh) *out = *ext;
    else if (base_fresh) *out = *base;
    out->guide = ext_fresh ? (ext->ext2 & FD_EXT2_HOME) != 0 :
                 base_fresh && base->guide;
}

void flydigi_cmd_pack(const flydigi_cmd_t *c, uint8_t out[FD_CMD_BYTES])
{
    memset(out, 0, FD_CMD_BYTES);
    if (c == NULL)
        return;
    uint8_t n = c->len;
    if (n > (uint8_t)sizeof c->payload)
        n = (uint8_t)sizeof c->payload;
    if (n > FD_CMD_BYTES)
        n = FD_CMD_BYTES;
    memcpy(out, c->payload, n);
}
