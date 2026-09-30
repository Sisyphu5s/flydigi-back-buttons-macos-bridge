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
    *percent = power == 2 ? 100 : (uint8_t)(level * 10);
    *power_state = power;
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

bool flydigi_rx_merge(const flydigi_rx_state_t *base, bool base_fresh, uint32_t base_age_us,
                      const flydigi_rx_state_t *ext, bool ext_fresh, uint32_t ext_age_us,
                      bool fresh_xinput_sticks, flydigi_rx_state_t *out)
{
    if (out == NULL || base == NULL || ext == NULL) return false;
    *out = (flydigi_rx_state_t){0};
    if (ext_fresh) *out = *ext;
    else if (base_fresh) *out = *base;

    /* The receiver's EF stream is required for M1-M4 and IMU, but on the
     * current 2.4G firmware its byte[11]/byte[12] standard-button fields can
     * remain zero while the XInput endpoint carries the real transitions.
     * Both parsers normalize D-pad bits to the same U/R/D/L constants, so
     * merge the standard fields from both fresh streams.  EF's ~100 Hz
     * release updates clear a short-lived event-driven XInput bit without
     * sacrificing an EF-only button transition. */
    if (ext_fresh && base_fresh) {
        out->b1 = (uint8_t)(ext->b1 | base->b1);
        out->b2 = (uint8_t)(ext->b2 | base->b2);
    }
    out->guide = ext_fresh ? (ext->ext2 & FD_EXT2_HOME) != 0 :
                 base_fresh && base->guide;
    bool const use_base_sticks = fresh_xinput_sticks && base_fresh && ext_fresh &&
                                 base_age_us < ext_age_us;
    if (use_base_sticks) {
        out->lx = base->lx; out->ly = base->ly;
        out->rx = base->rx; out->ry = base->ry;
    }
    return use_base_sticks;
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
