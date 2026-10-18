/* flydigi_cmd.c — 见 flydigi_cmd.h */
#include "flydigi_cmd.h"
#include <string.h>

/* 通用帧核心：out 先清零，填 5A A5 <cmd> <len> <payload…> CS。
 * n 截到 27（= 32 缓冲 - 4 头 - 1 校验），返回有效帧长 = n + 5。 */
static uint8_t frame_generic(uint8_t out[FD_CMD_BYTES], uint8_t cmd,
                             const uint8_t *p, size_t n)
{
    if (n > FD_CMD_BYTES - 5u) n = FD_CMD_BYTES - 5u;
    memset(out, 0, FD_CMD_BYTES);
    out[0] = FD_RX_MAGIC0;
    out[1] = FD_RX_MAGIC1;
    out[2] = cmd;
    out[3] = (uint8_t)(n + 2u);      /* len = payload_len + 2 */
    if (p != NULL && n > 0) memcpy(out + 4, p, n);
    uint8_t cs = 0;
    for (size_t i = 2; i <= 3u + n; i++) cs = (uint8_t)(cs + out[i]);
    out[4 + n] = cs;
    return (uint8_t)(n + 5u);
}

uint8_t flydigi_frame_u8(uint8_t out[FD_CMD_BYTES], uint8_t cmd, uint8_t a)
{
    return frame_generic(out, cmd, &a, 1);
}

uint8_t flydigi_frame_u16le(uint8_t out[FD_CMD_BYTES], uint8_t cmd, uint16_t v)
{
    uint8_t const p[2] = { (uint8_t)(v & 0xffu), (uint8_t)(v >> 8) };
    return frame_generic(out, cmd, p, sizeof p);
}

uint8_t flydigi_frame_bytes(uint8_t out[FD_CMD_BYTES], uint8_t cmd,
                            const uint8_t *p, size_t n)
{
    return frame_generic(out, cmd, p, n);
}

uint8_t flydigi_frame_query(uint8_t out[FD_CMD_BYTES], uint8_t cmd)
{
    return frame_generic(out, cmd, NULL, 0);
}

uint8_t flydigi_frame_field_u8(uint8_t out[FD_CMD_BYTES], uint8_t field_id, uint8_t value)
{
    uint8_t const p[2] = { field_id, value };
    return frame_generic(out, 0xa0, p, sizeof p);
}

uint8_t flydigi_frame_field_u16le(uint8_t out[FD_CMD_BYTES], uint8_t field_id, uint16_t value)
{
    uint8_t const p[3] = { field_id, (uint8_t)(value & 0xffu), (uint8_t)(value >> 8) };
    return frame_generic(out, 0xa0, p, sizeof p);
}

uint8_t flydigi_frame_field_text(uint8_t out[FD_CMD_BYTES], uint8_t field_id,
                                 const char *text, size_t len)
{
    if (len > 26u) len = 26u;    /* payload = 1 + len ≤ 27 */
    uint8_t p[1u + 26u] = { 0 };
    p[0] = field_id;
    if (text != NULL && len > 0) memcpy(p + 1, text, len);
    return frame_generic(out, 0xa0, p, 1u + len);
}

uint8_t flydigi_frame_ram_read_chunk(uint8_t out[FD_CMD_BYTES], uint8_t ram_id, uint8_t chunk)
{
    uint8_t const p[4] = { 0x00, ram_id, chunk, 0x10 };  /* 子操作 0，块大小 16 */
    return frame_generic(out, 0xa3, p, sizeof p);
}

uint8_t flydigi_frame_ram_meta(uint8_t out[FD_CMD_BYTES], uint8_t ram_id)
{
    uint8_t const p[2] = { 0x01, ram_id };               /* 子操作 1 */
    return frame_generic(out, 0xa3, p, sizeof p);
}

uint8_t flydigi_frame_slot_switch(uint8_t out[FD_CMD_BYTES], uint8_t slot)
{
    uint8_t const p[1] = { slot };
    return frame_generic(out, 0xa2, p, sizeof p);
}

uint8_t flydigi_frame_save(uint8_t out[FD_CMD_BYTES], uint8_t slot)
{
    uint8_t const p[2] = { 0x01, slot };                 /* payload [01, cfgId] */
    return frame_generic(out, 0xa7, p, sizeof p);
}

uint8_t flydigi_frame_reset(uint8_t out[FD_CMD_BYTES], uint8_t cfg_id)
{
    return flydigi_frame_u8(out, 0xaf, cfg_id);
}

uint8_t flydigi_frame_calib(uint8_t out[FD_CMD_BYTES], bool start)
{
    /* 特殊帧 1：32B 定长，校验位在 [31] = Σ[2..30]。 */
    static const uint8_t head[7] = { 0x5a, 0xa5, 0x34, 0x04, 0x00, 0x81, 0x03 };
    memset(out, 0, FD_CMD_BYTES);
    memcpy(out, head, sizeof head);
    out[7] = start ? 0x01 : 0x02;
    uint8_t cs = 0;
    for (unsigned i = 2; i <= 30u; i++) cs = (uint8_t)(cs + out[i]);
    out[31] = cs;
    return FD_CMD_BYTES;
}

uint8_t flydigi_frame_status_set(uint8_t out[FD_CMD_BYTES], uint8_t xinput,
                                 uint8_t dinput, uint8_t mouse)
{
    /* 载荷 [xinput,dinput,keyboard=0xff,mouse,thirdparty=0xff] + 22×0，共 27B。 */
    uint8_t p[27] = { 0 };
    p[0] = xinput;
    p[1] = dinput;
    p[2] = 0xff;    /* keyboardStatus 不动 */
    p[3] = mouse;
    p[4] = 0xff;    /* allowThirdparty 不动（acquire 依赖） */
    return frame_generic(out, 0x11, p, sizeof p);
}

uint8_t flydigi_frame_acquire(uint8_t out[FD_CMD_BYTES], bool acquire)
{
    /* 特殊帧 2：官方原帧，[3]=0x17 固定，无校验和；只带有效前 8 字节。 */
    static const uint8_t head[8] = { 0x5a, 0xa5, 0x1c, 0x17, 0x00, 'B', 'R', 'G' };
    memset(out, 0, FD_CMD_BYTES);
    memcpy(out, head, sizeof head);
    out[4] = acquire ? 0x01 : 0x00;
    return 8;
}
