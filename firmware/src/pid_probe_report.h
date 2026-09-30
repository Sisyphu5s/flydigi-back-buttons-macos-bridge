#pragma once
#include <stdint.h>

#define PID_PROBE_INPUT_REPORT_ID 1u
#define PID_PROBE_INPUT_BYTES 17u

/* Convert the bridge's 26-button, six-axis Generic report to the isolated
 * PID descriptor's 32-button, signed-16-bit input report. */
void pid_probe_pack_input(const uint8_t generic[11], uint8_t out[PID_PROBE_INPUT_BYTES]);
