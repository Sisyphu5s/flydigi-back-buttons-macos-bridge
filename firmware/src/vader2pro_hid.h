/* vader2pro_hid.h — 冒充 Flydigi Vader2Pro.MobileUSB 的报文布局与按钮映射
 *
 * 依据（一手，见 docs/11 §2/§4 与 firmware/report-verification.md）：
 *   匹配键：idVendor=0x04B4, idProduct=0x2412, bcdDevice=0x0500
 *   模型：  GameControllers-Custom.bundle/Personalities/Flydigi/Vader2Pro/MobileUSBWithBackButtons.plist
 *   语义：  UsageType = 元素类别 {1=Button, 2=Analog, 3=Hat}；
 *           UsageTypeIndex = 该类元素在描述符中的 0-based 位置（位置敏感！）
 *
 * 10 字节输入报告（无 Report ID，Generic 变体为 11 字节）：
 *   byte0 LX | byte1 LY | byte2 RX | byte3 RY | byte4 RT | byte5 LT
 *   byte6 [7:4]=填充, [3:0]=hat(0..7, 8=中位)
 *   byte7 按钮 1..8 | byte8 按钮 9..16 | byte9 [3:0]=按钮 17..20, [7:4]=填充
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef GENERIC_GAMEPAD
#define VADER2PRO_REPORT_BYTES 11
#ifdef CORSAIR_APPLE_IDENTITY
#define V2P_BTN_COUNT 29
#else
#define V2P_BTN_COUNT 26
#endif
#else
#define VADER2PRO_REPORT_BYTES 10
#define V2P_BTN_COUNT 20
#endif

/* 按钮位序号 = 描述符里的位置（0-based）；也是 Button page usage−1 */
#ifdef GENERIC_GAMEPAD
#ifdef CORSAIR_APPLE_IDENTITY
/* Apple scuff-omega model: P1..P4 map to M1/M3/M4/M2; G1..G5
 * provide five more independent GC buttons. */
enum {
    V2P_BTN_A = 0, V2P_BTN_B = 1, V2P_BTN_X = 3, V2P_BTN_Y = 4,
    V2P_BTN_LB = 6, V2P_BTN_RB = 7,
    V2P_BTN_SELECT = 10, V2P_BTN_START = 11, V2P_BTN_HOME = 12,
    V2P_BTN_L3 = 13, V2P_BTN_R3 = 14,
    V2P_BTN_M1 = 20, V2P_BTN_M2 = 23, V2P_BTN_M3 = 21, V2P_BTN_M4 = 22,
    V2P_BTN_C = 24, V2P_BTN_Z = 25, V2P_BTN_LM = 26,
    V2P_BTN_RM = 27, V2P_BTN_O = 28,
};
#elif defined(GENERIC_APPLE_IDENTITY)
enum {
    V2P_BTN_A = 0, V2P_BTN_B = 1, V2P_BTN_X = 3, V2P_BTN_Y = 4,
    V2P_BTN_LB = 6, V2P_BTN_RB = 7,
    V2P_BTN_SELECT = 10, V2P_BTN_START = 11,
    V2P_BTN_L3 = 12, V2P_BTN_R3 = 13,
    V2P_BTN_M1 = 14, V2P_BTN_M2 = 15, V2P_BTN_M3 = 16, V2P_BTN_M4 = 17,
    V2P_BTN_HOME = 19,
    V2P_BTN_C = 20, V2P_BTN_Z = 21, V2P_BTN_LM = 22,
    V2P_BTN_RM = 23, V2P_BTN_O = 24,
};
#else
enum {
    V2P_BTN_A = 0, V2P_BTN_B = 1, V2P_BTN_X = 2, V2P_BTN_Y = 3,
    V2P_BTN_LB = 4, V2P_BTN_RB = 5, V2P_BTN_LT = 6, V2P_BTN_RT = 7,
    V2P_BTN_SELECT = 8, V2P_BTN_START = 9,
    V2P_BTN_L3 = 10, V2P_BTN_R3 = 11,
    V2P_BTN_DPAD_UP = 12, V2P_BTN_DPAD_DOWN = 13,
    V2P_BTN_DPAD_LEFT = 14, V2P_BTN_DPAD_RIGHT = 15,
    V2P_BTN_HOME = 16,
    V2P_BTN_M1 = 17, V2P_BTN_M2 = 18, V2P_BTN_M3 = 19, V2P_BTN_M4 = 20,
    V2P_BTN_C = 21, V2P_BTN_Z = 22, V2P_BTN_LM = 23, V2P_BTN_RM = 24, V2P_BTN_O = 25,
};
#endif
#else
enum {
    V2P_BTN_A = 0, V2P_BTN_B = 1, V2P_BTN_X = 3, V2P_BTN_Y = 4,
    V2P_BTN_LB = 6, V2P_BTN_RB = 7,
    V2P_BTN_SELECT = 10, V2P_BTN_START = 11,
    V2P_BTN_L3 = 12, V2P_BTN_R3 = 13,
    V2P_BTN_M1 = 14, V2P_BTN_M2 = 15, V2P_BTN_M3 = 16, V2P_BTN_M4 = 17,
    V2P_BTN_HOME = 19,
    /* Apple model slots remain fixed; extras use its five unused positions. */
    V2P_BTN_C = 2, V2P_BTN_Z = 5, V2P_BTN_LM = 8, V2P_BTN_RM = 9, V2P_BTN_O = 18,
};
#endif

/* hat 取值：0=上 1=右上 2=右 3=右下 4=下 5=左下 6=左 7=左上 8=中位(null) */
#define V2P_HAT_NEUTRAL 8

typedef struct {
    uint8_t lx, ly, rx, ry;   /* 摇杆 0..255，中位 128 */
    uint8_t rt, lt;           /* 扳机 0..255（Ry / Rz） */
    uint8_t hat;              /* 0..7，或 V2P_HAT_NEUTRAL */
    bool buttons[V2P_BTN_COUNT];
} vader2pro_state_t;

/* 把状态打包成当前变体的 HID 输入报告 */
void vader2pro_pack(const vader2pro_state_t *s, uint8_t out[VADER2PRO_REPORT_BYTES]);

/* 诊断用：把报文还原成状态（与 pack 对称，供自测/回归） */
void vader2pro_unpack(const uint8_t in[VADER2PRO_REPORT_BYTES], vader2pro_state_t *s);
