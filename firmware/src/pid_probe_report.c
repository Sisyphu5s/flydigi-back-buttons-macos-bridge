#include "pid_probe_report.h"

static int16_t signed_axis(uint8_t value)
{
    return value < 128 ? (int16_t)(((int32_t)value - 128) * 32767 / 128) :
                         (int16_t)(((int32_t)value - 128) * 32767 / 127);
}

void pid_probe_pack_input(const uint8_t generic[11], uint8_t out[PID_PROBE_INPUT_BYTES])
{
    for (unsigned i = 0; i < 4; i++) out[i] = generic[7 + i];
    out[4] = generic[6] & 0x0f;
    for (unsigned i = 0; i < 6; i++) {
        uint16_t const axis = (uint16_t)signed_axis(generic[i]);
        out[5 + 2 * i] = (uint8_t)axis;
        out[6 + 2 * i] = (uint8_t)(axis >> 8);
    }
}
