#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "bridge_timing.h"

int main(void)
{
    bridge_input_sample_t sample = { .sticks_us = 9000, .other_us = 6000,
                                     .xinput_overlay = true };
    uint32_t age = 0;
    assert(bridge_changed_age_us(10000, &sample, true, false, &age) && age == 1000);
    assert(bridge_changed_age_us(10000, &sample, false, true, &age) && age == 4000);
    assert(bridge_changed_age_us(10000, &sample, true, true, &age) && age == 4000);
    assert(!bridge_changed_age_us(10000, &sample, false, false, &age));
    sample.other_us = 0;
    assert(!bridge_changed_age_us(10000, &sample, true, true, &age));
    sample.sticks_us = UINT32_MAX - 100;
    assert(bridge_changed_age_us(100, &sample, true, false, &age) && age == 201);
    puts("bridge timing: PASS");
    return 0;
}
