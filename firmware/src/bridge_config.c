/* bridge_config.c - pure C configuration and fixed-point shaping */
#include "bridge_config.h"
#include "vader2pro_hid.h"

#include <stdatomic.h>
#include <string.h>

static bridge_config_t s_config;
static bool s_config_ready;
static atomic_flag s_config_lock = ATOMIC_FLAG_INIT;
_Static_assert(V2P_BTN_COUNT == BRIDGE_OUTPUT_BUTTONS, "button count must match HID report");

static void config_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_config_lock, memory_order_acquire)) {}
}

static void config_unlock(void)
{
    atomic_flag_clear_explicit(&s_config_lock, memory_order_release);
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

void bridge_config_defaults(bridge_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->magic = BRIDGE_CONFIG_MAGIC;
    cfg->version = BRIDGE_CONFIG_VERSION;
    cfg->size = (uint8_t)sizeof *cfg;
    cfg->left_deadzone = 1024;
    cfg->right_deadzone = 1024;
    cfg->left_curve = 0;
    cfg->right_curve = 0;
    cfg->gyro_scale_percent = 100;
    cfg->rumble_gain_left = 255;
    cfg->rumble_gain_right = 255;
    cfg->rumble_min_interval_ms = 4;
    cfg->rumble_watchdog_ms = 250;
#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
    cfg->flags = BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS |
                 BRIDGE_FLAG_GENERIC_DPAD_MAPPING;
#endif
    for (unsigned i = 0; i < BRIDGE_INPUT_BUTTONS; i++)
        cfg->button_map[i] = BRIDGE_BUTTON_DISABLED;

    cfg->button_map[BRIDGE_SRC_A] = V2P_BTN_A;
    cfg->button_map[BRIDGE_SRC_B] = V2P_BTN_B;
    cfg->button_map[BRIDGE_SRC_X] = V2P_BTN_X;
    cfg->button_map[BRIDGE_SRC_Y] = V2P_BTN_Y;
    cfg->button_map[BRIDGE_SRC_LB] = V2P_BTN_LB;
    cfg->button_map[BRIDGE_SRC_RB] = V2P_BTN_RB;
    cfg->button_map[BRIDGE_SRC_SELECT] = V2P_BTN_SELECT;
    cfg->button_map[BRIDGE_SRC_START] = V2P_BTN_START;
    cfg->button_map[BRIDGE_SRC_L3] = V2P_BTN_L3;
    cfg->button_map[BRIDGE_SRC_R3] = V2P_BTN_R3;
    cfg->button_map[BRIDGE_SRC_M1] = V2P_BTN_M1;
    cfg->button_map[BRIDGE_SRC_M2] = V2P_BTN_M2;
    cfg->button_map[BRIDGE_SRC_M3] = V2P_BTN_M3;
    cfg->button_map[BRIDGE_SRC_M4] = V2P_BTN_M4;
    cfg->button_map[BRIDGE_SRC_HOME] = V2P_BTN_HOME;
    cfg->button_map[BRIDGE_SRC_C] = V2P_BTN_C;
    cfg->button_map[BRIDGE_SRC_Z] = V2P_BTN_Z;
    cfg->button_map[BRIDGE_SRC_LM] = V2P_BTN_LM;
    cfg->button_map[BRIDGE_SRC_RM] = V2P_BTN_RM;
    cfg->button_map[BRIDGE_SRC_O] = V2P_BTN_O;
#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
    cfg->button_map[BRIDGE_SRC_DPAD_UP] = V2P_BTN_DPAD_UP;
    cfg->button_map[BRIDGE_SRC_DPAD_DOWN] = V2P_BTN_DPAD_DOWN;
    cfg->button_map[BRIDGE_SRC_DPAD_LEFT] = V2P_BTN_DPAD_LEFT;
    cfg->button_map[BRIDGE_SRC_DPAD_RIGHT] = V2P_BTN_DPAD_RIGHT;
#endif
    bridge_config_finalize(cfg);
}

#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
bool bridge_config_upgrade_generic_defaults(bridge_config_t *cfg)
{
    static const uint8_t legacy_map[BRIDGE_INPUT_BUTTONS] = {
        0, 1, 0xff, 3, 4, 0xff, 6, 7, 0xff, 0xff,
        10, 11, 12, 13, 14, 15, 16, 17, 0xff, 19,
        20, 21, 22, 23, 24,
    };
    static const uint8_t generic_map_without_dpad_flag[BRIDGE_INPUT_BUTTONS] = {
        0, 1, 0xff, 2, 3, 0xff, 4, 5, 0xff, 0xff,
        8, 9, 10, 11, 17, 18, 19, 20, 0xff, 16,
        21, 22, 23, 24, 25,
    };
    if (!bridge_config_validate(cfg))
        return false;
    bool const legacy_default = cfg->flags == 0 &&
        memcmp(cfg->button_map, legacy_map, sizeof legacy_map) == 0;
    bool const current_default = cfg->flags == BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS &&
        memcmp(cfg->button_map, generic_map_without_dpad_flag,
               sizeof generic_map_without_dpad_flag) == 0;
    if (!legacy_default && !current_default) return false;
    bridge_config_t defaults;
    bridge_config_defaults(&defaults);
    memcpy(cfg->button_map, defaults.button_map, sizeof cfg->button_map);
    cfg->flags = BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS |
                 BRIDGE_FLAG_GENERIC_DPAD_MAPPING;
    bridge_config_finalize(cfg);
    return true;
}
#endif

void bridge_config_sanitize(bridge_config_t *cfg)
{
    if (cfg == NULL) return;
    if (cfg->magic != BRIDGE_CONFIG_MAGIC) cfg->magic = BRIDGE_CONFIG_MAGIC;
    cfg->version = BRIDGE_CONFIG_VERSION;
    cfg->size = (uint8_t)sizeof *cfg;
    if (cfg->left_deadzone > 32767u) cfg->left_deadzone = 32767u;
    if (cfg->right_deadzone > 32767u) cfg->right_deadzone = 32767u;
    if (cfg->gyro_scale_percent < 1u) cfg->gyro_scale_percent = 1u;
    if (cfg->gyro_scale_percent > 400u) cfg->gyro_scale_percent = 400u;
    if (cfg->rumble_min_interval_ms > 1000u) cfg->rumble_min_interval_ms = 1000u;
    if (cfg->rumble_watchdog_ms < 50u) cfg->rumble_watchdog_ms = 50u;
    if (cfg->rumble_watchdog_ms > 2000u) cfg->rumble_watchdog_ms = 2000u;
    for (unsigned i = 0; i < BRIDGE_INPUT_BUTTONS; i++) {
        if (cfg->button_map[i] >= BRIDGE_OUTPUT_BUTTONS && cfg->button_map[i] != BRIDGE_BUTTON_DISABLED)
            cfg->button_map[i] = BRIDGE_BUTTON_DISABLED;
    }
}

uint32_t bridge_config_crc32(const bridge_config_t *cfg)
{
    if (cfg == NULL) return 0;
    return crc32_bytes((const uint8_t *)cfg, offsetof(bridge_config_t, crc32));
}

void bridge_config_finalize(bridge_config_t *cfg)
{
    if (cfg == NULL) return;
    bridge_config_sanitize(cfg);
    cfg->crc32 = bridge_config_crc32(cfg);
}

bool bridge_config_validate(const bridge_config_t *cfg)
{
    if (cfg == NULL || cfg->magic != BRIDGE_CONFIG_MAGIC ||
        cfg->version != BRIDGE_CONFIG_VERSION || cfg->size != sizeof *cfg)
        return false;
    bridge_config_t copy = *cfg;
    bridge_config_sanitize(&copy);
    return memcmp(cfg, &copy, offsetof(bridge_config_t, crc32)) == 0 &&
           cfg->crc32 == bridge_config_crc32(cfg);
}

void bridge_config_get(bridge_config_t *out)
{
    if (out == NULL) return;
    config_lock();
    if (!s_config_ready) {
        bridge_config_defaults(&s_config);
        s_config_ready = true;
    }
    *out = s_config;
    config_unlock();
}

bool bridge_config_set(const bridge_config_t *cfg)
{
    if (!bridge_config_validate(cfg)) return false;
    config_lock();
    s_config = *cfg;
    s_config_ready = true;
    config_unlock();
    return true;
}

uint8_t bridge_axis_u8(int16_t raw, bool invert, uint16_t deadzone, uint8_t curve)
{
    int32_t value = raw;
    if (invert) value = -value;
    int32_t magnitude = value < 0 ? -value : value;
    if ((uint32_t)magnitude <= deadzone) return 128;

    int32_t span = 32768 - (int32_t)deadzone;
    int32_t normalized = ((magnitude - deadzone) * 32767 + span / 2) / span;
    if (normalized > 32767) normalized = 32767;
    /* Blend linear and cubic response.  This avoids powf and keeps endpoints
     * exact while still providing a useful anti-twitch curve. */
    int64_t cubic = (int64_t)normalized * normalized * normalized / (32767LL * 32767LL);
    int32_t shaped = (int32_t)(((int64_t)(255u - curve) * normalized +
                                (int64_t)curve * cubic + 127) / 255);
    int32_t output = (shaped * (value < 0 ? 128 : 127) + 16383) / 32767;
    if (output > (value < 0 ? 128 : 127)) output = value < 0 ? 128 : 127;
    int32_t result = value < 0 ? 128 - output : 128 + output;
    if (result < 0) result = 0;
    if (result > 255) result = 255;
    return (uint8_t)result;
}

static uint32_t integer_sqrt(uint32_t value)
{
    uint32_t root = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static uint8_t signed_axis_u8(int32_t value)
{
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    int32_t span = value < 0 ? 128 : 127;
    int32_t output = (int32_t)((magnitude * (uint32_t)span + 16383u) / 32767u);
    return (uint8_t)(value < 0 ? 128 - output : 128 + output);
}

void bridge_stick_u8(int16_t x, int16_t y, bool invert_x, bool invert_y,
                     uint16_t deadzone, uint8_t curve, bool radial,
                     uint8_t *out_x, uint8_t *out_y)
{
    if (out_x == NULL || out_y == NULL) return;
    if (!radial) {
        *out_x = bridge_axis_u8(x, invert_x, deadzone, curve);
        *out_y = bridge_axis_u8(y, invert_y, deadzone, curve);
        return;
    }

    int32_t vx = invert_x ? -(int32_t)x : x;
    int32_t vy = invert_y ? -(int32_t)y : y;
    uint32_t ax = (uint32_t)(vx < 0 ? -vx : vx);
    uint32_t ay = (uint32_t)(vy < 0 ? -vy : vy);
    uint32_t max_axis = ax > ay ? ax : ay;
    uint32_t radius = integer_sqrt(ax * ax + ay * ay);
    if (radius <= deadzone || max_axis == 0) {
        *out_x = *out_y = 128;
        return;
    }

    /* Scale along the stick direction to its square HID boundary so full
     * diagonals can still reach both axis endpoints. */
    uint32_t outer_radius = (uint32_t)(((uint64_t)radius * 32767u + max_axis / 2u) / max_axis);
    uint32_t normalized = outer_radius <= deadzone ? 32767u :
        (uint32_t)(((uint64_t)(radius - deadzone) * 32767u +
                    (outer_radius - deadzone) / 2u) / (outer_radius - deadzone));
    if (normalized > 32767u) normalized = 32767u;
    uint32_t cubic = (uint32_t)((uint64_t)normalized * normalized * normalized /
                                (32767u * 32767u));
    uint32_t shaped = ((255u - curve) * normalized + curve * cubic + 127u) / 255u;
    int32_t base_x = (int32_t)((int64_t)vx * 32767 / (int32_t)max_axis);
    int32_t base_y = (int32_t)((int64_t)vy * 32767 / (int32_t)max_axis);
    *out_x = signed_axis_u8((int32_t)((int64_t)base_x * shaped / 32767));
    *out_y = signed_axis_u8((int32_t)((int64_t)base_y * shaped / 32767));
}

void bridge_config_rumble(uint8_t in_left, uint8_t in_right,
                          uint8_t *out_left, uint8_t *out_right)
{
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    if (out_left) *out_left = (uint8_t)(((uint16_t)in_left * cfg.rumble_gain_left + 127u) / 255u);
    if (out_right) *out_right = (uint8_t)(((uint16_t)in_right * cfg.rumble_gain_right + 127u) / 255u);
}

void bridge_sensor_values(const int16_t raw[3], const bridge_config_t *cfg,
                          bool gyro, bool fresh, int16_t out[3])
{
    if (!fresh) {
        memset(out, 0, 3u * sizeof *out);
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        int32_t value = raw[i];
        if (gyro) {
            value = (value - cfg->gyro_bias[i]) * cfg->gyro_scale_percent / 100;
            if (cfg->gyro_invert & (1u << i)) value = -value;
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
        }
        out[i] = (int16_t)value;
    }
}
