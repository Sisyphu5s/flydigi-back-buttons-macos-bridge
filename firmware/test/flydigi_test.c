/* flydigi_test.c — 输入侧（接收器扩展帧 + 命令表）与映射层的纯主机验证
 * 编译: clang -std=c11 -Wall -Wextra -I../src flydigi_test.c ../src/flydigi_rx.c ../src/bridge_map.c ../src/report_pack.c -o flydigi_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "flydigi_rx.h"
#include "bridge_map.h"
#include "vader2pro_hid.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (cond) { printf("  \xe2\x9c\x94 "); } else { fails++; printf("  \xe2\x9c\x98 "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* 真机帧：2026-09-29 板子 stage-2 固件（开机瞬间唯一一帧 5a a5 ef），docs/02 §7.1 */
static const uint8_t REAL[32] = {
    0x5a, 0xa5, 0xef,              /* 魔数 */
    0x09, 0x00, 0x08, 0x00,        /* LX=9, LY=8 */
    0xe5, 0xff, 0xe2, 0xff,        /* RX=-27, RY=-30 */
    0x00, 0x00, 0x00, 0x08,        /* b1, b2, ext1(无背键), ext2 */
    0x00, 0x00,                    /* LT=0, RT=0 */
    0xf4, 0xff, 0xa1, 0x00, 0x34, 0x01,   /* 陀螺 XYZ */
    0x63, 0xff, 0xef, 0x00, 0xa7, 0x11,   /* 加速度 XYZ（Z≈1.1g） */
    0x00, 0x00, 0x9f,              /* 保留 + 疑为校验 */
};

static void hex_report(const uint8_t *b, char *out)
{
    for (int i = 0; i < VADER2PRO_REPORT_BYTES; i++)
        sprintf(out + 3 * i, "%02x ", b[i]);
    out[3 * VADER2PRO_REPORT_BYTES - 1] = '\0';
}

static void update_frame_checksum(uint8_t frame[FD_RX_FRAME_BYTES])
{
    uint8_t checksum = 1;
    for (size_t i = 0; i < FD_RX_FRAME_BYTES - 1; i++) checksum += frame[i];
    frame[FD_RX_FRAME_BYTES - 1] = checksum;
}

int main(void)
{
    printf("== 1. 解析真机扩展帧 ==\n");
    flydigi_rx_state_t s;
    memset(&s, 0, sizeof s);
    CHECK(flydigi_rx_parse(REAL, sizeof REAL, &s), "魔数 5a a5 ef 通过");
    CHECK(s.lx == 9 && s.ly == 8, "左摇杆 LX=%d LY=%d（期望 9 / 8）", s.lx, s.ly);
    CHECK(s.rx == -27 && s.ry == -30, "右摇杆 RX=%d RY=%d（期望 -27 / -30）", s.rx, s.ry);
    CHECK(s.lt == 0 && s.rt == 0, "扳机 LT=%u RT=%u", s.lt, s.rt);
    CHECK(s.ext1 == 0x00, "扩展按键 [13]=0x%02x（未按键 ⇒ 无背键）", s.ext1);
    CHECK(s.ext2 == 0x08, "扩展按键2 [14]=0x%02x（空闲帧也为 0x08 ⇒ Home 位语义存疑）", s.ext2);
    double gz = s.accel[2] / 4096.0;
    CHECK(gz > 0.9 && gz < 1.3, "加速度 Z=%.2f g（静止应 ≈1g ⇒ 位布局自洽）", gz);
    CHECK(!flydigi_m_pressed(&s, 1) && !flydigi_m_pressed(&s, 4), "M1/M4 未按下");
    uint8_t rep[VADER2PRO_REPORT_BYTES];

    flydigi_rx_state_t base = s, ext = s, merged;
    base.lx = 12000; base.ly = -9000; base.rx = 7000; base.ry = -5000;
    base.lt = 255; base.rt = 255; base.guide = true;
    ext.lx = 1000; ext.ly = 2000; ext.rx = 3000; ext.ry = 4000;
    ext.lt = 12; ext.rt = 34; ext.ext1 = FD_EXT_M1; ext.ext2 = 0;
    CHECK(!flydigi_rx_merge(&base, true, 1000, &ext, true, 5000, false, &merged) &&
          merged.lx == ext.lx && merged.lt == ext.lt && merged.ext1 == FD_EXT_M1 &&
          !merged.guide, "默认 EF 优先且不引入 XInput 扳机/Home");
    base.b1 = FD_B1_A | FD_B1_DPAD_UP;
    base.b2 = FD_B2_RB;
    ext.b1 = 0;
    ext.b2 = 0;
    CHECK(!flydigi_rx_merge(&base, true, 1000, &ext, true, 5000, false, &merged) &&
          merged.b1 == base.b1 && merged.b2 == base.b2 &&
          merged.ext1 == FD_EXT_M1,
          "EF 标准键为空时合并新鲜 XInput 的 A/D-pad/RB");
    base.b1 = FD_B1_A | FD_B1_DPAD_UP;
    base.b2 = FD_B2_RB;
    ext.b1 = FD_B1_B;
    ext.b2 = FD_B2_LB;
    CHECK(!flydigi_rx_merge(&base, true, 1000, &ext, true, 5000, false, &merged) &&
          merged.b1 == (uint8_t)(FD_B1_A | FD_B1_B | FD_B1_DPAD_UP) &&
          merged.b2 == (uint8_t)(FD_B2_LB | FD_B2_RB),
          "EF 与 XInput 的标准键位按归一化位域合并");
    CHECK(flydigi_rx_merge(&base, true, 1000, &ext, true, 5000, true, &merged) &&
          merged.lx == base.lx && merged.ly == base.ly &&
          merged.rx == base.rx && merged.ry == base.ry &&
          merged.lt == ext.lt && merged.rt == ext.rt && merged.ext1 == FD_EXT_M1 &&
          !merged.guide && merged.gyro[0] == ext.gyro[0],
          "可选模式只取较新的 XInput 摇杆轴");
    CHECK(!flydigi_rx_merge(&base, true, 5000, &ext, true, 1000, true, &merged) &&
          merged.lx == ext.lx, "较旧的 XInput 不覆盖 EF 摇杆轴");
    CHECK(!flydigi_rx_merge(&base, true, 1000, &ext, false, 250000, true, &merged) &&
          merged.lx == base.lx && merged.lt == base.lt && merged.guide,
          "EF 过期后恢复完整 XInput 兜底");
    CHECK(!flydigi_rx_merge(&base, false, 250000, &ext, false, 250000, true, &merged) &&
          merged.lx == 0 && merged.lt == 0 && !merged.guide,
          "两个输入源均过期时释放全部输入");

    static const uint8_t trigger_spike[20] = {
        0x00, 0x14, 0x00, 0x00, 0xd6, 0xff,
    };
    flydigi_rx_state_t neutral_ext = {0};
    CHECK(flydigi_rx_xinput_idle_spike(trigger_spike, sizeof trigger_spike,
                                       &neutral_ext, true),
          "真机双扳机尖峰在 EF 确认松开时被过滤");
    CHECK(!flydigi_rx_xinput_idle_spike(trigger_spike, sizeof trigger_spike,
                                        &neutral_ext, false),
          "EF 不新鲜时保留 XInput 兜底");
    neutral_ext.lt = 255;
    CHECK(!flydigi_rx_xinput_idle_spike(trigger_spike, sizeof trigger_spike,
                                        &neutral_ext, true),
          "EF 确认扳机按下时不丢合法 XInput 帧");
    neutral_ext.lt = 0;
    uint8_t with_stick[20];
    memcpy(with_stick, trigger_spike, sizeof with_stick);
    with_stick[6] = 1;
    CHECK(!flydigi_rx_xinput_idle_spike(with_stick, sizeof with_stick,
                                        &neutral_ext, true),
          "XInput 摇杆变化时不套用空闲尖峰规则");

    printf("\n== 1a. XInput D-pad U/D/L/R → EF U/R/D/L ==\n");
    static const uint16_t xinput_dpad_bits[] = {
        1u << 0, 1u << 3, 1u << 1, 1u << 2,
    };
    static const uint8_t normalized_dpad_bits[] = {
        FD_B1_DPAD_UP, FD_B1_DPAD_RIGHT, FD_B1_DPAD_DOWN, FD_B1_DPAD_LEFT,
    };
    for (unsigned i = 0; i < 4; i++) {
        uint8_t x_b1 = 0, x_b2 = 0;
        flydigi_xinput_normalize_buttons(xinput_dpad_bits[i], &x_b1, &x_b2);
        CHECK(x_b1 == normalized_dpad_bits[i] && x_b2 == 0,
              "XInput D-pad 方向 %u 归一化为 EF 位 0x%02x", i, x_b1);
    }
    CHECK(flydigi_xinput_buttons_valid(FD_XINPUT_KNOWN_BUTTON_MASK),
          "XInput 已定义按钮位全部可接受");
    CHECK(!flydigi_xinput_buttons_valid(0xff00),
          "XInput 保留 bit11 的异常包被拒绝");

    printf("\n== 1b. EF D-pad 原始位序 U/R/D/L ==\n");
    static const uint8_t raw_dpad_bits[] = {
        FD_B1_DPAD_UP, FD_B1_DPAD_RIGHT, FD_B1_DPAD_DOWN, FD_B1_DPAD_LEFT,
    };
    static const uint8_t raw_dpad_hat[] = { 0, 2, 4, 6 };
    for (unsigned i = 0; i < 4; i++) {
        uint8_t frame[32];
        memcpy(frame, REAL, sizeof frame);
        frame[11] = raw_dpad_bits[i];
        update_frame_checksum(frame);
        flydigi_rx_state_t dpad;
        memset(&dpad, 0, sizeof dpad);
        CHECK(flydigi_rx_parse(frame, sizeof frame, &dpad), "D-pad 原始位 %u 可解析", i);
        bridge_pack(&dpad, rep);
        vader2pro_state_t dpad_state;
        vader2pro_unpack(rep, &dpad_state);
        CHECK(dpad_state.hat == raw_dpad_hat[i], "D-pad 原始位 %u → hat=%u", i, dpad_state.hat);
    }
    /* Real synchronized samples: EF byte[11] 0x01=up and 0x02=right. */
    CHECK(flydigi_hat_from_b1(0x01) == 0, "真机 EF byte[11]=0x01 → 上");
    CHECK(flydigi_hat_from_b1(0x02) == 2, "真机 EF byte[11]=0x02 → 右");
    CHECK(flydigi_hat_from_b1(0x04) == 4, "EF byte[11]=0x04 → 下");
    CHECK(flydigi_hat_from_b1(0x08) == 6, "EF byte[11]=0x08 → 左");

    printf("\n== 2. 坏帧必须被拒 ==\n");
    uint8_t bad[32]; memcpy(bad, REAL, 32); bad[2] = 0x01;
    CHECK(!flydigi_rx_parse(bad, sizeof bad, &s), "魔数不符（5a a5 01）→ 拒绝");
    memcpy(bad, REAL, 32); bad[15] = 255;
    CHECK(!flydigi_rx_parse(bad, sizeof bad, &s), "扳机字段损坏但尾校验未更新 → 拒绝");
    CHECK(!flydigi_rx_parse(REAL, 31, &s), "长度不足 32 → 拒绝");
    CHECK(flydigi_rx_parse(REAL, 32, &s) && s.ext1 == 0x00, "拒绝后 state 未被污染");

    printf("\n== 3. 撤销/状态帧（固件当前只能拿到的 `5a a5 01 01`）必须被拒 ==\n");
    static const uint8_t STATUS[32] = {
        0x5a, 0xa5, 0x01, 0x01, 0x00, 0x82, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x45, 0x01, 0x00, 0x72,
        0x26, 0x04, 0x83, 0x36, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x43, 0x27, 0x00, 0xa9
    };
    CHECK(!flydigi_rx_parse(STATUS, sizeof STATUS, &s), "状态帧 → 拒绝（不是输入帧）");
    uint8_t battery_percent = 0xff, power_state = 0xff;
    CHECK(flydigi_rx_battery_status(STATUS, sizeof STATUS, &battery_percent, &power_state) &&
          battery_percent == 100 && power_state == 0,
          "真机 device-info 帧电量 0x0a → 100%%");
    uint8_t invalid_status[sizeof STATUS];
    memcpy(invalid_status, STATUS, sizeof invalid_status);
    invalid_status[11] = 0xfa;
    update_frame_checksum(invalid_status);
    battery_percent = 60;
    power_state = 1;
    CHECK(!flydigi_rx_battery_status(invalid_status, sizeof invalid_status,
                                     &battery_percent, &power_state) &&
          battery_percent == 60 && power_state == 1,
          "无效电量不会覆盖上一条有效读数");
    memcpy(invalid_status, STATUS, sizeof invalid_status);
    invalid_status[11] = 0x09;
    CHECK(!flydigi_rx_battery_status(invalid_status, sizeof invalid_status,
                                     &battery_percent, &power_state) &&
          battery_percent == 60 && power_state == 1,
          "校验错误但数值合法的电量帧不会覆盖读数");
    CHECK(!flydigi_rx_battery_status(STATUS, sizeof STATUS - 1,
                                     &battery_percent, &power_state) &&
          battery_percent == 60 && power_state == 1,
          "截断的电量帧不会覆盖有效读数");

    printf("\n== 4. 命令表（初始化握手 + test mode）==\n");
    CHECK(FD_CMDS_COUNT == 6, "共 %zu 条命令", FD_CMDS_COUNT);
    static const struct { const char *hex; } want[] = {
        { "5a a5 01 02 03" }, { "5a a5 a1 02 a3" }, { "5a a5 02 02 04" }, { "5a a5 04 02 06" },
        { "5a a5 11 07 ff 01 ff ff ff 15 00" }, { "5a a5 11 07 ff 00 ff ff ff 14 00" },
    };
    uint8_t cmd[FD_CMD_BYTES];
    for (size_t i = 0; i < FD_CMDS_COUNT; i++) {
        flydigi_cmd_pack(&FD_CMDS[i], cmd);
        char got[128] = {0};
        for (int j = 0; j < FD_CMDS[i].len; j++) sprintf(got + 3 * j, "%02x ", cmd[j]);
        got[3 * FD_CMDS[i].len - 1] = '\0';
        CHECK(strcmp(got, want[i].hex) == 0, "%s → %s", FD_CMDS[i].name, got);
        int tail_ok = 1;
        for (int j = FD_CMDS[i].len; j < FD_CMD_BYTES; j++) if (cmd[j] != 0) tail_ok = 0;
        CHECK(tail_ok, "  %s：尾部补 0 至 32 字节", FD_CMDS[i].name);
    }

    printf("\n== 5. 映射：真机帧 → %u 字节 HID 报文 ==\n", VADER2PRO_REPORT_BYTES);
    char line[64];
    bridge_pack(&s, rep);
    hex_report(rep, line);
#ifdef GENERIC_GAMEPAD
    CHECK(strcmp(line, "80 80 80 80 00 00 08 00 00 00 00") == 0, "回报 %s", line);
#else
    CHECK(strcmp(line, "80 80 80 80 00 00 08 00 00 00") == 0, "回报 %s", line);
#endif
    vader2pro_state_t vs;
    vader2pro_unpack(rep, &vs);
    CHECK(vs.lx == 128 && vs.hat == V2P_HAT_NEUTRAL, "轴中心=128, hat=中位(%u)", vs.hat);
    CHECK(!vs.buttons[V2P_BTN_HOME],
          "Home 空闲未按下（EF byte14=0x08 不再被误判）");

    flydigi_rx_state_t guide = s;
    guide.guide = true;
    bridge_pack(&guide, rep);
    vader2pro_unpack(rep, &vs);
    CHECK(vs.buttons[V2P_BTN_HOME], "接口 0 XInput Guide bit10 → Home 按钮");

    printf("\n== 6. 背键 M1–M4 → 当前身份的独立按钮位 ==\n");
    flydigi_rx_state_t m = s;
    m.ext1 = FD_EXT_M1 | FD_EXT_M2 | FD_EXT_M3 | FD_EXT_M4;
    m.b1 = FD_B1_A | FD_B1_DPAD_UP;
    CHECK(bridge_source_buttons(&m) == ((1u << BRIDGE_SRC_A) |
          (1u << BRIDGE_SRC_DPAD_UP) |
          (1u << BRIDGE_SRC_M1) | (1u << BRIDGE_SRC_M2) |
          (1u << BRIDGE_SRC_M3) | (1u << BRIDGE_SRC_M4)),
          "WebHID 原始按键位图包含 A、D-pad 和全部背键");
    bridge_pack(&m, rep);
    hex_report(rep, line);
#ifdef CORSAIR_APPLE_IDENTITY
    CHECK(strcmp(line, "80 80 80 80 00 00 00 01 00 f0 00") == 0,
          "SCUF-order 29-button M1–M4+A+上 → %s", line);
#elif defined(GENERIC_APPLE_IDENTITY)
    CHECK(strcmp(line, "80 80 80 80 00 00 00 01 c0 03 00") == 0,
          "Apple-order 26-button M1–M4+A+上 → %s", line);
#elif defined(GENERIC_GAMEPAD)
    CHECK(strcmp(line, "80 80 80 80 00 00 00 01 10 1e 00") == 0, "M1–M4+A+上 → %s", line);
#else
    CHECK(strcmp(line, "80 80 80 80 00 00 00 01 c0 03") == 0, "M1–M4+A+上 → %s", line);
#endif
    vader2pro_unpack(rep, &vs);
    CHECK(vs.buttons[V2P_BTN_M1] && vs.buttons[V2P_BTN_M2] &&
          vs.buttons[V2P_BTN_M3] && vs.buttons[V2P_BTN_M4], "M1..M4 四个按钮位全为 1");
    CHECK(vs.buttons[V2P_BTN_A] && !vs.buttons[V2P_BTN_B], "A=1, B=0");
    CHECK(vs.hat == 0, "hat=上(0)");
    /* 逐个背键：位不串扰 */
#ifdef CORSAIR_APPLE_IDENTITY
    CHECK(rep[9] == 0xf0, "SCUF-order paddles occupy Button 21..24");
#elif defined(GENERIC_APPLE_IDENTITY)
    CHECK((rep[8] & 0xc0) == 0xc0 && (rep[9] & 0x03) == 0x03,
          "Apple-order paddles retain byte8/9 positions");
#elif defined(GENERIC_GAMEPAD)
    CHECK((rep[9] & 0x1e) == 0x1e, "Generic M1–M4 位于 byte9 bits1..4");
#else
    CHECK((rep[8] & 0xc0) == 0xc0 && (rep[9] & 0x03) == 0x03, "byte8=0x%02x byte9=0x%02x（背键落点）", rep[8], rep[9]);
#endif
    const unsigned paddle_slots[] = { V2P_BTN_M1, V2P_BTN_M2, V2P_BTN_M3, V2P_BTN_M4 };
    for (unsigned k = 0; k < 4; k++) {
        flydigi_rx_state_t one = s;
        one.ext1 = (uint8_t)(FD_EXT_M1 << k);
        bridge_pack(&one, rep);
        vader2pro_unpack(rep, &vs);
        int cnt = 0;
        for (unsigned i = 0; i < V2P_BTN_COUNT; i++) cnt += vs.buttons[i];
        CHECK(vs.buttons[paddle_slots[k]] && cnt == 1, "只按 M%u → 恰好 1 个按钮位", k + 1);
    }
    flydigi_rx_state_t extras = s;
    extras.ext1 = FD_EXT_C | FD_EXT_Z | FD_EXT_LM | FD_EXT_RM;
    extras.ext2 = FD_EXT2_O;
    CHECK(bridge_source_buttons(&extras) == ((1u << BRIDGE_SRC_C) |
          (1u << BRIDGE_SRC_Z) | (1u << BRIDGE_SRC_LM) |
          (1u << BRIDGE_SRC_RM) | (1u << BRIDGE_SRC_O)),
          "WebHID 原始按键位图包含最高位 O");
    bridge_pack(&extras, rep);
    vader2pro_unpack(rep, &vs);
    CHECK(vs.buttons[V2P_BTN_C] && vs.buttons[V2P_BTN_Z] &&
          vs.buttons[V2P_BTN_LM] && vs.buttons[V2P_BTN_RM] && vs.buttons[V2P_BTN_O],
          "C/Z/LM/RM/O → 原空槽按钮位");
#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
    flydigi_rx_state_t digital = s;
    digital.lt = 127; digital.rt = 128;
    digital.b1 = FD_B1_DPAD_UP | FD_B1_DPAD_RIGHT;
    bridge_pack(&digital, rep);
    vader2pro_unpack(rep, &vs);
    CHECK(!vs.buttons[V2P_BTN_LT] && vs.buttons[V2P_BTN_RT],
          "扳机数字按钮阈值 128，模拟轴仍保留");
    CHECK(vs.buttons[V2P_BTN_DPAD_UP] && vs.buttons[V2P_BTN_DPAD_RIGHT] &&
          !vs.buttons[V2P_BTN_DPAD_DOWN] && !vs.buttons[V2P_BTN_DPAD_LEFT],
          "十字键对角同时映射两个标准索引按钮");

    flydigi_rx_state_t remapped = s;
    remapped.b1 = FD_B1_DPAD_UP;
    bridge_config_t remap_cfg;
    bridge_config_get(&remap_cfg);
    remap_cfg.button_map[BRIDGE_SRC_DPAD_UP] = V2P_BTN_O;
    remap_cfg.button_map[BRIDGE_SRC_DPAD_DOWN] = BRIDGE_BUTTON_DISABLED;
    remap_cfg.button_map[BRIDGE_SRC_DPAD_LEFT] = BRIDGE_BUTTON_DISABLED;
    remap_cfg.button_map[BRIDGE_SRC_DPAD_RIGHT] = BRIDGE_BUTTON_DISABLED;
    remap_cfg.flags |= BRIDGE_FLAG_GENERIC_DPAD_MAPPING;
    bridge_config_finalize(&remap_cfg);
    CHECK(bridge_config_set(&remap_cfg), "apply explicit D-pad mapping");
    bridge_pack(&remapped, rep);
    vader2pro_unpack(rep, &vs);
    CHECK(vs.buttons[V2P_BTN_O] && !vs.buttons[V2P_BTN_DPAD_UP],
          "explicit D-pad mapping moves Up without fixed standard copy");
    bridge_config_defaults(&remap_cfg);
    CHECK(bridge_config_set(&remap_cfg), "restore default D-pad mapping");
#endif

    printf("\n== 7. hat 八向 + 反方向对 ==\n");
    struct { uint8_t b1; uint8_t want; const char *name; } hat[] = {
        { FD_B1_DPAD_UP, 0, "上" }, { FD_B1_DPAD_UP | FD_B1_DPAD_RIGHT, 1, "右上" },
        { FD_B1_DPAD_RIGHT, 2, "右" }, { FD_B1_DPAD_RIGHT | FD_B1_DPAD_DOWN, 3, "右下" },
        { FD_B1_DPAD_DOWN, 4, "下" }, { FD_B1_DPAD_DOWN | FD_B1_DPAD_LEFT, 5, "左下" },
        { FD_B1_DPAD_LEFT, 6, "左" }, { FD_B1_DPAD_LEFT | FD_B1_DPAD_UP, 7, "左上" },
        { 0, 8, "中位" }, { FD_B1_DPAD_UP | FD_B1_DPAD_DOWN, 8, "上+下(反方向对)" },
        { FD_B1_DPAD_LEFT | FD_B1_DPAD_RIGHT, 8, "左+右(反方向对)" },
    };
    for (unsigned i = 0; i < sizeof hat / sizeof hat[0]; i++)
        CHECK(flydigi_hat_from_b1(hat[i].b1) == hat[i].want, "%s → %u", hat[i].name, hat[i].want);

    printf("\n== 8. 轴量程边界 + 扳机直通 ==\n");
    flydigi_rx_state_t ax = s;
    ax.lx = 32767; ax.ly = -32768; ax.rx = 0; ax.ry = 1;
    ax.lt = 0; ax.rt = 255;
    bridge_pack(&ax, rep);
    hex_report(rep, line);
    CHECK(rep[0] == 255 && rep[1] == 255 && rep[2] == 128 && rep[3] == 128, "极值映射 %s", line);
    CHECK(rep[4] == 255 && rep[5] == 0, "RT=255 LT=0 直通");

    printf("\n%s: %d 项检查，%d 失败\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
