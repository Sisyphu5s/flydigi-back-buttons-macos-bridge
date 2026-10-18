/* flydigi_cmd.h — 飞智配置命令帧构造（纯函数，主机可测）
 *
 * 通用帧：5A A5 <cmd> <len=payload_len+2> <payload…> CS
 *   CS = Σ 帧字节[2..(3+payload_len)] & 0xFF，有效帧长 = 4+payload_len+1
 *   缓冲固定 FD_CMD_BYTES(32)，尾部补 0；直接的 u8/u16LE/文本/字段/子操作
 *   构造全部走同一条路径，供 flydigi_host.c 后续收拢使用（R1）。
 *
 * 特殊帧（保持官方原帧字节，勿按通用格式改）：
 *   校准 0x34：32B 定长，校验位在 [31] = Σ[2..30]；
 *   acquire 0x1C：[3]=0x17 是固定值，无校验和。
 *
 * 所有构造函数：out 先 memset 0，返回“有效帧长”（acquire 返回 8）。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "flydigi_rx.h"

/* payload [a] */
uint8_t flydigi_frame_u8(uint8_t out[FD_CMD_BYTES], uint8_t cmd, uint8_t a);

/* payload [lo,hi]（u16 小端） */
uint8_t flydigi_frame_u16le(uint8_t out[FD_CMD_BYTES], uint8_t cmd, uint16_t v);

/* 原始字节 payload（n 截到 27） */
uint8_t flydigi_frame_bytes(uint8_t out[FD_CMD_BYTES], uint8_t cmd, const uint8_t *p, size_t n);

/* 空载荷（如查询：5A A5 <cmd> 02 CS） */
uint8_t flydigi_frame_query(uint8_t out[FD_CMD_BYTES], uint8_t cmd);

/* 0xA0 SetDeviceConfig：payload [fieldId, value] / [fieldId, lo, hi] / [fieldId, text…] */
uint8_t flydigi_frame_field_u8(uint8_t out[FD_CMD_BYTES], uint8_t field_id, uint8_t value);
uint8_t flydigi_frame_field_u16le(uint8_t out[FD_CMD_BYTES], uint8_t field_id, uint16_t value);
uint8_t flydigi_frame_field_text(uint8_t out[FD_CMD_BYTES], uint8_t field_id, const char *text, size_t len); /* len ≤ 26 */

/* 0xA3 子操作 0 READ_CHUNK：5a a5 a3 06 00 <ram> <chunk> 10 CS */
uint8_t flydigi_frame_ram_read_chunk(uint8_t out[FD_CMD_BYTES], uint8_t ram_id, uint8_t chunk);

/* 0xA3 子操作 1 RAM 元数据：5a a5 a3 04 01 <ram> CS */
uint8_t flydigi_frame_ram_meta(uint8_t out[FD_CMD_BYTES], uint8_t ram_id);

/* 0xA2 切换板载配置槽 */
uint8_t flydigi_frame_slot_switch(uint8_t out[FD_CMD_BYTES], uint8_t slot);

/* 0xA7 保存到板载：5a a5 a7 04 01 <slot> CS */
uint8_t flydigi_frame_save(uint8_t out[FD_CMD_BYTES], uint8_t slot);

/* 0xAF 重置配置段（0-3 用户槽 / 128 全局 / 129 出厂 / 255 全部） */
uint8_t flydigi_frame_reset(uint8_t out[FD_CMD_BYTES], uint8_t cfg_id);

/* 摇杆校准特殊帧 1（start=开始 / 结束），返回 32 */
uint8_t flydigi_frame_calib(uint8_t out[FD_CMD_BYTES], bool start);

/* 0x11 SET_STATUS：payload [x,d,0xff,m,0xff]+22×0，len=0x1d，返回 32 */
uint8_t flydigi_frame_status_set(uint8_t out[FD_CMD_BYTES], uint8_t xinput, uint8_t dinput, uint8_t mouse);

/* acquire 特殊帧 2（官方原帧，无校验和），返回 8 */
uint8_t flydigi_frame_acquire(uint8_t out[FD_CMD_BYTES], bool acquire);
