#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "pid_probe_report.h"

int main(void)
{
    uint8_t generic[11] = {128, 0, 255, 128, 0, 255, 8, 0x01, 0x80, 0x00, 0x02};
    uint8_t out[PID_PROBE_INPUT_BYTES];
    pid_probe_pack_input(generic, out);
    assert(out[0] == 0x01 && out[1] == 0x80 && out[2] == 0 && out[3] == 0x02);
    assert(out[4] == 8);
    assert(out[5] == 0 && out[6] == 0);
    assert(out[7] == 0x01 && out[8] == 0x80); /* -32767 */
    assert(out[9] == 0xff && out[10] == 0x7f); /* +32767 */
    assert(out[13] == 0x01 && out[14] == 0x80);
    assert(out[15] == 0xff && out[16] == 0x7f);
    puts("PID probe input packing: PASS");
    return 0;
}
