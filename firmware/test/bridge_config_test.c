/* bridge_config_test.c - fixed-point shaping and WebHID config payload checks */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge_config.h"
#include "vader2pro_hid.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    bridge_config_t cfg;
    bridge_config_defaults(&cfg);
    CHECK(sizeof cfg < 63u, "config fits one WebHID response");
    CHECK(bridge_config_validate(&cfg), "defaults have valid CRC");
#ifdef CORSAIR_APPLE_IDENTITY
    CHECK((cfg.flags & BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS) == 0,
          "SCUF profile uses analog triggers and a hat");
    CHECK(cfg.button_map[BRIDGE_SRC_HOME] == 12 &&
          cfg.button_map[BRIDGE_SRC_M1] == 20 && cfg.button_map[BRIDGE_SRC_M2] == 23 &&
          cfg.button_map[BRIDGE_SRC_M3] == 21 && cfg.button_map[BRIDGE_SRC_M4] == 22,
          "SCUF profile assigns four named paddle elements");
    CHECK(cfg.button_map[BRIDGE_SRC_C] == 24 && cfg.button_map[BRIDGE_SRC_Z] == 25 &&
          cfg.button_map[BRIDGE_SRC_LM] == 26 && cfg.button_map[BRIDGE_SRC_RM] == 27 &&
          cfg.button_map[BRIDGE_SRC_O] == 28,
          "SCUF profile assigns five named G elements");
#elif defined(GENERIC_APPLE_IDENTITY)
    CHECK((cfg.flags & BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS) == 0,
          "Apple-order profile does not inject standard trigger/D-pad buttons");
    CHECK(cfg.button_map[BRIDGE_SRC_A] == 0 && cfg.button_map[BRIDGE_SRC_B] == 1 &&
          cfg.button_map[BRIDGE_SRC_X] == 3 && cfg.button_map[BRIDGE_SRC_Y] == 4 &&
          cfg.button_map[BRIDGE_SRC_HOME] == 19, "Apple-order face and Home indices");
    CHECK(cfg.button_map[BRIDGE_SRC_M1] == 14 && cfg.button_map[BRIDGE_SRC_M4] == 17,
          "Apple-order paddles preserve native indices");
    CHECK(cfg.button_map[BRIDGE_SRC_C] == 20 && cfg.button_map[BRIDGE_SRC_Z] == 21 &&
          cfg.button_map[BRIDGE_SRC_LM] == 22 && cfg.button_map[BRIDGE_SRC_RM] == 23 &&
          cfg.button_map[BRIDGE_SRC_O] == 24, "extra buttons follow Apple model slots");
#elif defined(GENERIC_GAMEPAD)
    CHECK((cfg.flags & (BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS |
                        BRIDGE_FLAG_GENERIC_DPAD_MAPPING)) ==
          (BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS |
           BRIDGE_FLAG_GENERIC_DPAD_MAPPING), "generic standard index flags");
    CHECK(cfg.button_map[BRIDGE_SRC_X] == 2 && cfg.button_map[BRIDGE_SRC_Y] == 3 &&
          cfg.button_map[BRIDGE_SRC_LB] == 4 && cfg.button_map[BRIDGE_SRC_RB] == 5 &&
          cfg.button_map[BRIDGE_SRC_HOME] == 16, "generic standard button indices");
    CHECK(cfg.button_map[BRIDGE_SRC_M1] == 17 && cfg.button_map[BRIDGE_SRC_M4] == 20,
          "generic paddles follow standard buttons");
    CHECK(cfg.button_map[BRIDGE_SRC_C] == 21 && cfg.button_map[BRIDGE_SRC_Z] == 22 &&
          cfg.button_map[BRIDGE_SRC_LM] == 23 && cfg.button_map[BRIDGE_SRC_RM] == 24 &&
          cfg.button_map[BRIDGE_SRC_O] == 25, "generic extras have independent slots");
    CHECK(cfg.button_map[BRIDGE_SRC_DPAD_UP] == V2P_BTN_DPAD_UP &&
          cfg.button_map[BRIDGE_SRC_DPAD_DOWN] == V2P_BTN_DPAD_DOWN &&
          cfg.button_map[BRIDGE_SRC_DPAD_LEFT] == V2P_BTN_DPAD_LEFT &&
          cfg.button_map[BRIDGE_SRC_DPAD_RIGHT] == V2P_BTN_DPAD_RIGHT,
          "generic D-pad directions have independent mapping sources");

    bridge_config_t legacy = cfg;
    static const uint8_t old_map[BRIDGE_INPUT_BUTTONS] = {
        0, 1, 0xff, 3, 4, 0xff, 6, 7, 0xff, 0xff,
        10, 11, 12, 13, 14, 15, 16, 17, 0xff, 19,
        20, 21, 22, 23, 24,
    };
    memcpy(legacy.button_map, old_map, sizeof old_map);
    legacy.flags = 0;
    legacy.left_deadzone = 2048;
    bridge_config_finalize(&legacy);
    CHECK(bridge_config_upgrade_generic_defaults(&legacy) &&
          bridge_config_validate(&legacy) && legacy.left_deadzone == 2048 &&
          memcmp(legacy.button_map, cfg.button_map, sizeof cfg.button_map) == 0,
          "legacy default map migrates without losing stick settings");
    legacy.flags = 0;
    legacy.button_map[BRIDGE_SRC_A] = 5;
    bridge_config_finalize(&legacy);
    CHECK(!bridge_config_upgrade_generic_defaults(&legacy), "custom legacy map is preserved");

    bridge_config_t current = cfg;
    current.flags = BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS;
    current.button_map[BRIDGE_SRC_DPAD_UP] = BRIDGE_BUTTON_DISABLED;
    current.button_map[BRIDGE_SRC_DPAD_DOWN] = BRIDGE_BUTTON_DISABLED;
    current.button_map[BRIDGE_SRC_DPAD_LEFT] = BRIDGE_BUTTON_DISABLED;
    current.button_map[BRIDGE_SRC_DPAD_RIGHT] = BRIDGE_BUTTON_DISABLED;
    bridge_config_finalize(&current);
    CHECK(bridge_config_upgrade_generic_defaults(&current) &&
          memcmp(current.button_map, cfg.button_map, sizeof cfg.button_map) == 0 &&
          current.flags == cfg.flags,
          "current Generic default map gains explicit D-pad mapping");
#else
    CHECK(cfg.button_map[BRIDGE_SRC_M1] == 14 && cfg.button_map[BRIDGE_SRC_M4] == 17,
          "M1-M4 preserve model slots");
    CHECK(cfg.button_map[BRIDGE_SRC_C] == 2 && cfg.button_map[BRIDGE_SRC_Z] == 5 &&
          cfg.button_map[BRIDGE_SRC_LM] == 8 && cfg.button_map[BRIDGE_SRC_RM] == 9 &&
          cfg.button_map[BRIDGE_SRC_O] == 18, "extra buttons use unused slots");
#endif

    CHECK(bridge_axis_u8(0, false, cfg.left_deadzone, cfg.left_curve) == 128,
          "deadzone centers small drift");
    CHECK(bridge_axis_u8(32767, false, 0, 0) == 255 &&
          bridge_axis_u8(-32768, false, 0, 0) == 0, "axis endpoints are exact");
    CHECK(bridge_axis_u8(10000, false, 0, 0) > 128 &&
          bridge_axis_u8(10000, false, 255, 0) <= bridge_axis_u8(10000, false, 0, 0),
          "curve remains monotonic");

    uint8_t x, y, linear_x, linear_y;
    bridge_stick_u8(2000, 2000, false, false, 2500, 0, false, &x, &y);
    CHECK(x == 128 && y == 128, "axial deadzone keeps existing diagonal behavior");
    bridge_stick_u8(2000, 2000, false, false, 2500, 0, true, &x, &y);
    CHECK(x > 128 && y > 128 && x == y, "radial deadzone preserves shallow diagonal");
    bridge_stick_u8(32767, 32767, false, false, 1024, 0, true, &x, &y);
    CHECK(x == 255 && y == 255, "radial full diagonal reaches square endpoints");
    bridge_stick_u8(-32768, 0, false, false, 1024, 0, true, &x, &y);
    CHECK(x == 0 && y == 128, "radial negative endpoint remains exact");
    bridge_stick_u8(0, 32767, false, true, 1024, 0, true, &x, &y);
    CHECK(x == 128 && y == 0, "radial mode respects axis inversion");
    /* The mapper applies X/Y swap before this same shaping function; verify
     * the flag values remain available without changing the packed ABI. */
    CHECK((BRIDGE_FLAG_LEFT_SWAP_XY | BRIDGE_FLAG_RIGHT_SWAP_XY) == 0x60u,
          "axis swap flags use reserved config bits");
    bridge_stick_u8(12000, 18000, false, false, 1024, 0, true, &linear_x, &linear_y);
    bridge_stick_u8(12000, 18000, false, false, 1024, 255, true, &x, &y);
    CHECK(x <= linear_x && y <= linear_y && x >= 128 && y >= 128,
          "radial response curve remains monotonic");
    uint32_t seed = 0x13579bdfu;
    bool radial_monotonic = true;
    for (unsigned i = 0; i < 4096; i++) {
        seed = seed * 1664525u + 1013904223u;
        int16_t sx = (int16_t)(seed >> 16);
        seed = seed * 1664525u + 1013904223u;
        int16_t sy = (int16_t)(seed >> 16);
        uint16_t inner = (uint16_t)(i * 7u);
        uint16_t outer = inner + 1024u;
        uint8_t near_x, near_y, far_x, far_y;
        bridge_stick_u8(sx, sy, false, false, inner, 0, true, &near_x, &near_y);
        bridge_stick_u8(sx, sy, false, false, outer, 0, true, &far_x, &far_y);
        if ((sx > 0 && (near_x < 128 || far_x < 128)) ||
            (sx < 0 && (near_x > 128 || far_x > 128)) ||
            (sy > 0 && (near_y < 128 || far_y < 128)) ||
            (sy < 0 && (near_y > 128 || far_y > 128)) ||
            abs((int)far_x - 128) > abs((int)near_x - 128) ||
            abs((int)far_y - 128) > abs((int)near_y - 128)) {
            radial_monotonic = false;
            break;
        }
    }
    CHECK(radial_monotonic, "radial direction and deadzone monotonicity across 4096 vectors");
    cfg.flags |= BRIDGE_FLAG_LEFT_RADIAL_DEADZONE | BRIDGE_FLAG_RIGHT_RADIAL_DEADZONE |
                 BRIDGE_FLAG_FRESH_XINPUT_STICKS;
    bridge_config_finalize(&cfg);
    CHECK(bridge_config_validate(&cfg), "radial and XInput stick modes persist in flags");

    cfg.gyro_bias[0] = 100;
    cfg.gyro_bias[1] = -100;
    cfg.gyro_scale_percent = 200;
    cfg.gyro_invert = 1;
    const int16_t sensor_raw[3] = {500, -500, 100};
    int16_t sensor_out[3];
    bridge_sensor_values(sensor_raw, &cfg, true, true, sensor_out);
    CHECK(sensor_out[0] == -800 && sensor_out[1] == -800 && sensor_out[2] == 200,
          "live gyro applies bias, scale and inversion");
    bridge_sensor_values(sensor_raw, &cfg, false, true, sensor_out);
    CHECK(sensor_out[0] == 500 && sensor_out[1] == -500 && sensor_out[2] == 100,
          "live accelerometer bypasses gyro calibration");
    bridge_sensor_values(sensor_raw, &cfg, true, false, sensor_out);
    CHECK(sensor_out[0] == 0 && sensor_out[1] == 0 && sensor_out[2] == 0,
          "stale gyro stays zero despite nonzero bias");
    bridge_sensor_values(sensor_raw, &cfg, false, false, sensor_out);
    CHECK(sensor_out[0] == 0 && sensor_out[1] == 0 && sensor_out[2] == 0,
          "stale accelerometer is cleared");
    const int16_t sensor_max[3] = {32767, -32768, 0};
    cfg.gyro_bias[0] = -32768;
    cfg.gyro_bias[1] = 32767;
    cfg.gyro_scale_percent = 400;
    bridge_sensor_values(sensor_max, &cfg, true, true, sensor_out);
    CHECK(sensor_out[0] == -32768 && sensor_out[1] == -32768,
          "gyro calibration clamps both extremes");

    cfg.button_map[BRIDGE_SRC_C] = 0;
    bridge_config_finalize(&cfg);
    CHECK(bridge_config_validate(&cfg), "edited config can be finalized");
    cfg.button_map[BRIDGE_SRC_C] = 1;
    CHECK(!bridge_config_validate(&cfg), "CRC rejects an unfinalized edit");

    bridge_config_defaults(&cfg);
    cfg.left_deadzone = UINT16_MAX;
    bridge_config_t sanitized = cfg;
    bridge_config_finalize(&sanitized);
    cfg.crc32 = sanitized.crc32;
    CHECK(!bridge_config_validate(&cfg), "sanitized CRC cannot authorize out-of-range deadzone");
    CHECK(!bridge_config_set(&cfg), "out-of-range config cannot become active");
    printf("bridge config: %s (%zu bytes)\n", fails ? "FAIL" : "PASS", sizeof cfg);
    return fails != 0;
}
