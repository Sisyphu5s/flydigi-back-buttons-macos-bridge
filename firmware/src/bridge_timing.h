#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sticks_us;
    uint32_t other_us;
    uint32_t sensor_us;
    bool xinput_overlay;
} bridge_input_sample_t;

/* Report the oldest contributing sample when both sticks and other fields
 * change in one HID report. All timestamps use the same wrapping 32-bit clock. */
bool bridge_changed_age_us(uint32_t now_us, const bridge_input_sample_t *sample,
                           bool sticks_changed, bool other_changed, uint32_t *age_us);
