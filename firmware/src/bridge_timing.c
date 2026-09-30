#include "bridge_timing.h"

bool bridge_changed_age_us(uint32_t now_us, const bridge_input_sample_t *sample,
                           bool sticks_changed, bool other_changed, uint32_t *age_us)
{
    if (sample == 0 || age_us == 0 || (!sticks_changed && !other_changed) ||
        (sticks_changed && sample->sticks_us == 0) ||
        (other_changed && sample->other_us == 0)) return false;
    uint32_t sticks_age = sticks_changed ? now_us - sample->sticks_us : 0;
    uint32_t other_age = other_changed ? now_us - sample->other_us : 0;
    *age_us = sticks_age > other_age ? sticks_age : other_age;
    return true;
}
