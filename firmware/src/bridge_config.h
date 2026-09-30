/* bridge_config.h - runtime mapping, axis and haptics configuration */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_CONFIG_VERSION 1u
#define BRIDGE_INPUT_BUTTONS 25u
#define BRIDGE_FLAG_LEFT_RADIAL_DEADZONE 0x0002u
#define BRIDGE_FLAG_RIGHT_RADIAL_DEADZONE 0x0004u
#define BRIDGE_FLAG_FRESH_XINPUT_STICKS 0x0008u
/* Generic profiles can route D-pad directions through button_map.  The bit
 * is deliberately separate so old Generic Flash maps keep their historical
 * hard-coded D-pad fallback until they are explicitly reset or saved. */
#define BRIDGE_FLAG_GENERIC_DPAD_MAPPING 0x0010u
/* Keep these in the existing flags word so old 59-byte configurations remain
 * valid.  The swap is applied before inversion and deadzone shaping. */
#define BRIDGE_FLAG_LEFT_SWAP_XY 0x0020u
#define BRIDGE_FLAG_RIGHT_SWAP_XY 0x0040u
#ifdef CORSAIR_APPLE_IDENTITY
#define BRIDGE_OUTPUT_BUTTONS 29u
#elif defined(GENERIC_GAMEPAD)
#define BRIDGE_OUTPUT_BUTTONS 26u
#else
#define BRIDGE_OUTPUT_BUTTONS 20u
#endif
#define BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS 0x0001u
#define BRIDGE_CONFIG_MAGIC 0x31474643u /* "CFG1" */

/* Source indices include five slots that used to be reserved. Four of them
 * are now aliases for individual D-pad directions; the remaining slot stays
 * reserved so the packed config size remains backward compatible. */
enum {
    BRIDGE_SRC_A = 0, BRIDGE_SRC_B, BRIDGE_SRC_UNUSED2, BRIDGE_SRC_X,
    BRIDGE_SRC_Y, BRIDGE_SRC_UNUSED5, BRIDGE_SRC_LB, BRIDGE_SRC_RB,
    BRIDGE_SRC_UNUSED8, BRIDGE_SRC_UNUSED9, BRIDGE_SRC_SELECT, BRIDGE_SRC_START,
    BRIDGE_SRC_L3, BRIDGE_SRC_R3, BRIDGE_SRC_M1, BRIDGE_SRC_M2,
    BRIDGE_SRC_M3, BRIDGE_SRC_M4, BRIDGE_SRC_UNUSED18, BRIDGE_SRC_HOME,
    BRIDGE_SRC_C, BRIDGE_SRC_Z, BRIDGE_SRC_LM, BRIDGE_SRC_RM, BRIDGE_SRC_O,
};

#define BRIDGE_SRC_DPAD_UP    BRIDGE_SRC_UNUSED2
#define BRIDGE_SRC_DPAD_DOWN  BRIDGE_SRC_UNUSED5
#define BRIDGE_SRC_DPAD_LEFT  BRIDGE_SRC_UNUSED8
#define BRIDGE_SRC_DPAD_RIGHT BRIDGE_SRC_UNUSED9

#define BRIDGE_BUTTON_DISABLED 0xffu

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t size;
    uint16_t flags;
    uint16_t left_deadzone;
    uint16_t right_deadzone;
    uint8_t left_curve;
    uint8_t right_curve;
    uint8_t axis_invert;       /* bit0 LX, bit1 LY, bit2 RX, bit3 RY */
    uint8_t gyro_invert;       /* bit0 X, bit1 Y, bit2 Z */
    uint16_t gyro_scale_percent;
    int16_t gyro_bias[3];
    uint8_t rumble_gain_left;
    uint8_t rumble_gain_right;
    uint16_t rumble_min_interval_ms;
    uint16_t rumble_watchdog_ms;
    uint8_t button_map[BRIDGE_INPUT_BUTTONS]; /* source -> output button slot */
    uint32_t crc32;
} bridge_config_t;

void bridge_config_defaults(bridge_config_t *cfg);
void bridge_config_sanitize(bridge_config_t *cfg);
uint32_t bridge_config_crc32(const bridge_config_t *cfg);
void bridge_config_finalize(bridge_config_t *cfg);
bool bridge_config_validate(const bridge_config_t *cfg);

/* Copy the active configuration.  Keeping the copy operation explicit makes
 * HID updates atomic from the mapper's point of view. */
void bridge_config_get(bridge_config_t *out);
bool bridge_config_set(const bridge_config_t *cfg);

#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
/* Upgrade only the previous unmodified Generic default map in flash. */
bool bridge_config_upgrade_generic_defaults(bridge_config_t *cfg);
#endif

/* Fixed-point stick shaping.  deadzone is in raw int16 units; curve is a
 * blend from linear (0) to cubic (255), which is stable and cheap on RP2350. */
uint8_t bridge_axis_u8(int16_t raw, bool invert, uint16_t deadzone, uint8_t curve);
void bridge_stick_u8(int16_t x, int16_t y, bool invert_x, bool invert_y,
                     uint16_t deadzone, uint8_t curve, bool radial,
                     uint8_t *out_x, uint8_t *out_y);

/* Apply configured motor gains without changing the wire-level protocol. */
void bridge_config_rumble(uint8_t in_left, uint8_t in_right,
                         uint8_t *out_left, uint8_t *out_right);

/* Transform live IMU samples; stale input must remain zero even with gyro bias. */
void bridge_sensor_values(const int16_t raw[3], const bridge_config_t *cfg,
                          bool gyro, bool fresh, int16_t out[3]);
