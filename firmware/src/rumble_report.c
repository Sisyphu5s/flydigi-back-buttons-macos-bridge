/* rumble_report.c — protocol decoding and timing independent of TinyUSB callbacks */
#include "rumble_report.h"
#include <stddef.h>

bool flydigi_rumble_decode(const uint8_t *buffer, uint16_t bufsize,
                           uint8_t *left, uint8_t *right)
{
    if (buffer == NULL || left == NULL || right == NULL) return false;

    /* Flydigi's unnumbered command: 5a a5 12 06 LL RR. */
    if (bufsize >= 6 && buffer[0] == 0x5a && buffer[1] == 0xa5 &&
        buffer[2] == 0x12 && buffer[3] == 0x06) {
        *left = buffer[4];
        *right = buffer[5];
        return true;
    }

    /* SDL/ hidraw captures sometimes carry a leading report selector 03. */
    if (bufsize >= 7 && buffer[0] == 0x03 && buffer[1] == 0x5a &&
        buffer[2] == 0xa5 && buffer[3] == 0x12 && buffer[4] == 0x06) {
        *left = buffer[5];
        *right = buffer[6];
        return true;
    }

    /* XInput output report used by the Flydigi receiver: 00 08 00 LL RR... */
    if (bufsize >= 5 && buffer[0] == 0x00 && buffer[1] == 0x08) {
        *left = buffer[3];
        *right = buffer[4];
        return true;
    }

    return false;
}

bool flydigi_stadia_haptic_decode(uint8_t report_id, const uint8_t *buffer,
                                 uint16_t bufsize, uint8_t *left, uint8_t *right)
{
    if (buffer == NULL || left == NULL || right == NULL) return false;
    /* Control SET_REPORT strips ID 5 in TinyUSB; interrupt OUT retains it. */
    if (report_id == 0 && bufsize == 5 && buffer[0] == 5) {
        buffer++;
        bufsize--;
    } else if (report_id != 5) {
        return false;
    }
    if (bufsize != 4) return false;
    uint16_t strong = (uint16_t)buffer[0] | (uint16_t)buffer[1] << 8;
    uint16_t weak = (uint16_t)buffer[2] | (uint16_t)buffer[3] << 8;
    *left = (uint8_t)((strong + 128u) / 257u);
    *right = (uint8_t)((weak + 128u) / 257u);
    return true;
}

bool flydigi_rumble_send_due(uint8_t left, uint8_t right,
                             uint8_t last_left, uint8_t last_right,
                             uint32_t elapsed_us, uint32_t min_interval_us,
                             bool never_sent, uint32_t now_us,
                             uint32_t retry_after_us)
{
    if (retry_after_us != 0)
        return (int32_t)(now_us - retry_after_us) >= 0 ||
               left < last_left || right < last_right;
    if (never_sent || elapsed_us >= min_interval_us) return true;
    if (left < last_left || right < last_right) return true;
    if (left == 0 && right == 0 && elapsed_us >= 4000u) return true;
    return last_left == 0 && last_right == 0 && (left != 0 || right != 0);
}
