/* rumble_report.h — device-side HID Output report decoding */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Decode the accepted host/browser formats into left/right motor strengths. */
bool flydigi_rumble_decode(const uint8_t *buffer, uint16_t bufsize,
                           uint8_t *left, uint8_t *right);
bool flydigi_stadia_haptic_decode(uint8_t report_id, const uint8_t *buffer,
                                 uint16_t bufsize, uint8_t *left, uint8_t *right);

/* Rate-limit increases; allow start and reductions immediately. A failed
 * transfer retries after its short backoff, independent of the configured interval. */
bool flydigi_rumble_send_due(uint8_t left, uint8_t right,
                             uint8_t last_left, uint8_t last_right,
                             uint32_t elapsed_us, uint32_t min_interval_us,
                             bool never_sent, uint32_t now_us,
                             uint32_t retry_after_us);
