/* report_pack.c — 状态 <-> 10 字节 HID 报告（RP2350 固件与主机测试共用） */
#include "vader2pro_hid.h"
#include <string.h>

static void set_bits(uint8_t *buf, unsigned bitpos, unsigned nbits, uint32_t val)
{
    for (unsigned i = 0; i < nbits; i++) {
        if ((val >> i) & 1u) {
            unsigned p = bitpos + i;
            buf[p >> 3] |= (uint8_t)(1u << (p & 7u));
        }
    }
}

static uint32_t get_bits(const uint8_t *buf, unsigned bitpos, unsigned nbits)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned p = bitpos + i;
        if (buf[p >> 3] & (uint8_t)(1u << (p & 7u)))
            v |= (uint32_t)1u << i;
    }
    return v;
}

void vader2pro_pack(const vader2pro_state_t *s, uint8_t out[VADER2PRO_REPORT_BYTES])
{
    memset(out, 0, VADER2PRO_REPORT_BYTES);
    /* 6 个 8-bit 轴：X LX, Y LY, Z RX, Rx RY, Ry RT, Rz LT */
    out[0] = s->lx; out[1] = s->ly; out[2] = s->rx;
    out[3] = s->ry; out[4] = s->rt; out[5] = s->lt;
    /* hat：bit48 起 4 bit（低半字节） */
    set_bits(out, 48, 4, s->hat & 0x0Fu);
    /* Buttons start at bit56; Generic has 26 buttons and therefore uses
     * the final byte, while Vader2Pro keeps its 20-bit layout. */
    for (unsigned i = 0; i < V2P_BTN_COUNT; i++)
        set_bits(out, 56 + i, 1, s->buttons[i] ? 1u : 0u);
}

void vader2pro_unpack(const uint8_t in[VADER2PRO_REPORT_BYTES], vader2pro_state_t *s)
{
    *s = (vader2pro_state_t){0};
    s->lx = in[0]; s->ly = in[1]; s->rx = in[2];
    s->ry = in[3]; s->rt = in[4]; s->lt = in[5];
    s->hat = (uint8_t)get_bits(in, 48, 4);
    for (unsigned i = 0; i < V2P_BTN_COUNT; i++)
        s->buttons[i] = get_bits(in, 56 + i, 1) != 0;
}
