/* bridge_map.h — 飞智接收器扩展输入 → Vader2Pro 10 字节 HID 报文（输出身份）
 *
 * 这是整条桥的"映射层"：输入侧 flydigi_rx_state_t → 输出侧 vader2pro_state_t → 10 B 报文。
 * 背键 M1–M4 在输出侧的落点：按钮位 14..17（描述符 usage 15..18）→ GC 元素 BUTTON_M1..M4。
 * 依据：docs/11 §2/§4（苹果模型库 + UsageTypeIndex 语义）、docs/02 §7.1（背键位图）
 */
#pragma once
#include "flydigi_rx.h"
#include "vader2pro_hid.h"
#include "bridge_config.h"

/* 轴量程：接收器给 int16、中心 ≈ 0；真实满量程待实测（docs/02 §7.3），
 * 暂按 int16 全域线性映射到 0..255（中心 0 → 128）。 */
#define FD_AXIS_HALF 32768L

/* EF byte[11] 的 D-pad 四位（U/R/D/L）→ hat（0=上 1=右上 2=右 … 7=左上 8=中位）。
 * 反方向对（上+下、左+右）与三向同时按 → 中位（8）。 */
uint8_t flydigi_hat_from_b1(uint8_t b1);

/* Physical button sources in the same index order as button_map. */
uint32_t bridge_source_buttons(const flydigi_rx_state_t *rx);

/* 输入状态 → 输出状态（含 5 个额外扩展键映射到原空槽） */
void bridge_map(const flydigi_rx_state_t *rx, vader2pro_state_t *out);

/* 一步到位：输入状态 → 10 字节 HID 输入报告 */
void bridge_pack(const flydigi_rx_state_t *rx, uint8_t out[VADER2PRO_REPORT_BYTES]);
