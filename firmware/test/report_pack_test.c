/* report_pack_test.c — 主机侧自测：位偏移断言 + 打包/解包往返 + 与 Python 参考实现比对用输出
 * 编译：clang -std=c11 -Wall -Wextra -o build/report_pack_test test/report_pack_test.c src/report_pack.c -I. -Isrc
 */
#include <stdio.h>
#include <string.h>
#include "vader2pro_hid.h"
#include "../desc_vader2pro.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  ✘ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void print_hex(const char *label, const uint8_t *p, unsigned n)
{
    printf("%s", label);
    for (unsigned i = 0; i < n; i++) printf("%02x%s", p[i], i + 1 < n ? " " : "");
    printf("\n");
}

/* 断言某按钮落在哪个比特位（独立于场景，直接量描述符布局） */
static void check_button_bit(unsigned btn_index, unsigned expect_bit)
{
    /* 位置 = 56 + btn_index（20 个按钮从 bit56 起，1 bit 一个） */
    unsigned bit = 56 + btn_index;
    vader2pro_state_t s = {0};
    s.buttons[btn_index] = true;
    uint8_t r[VADER2PRO_REPORT_BYTES];
    vader2pro_pack(&s, r);
    bool ok_bit = (r[bit >> 3] >> (bit & 7)) & 1u;
    bool ok_expect = bit == expect_bit;
    CHECK(ok_bit && ok_expect, "按钮 index %u 应在 bit %u（实测 %u）", btn_index, expect_bit, bit);
}

int main(void)
{
    printf("描述符 %u 字节 / 报告 %u 字节\n", (unsigned)sizeof(kDescVader2Pro), VADER2PRO_REPORT_BYTES);
    print_hex("DESC: ", kDescVader2Pro, (unsigned)sizeof(kDescVader2Pro));

    /* 1) 背键与标准键的位位置 */
    check_button_bit(V2P_BTN_A, 56);
    check_button_bit(V2P_BTN_Y, 60);
    check_button_bit(V2P_BTN_START, 67);
    check_button_bit(V2P_BTN_M1, 70);
    check_button_bit(V2P_BTN_M2, 71);
    check_button_bit(V2P_BTN_M3, 72);
    check_button_bit(V2P_BTN_M4, 73);
    check_button_bit(V2P_BTN_HOME, 75);

    /* 2) hat 在 bit48..51 */
    vader2pro_state_t h = {0}; h.hat = 5;
    uint8_t rh[VADER2PRO_REPORT_BYTES];
    vader2pro_pack(&h, rh);
    CHECK(rh[6] == 0x05, "hat=5 应落在 byte6 低半字节（实测 0x%02x）", rh[6]);

    /* 3) 场景报文（与 firmware/dryrun_report.py 同一场景，逐字节比对由 verify.sh 做） */
    vader2pro_state_t s = {0};
    s.lx = 64; s.ly = 160; s.rx = 128; s.ry = 128; s.rt = 255; s.lt = 0; s.hat = 0;
    s.buttons[V2P_BTN_M1] = true; s.buttons[V2P_BTN_M3] = true;
    s.buttons[V2P_BTN_A] = true; s.buttons[V2P_BTN_Y] = true; s.buttons[V2P_BTN_START] = true;
    uint8_t r[VADER2PRO_REPORT_BYTES];
    vader2pro_pack(&s, r);
    print_hex("REPORT: ", r, VADER2PRO_REPORT_BYTES);
    const uint8_t expect[VADER2PRO_REPORT_BYTES] = {0x40,0xa0,0x80,0x80,0xff,0x00,0x00,0x11,0x48,0x01};
    CHECK(memcmp(r, expect, VADER2PRO_REPORT_BYTES) == 0, "场景报文与预期不一致");

    /* 4) 往返一致性（含极端值） */
    vader2pro_state_t rt = {0};
    vader2pro_unpack(r, &rt);
    CHECK(memcmp(&rt, &s, sizeof(s)) == 0, "pack/unpack 往返不一致");
    vader2pro_state_t all = {0};
    all.lx = 0; all.ly = 255; all.rx = 255; all.ry = 0; all.rt = 255; all.lt = 255; all.hat = 7;
    for (unsigned i = 0; i < V2P_BTN_COUNT; i++) all.buttons[i] = true;
    uint8_t ra[VADER2PRO_REPORT_BYTES]; vader2pro_pack(&all, ra);
    vader2pro_state_t back = {0}; vader2pro_unpack(ra, &back);
    CHECK(memcmp(&back, &all, sizeof(all)) == 0, "全按下往返不一致");
    CHECK(ra[7] == 0xff && ra[8] == 0xff && ra[9] == 0x0f,
          "20 按钮全按下应为 byte7=ff byte8=ff byte9=0f（实测 %02x %02x %02x）", ra[7], ra[8], ra[9]);

    printf("结果: %s\n", failures ? "失败" : "全部通过 ✔");
    return failures ? 1 : 0;
}
