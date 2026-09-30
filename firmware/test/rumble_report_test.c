/* rumble_report_test.c — host-side coverage for all accepted Output formats */
#include <stdio.h>
#include <string.h>
#include "../src/rumble_report.h"

static unsigned failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
} } while (0)
#define SEND_DUE(l, r, last_l, last_r, elapsed, minimum, first) \
    flydigi_rumble_send_due(l, r, last_l, last_r, elapsed, minimum, first, 100000u, 0)

static void check_packet(const uint8_t *packet, uint16_t len,
                         uint8_t want_left, uint8_t want_right, const char *name)
{
    uint8_t left = 0, right = 0;
    CHECK(flydigi_rumble_decode(packet, len, &left, &right), "%s 应被接受", name);
    CHECK(left == want_left && right == want_right,
          "%s 应解码为 %02x/%02x，实际 %02x/%02x", name,
          want_left, want_right, left, right);
}

int main(void)
{
    static const uint8_t flydigi[] = { 0x5a, 0xa5, 0x12, 0x06, 0x12, 0xe7 };
    static const uint8_t prefixed[] = { 0x03, 0x5a, 0xa5, 0x12, 0x06, 0x34, 0x56 };
    static const uint8_t xinput[] = { 0x00, 0x08, 0x00, 0x9a, 0x2b, 0, 0, 0 };
    static const uint8_t malformed[] = { 0x5a, 0xa5, 0x12, 0x06, 0x12 };
    static const uint8_t unknown[] = { 0x01, 0x08, 0x00, 0x9a, 0x2b };

    check_packet(flydigi, sizeof flydigi, 0x12, 0xe7, "Flydigi 5a a5");
    check_packet(prefixed, sizeof prefixed, 0x34, 0x56, "前导 03");
    check_packet(xinput, sizeof xinput, 0x9a, 0x2b, "XInput 00 08");
    check_packet((const uint8_t[]){ 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0 }, 8,
                 0, 0, "XInput 停止包");

    uint8_t left = 0xaa, right = 0xbb;
    CHECK(!flydigi_rumble_decode(malformed, sizeof malformed, &left, &right),
          "截断 Flydigi 包必须拒绝");
    CHECK(!flydigi_rumble_decode(unknown, sizeof unknown, &left, &right),
          "未知包头必须拒绝");
    CHECK(!flydigi_rumble_decode(NULL, 0, &left, &right), "空指针必须拒绝");

    static const uint8_t stadia_control[] = { 0xff, 0xff, 0x00, 0x80 };
    static const uint8_t stadia_interrupt[] = { 0x05, 0x00, 0x40, 0x00, 0x00 };
    CHECK(flydigi_stadia_haptic_decode(5, stadia_control,
                                      sizeof stadia_control, &left, &right) &&
          left == 255 && right == 128,
          "Stadia 控制报告双通道 16-bit → 8-bit: %u/%u", left, right);
    CHECK(flydigi_stadia_haptic_decode(0, stadia_interrupt,
                                      sizeof stadia_interrupt, &left, &right) &&
          left == 64 && right == 0,
          "Stadia 中断 OUT 包含 Report ID: %u/%u", left, right);
    CHECK(flydigi_stadia_haptic_decode(5, (const uint8_t[]){0, 0, 0, 0},
                                      4, &left, &right) && left == 0 && right == 0,
          "Stadia 停振包应同时清零");
    left = 17; right = 29;
    CHECK(!flydigi_stadia_haptic_decode(4, stadia_control, 4, &left, &right) &&
          left == 17 && right == 29, "Stadia 错误 Report ID 必须拒绝");
    CHECK(!flydigi_stadia_haptic_decode(5, stadia_control, 3, &left, &right) &&
          left == 17 && right == 29, "Stadia 截断报告必须拒绝");

    const uint32_t min_interval_us = 1000000;
    CHECK(SEND_DUE(180, 180, 0, 0, 0, min_interval_us, true),
          "首包立即发送");
    CHECK(SEND_DUE(180, 180, 0, 0, 1000, min_interval_us, false),
          "停振后的新脉冲立即启动");
    CHECK(SEND_DUE(0, 0, 180, 180, 250000, min_interval_us, false),
          "看门狗到期时不能被最小间隔拖延");
    CHECK(SEND_DUE(0, 180, 180, 180, 1000, min_interval_us, false),
          "单侧马达停振立即发送");
    CHECK(SEND_DUE(100, 180, 180, 180, 1000, min_interval_us, false),
          "降低马达强度立即发送");
    CHECK(!SEND_DUE(0, 0, 0, 0, 1000, min_interval_us, false),
          "失败的停振包短暂退避");
    CHECK(SEND_DUE(0, 0, 0, 0, 4000, min_interval_us, false),
          "失败的停振包最多 4 ms 后重试");
    CHECK(!SEND_DUE(220, 180, 180, 180, 1000, min_interval_us, false),
          "增加马达强度遵守最小间隔");
    CHECK(!SEND_DUE(180, 180, 180, 180, 1000, min_interval_us, false),
          "重复强度遵守最小间隔");
    CHECK(SEND_DUE(220, 180, 180, 180, min_interval_us,
                   min_interval_us, false),
          "到达最小间隔后发送更新");
    CHECK(!flydigi_rumble_send_due(180, 180, 180, 180, 1000, min_interval_us,
                                   false, 100000u, 103000u),
          "失败的启动包在退避期内不重试");
    CHECK(flydigi_rumble_send_due(180, 180, 180, 180, 4000, min_interval_us,
                                  false, 104000u, 103000u),
          "失败的启动包不受长间隔阻塞");
    CHECK(flydigi_rumble_send_due(0, 0, 180, 180, 1000, min_interval_us,
                                  false, 100000u, 103000u),
          "退避期间的停振优先发送");

    printf("震动报告与调度: %s\n", failures ? "失败" : "全部通过");
    return failures ? 1 : 0;
}
