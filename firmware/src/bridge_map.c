/* bridge_map.c — 见 bridge_map.h */
#include "bridge_map.h"

/* EF byte[11] D-pad 四位组合（up=1, right=2, down=4, left=8）→ hat 表 */
static const uint8_t HAT_LUT[16] = {
    /* 0  */ 8,  /* 1  up        */ 0,  /* 2  right     */ 2,  /* 3  up+right  */ 1,
    /* 4  */ 4,  /* 5  up+down   */ 8,  /* 6  right+down*/ 3,  /* 7  三向      */ 8,
    /* 8  */ 6,  /* 9  left+up   */ 7,  /* 10 左+右     */ 8,  /* 11 三向      */ 8,
    /* 12 */ 5,  /* 13 三向      */ 8,  /* 14 三向      */ 8,  /* 15 四向      */ 8,
};

uint8_t flydigi_hat_from_b1(uint8_t b1)
{
    unsigned idx = 0;
    if (b1 & FD_B1_DPAD_UP)    idx |= 1u;
    if (b1 & FD_B1_DPAD_RIGHT) idx |= 2u;
    if (b1 & FD_B1_DPAD_DOWN)  idx |= 4u;
    if (b1 & FD_B1_DPAD_LEFT)  idx |= 8u;
    return HAT_LUT[idx];
}

uint32_t bridge_source_buttons(const flydigi_rx_state_t *rx)
{
    if (rx == NULL) return 0;
    const uint8_t b1 = rx->b1, b2 = rx->b2, e1 = rx->ext1, e2 = rx->ext2;
    bool source[BRIDGE_INPUT_BUTTONS] = {0};
    source[BRIDGE_SRC_A] = (b1 & FD_B1_A) != 0;
    source[BRIDGE_SRC_B] = (b1 & FD_B1_B) != 0;
    source[BRIDGE_SRC_X] = (b1 & FD_B1_X) != 0;
    source[BRIDGE_SRC_Y] = (b2 & FD_B2_Y) != 0;
    source[BRIDGE_SRC_LB] = (b2 & FD_B2_LB) != 0;
    source[BRIDGE_SRC_RB] = (b2 & FD_B2_RB) != 0;
    source[BRIDGE_SRC_SELECT] = (b1 & FD_B1_SELECT) != 0;
    source[BRIDGE_SRC_START] = (b2 & FD_B2_START) != 0;
    source[BRIDGE_SRC_L3] = (b2 & FD_B2_L3) != 0;
    source[BRIDGE_SRC_R3] = (b2 & FD_B2_R3) != 0;
    /* flydigi_host_get() has already resolved the receiver's measured
     * active-high EF Home bit (idle 0x00, held 0x08), while preserving the
     * standard XInput Guide bit for receiver variants that provide it. */
    source[BRIDGE_SRC_HOME] = rx->guide;
    source[BRIDGE_SRC_M1] = (e1 & FD_EXT_M1) != 0;
    source[BRIDGE_SRC_M2] = (e1 & FD_EXT_M2) != 0;
    source[BRIDGE_SRC_M3] = (e1 & FD_EXT_M3) != 0;
    source[BRIDGE_SRC_M4] = (e1 & FD_EXT_M4) != 0;
    source[BRIDGE_SRC_DPAD_UP] = (b1 & FD_B1_DPAD_UP) != 0;
    source[BRIDGE_SRC_DPAD_DOWN] = (b1 & FD_B1_DPAD_DOWN) != 0;
    source[BRIDGE_SRC_DPAD_LEFT] = (b1 & FD_B1_DPAD_LEFT) != 0;
    source[BRIDGE_SRC_DPAD_RIGHT] = (b1 & FD_B1_DPAD_RIGHT) != 0;
    source[BRIDGE_SRC_C] = (e1 & FD_EXT_C) != 0;
    source[BRIDGE_SRC_Z] = (e1 & FD_EXT_Z) != 0;
    source[BRIDGE_SRC_LM] = (e1 & FD_EXT_LM) != 0;
    source[BRIDGE_SRC_RM] = (e1 & FD_EXT_RM) != 0;
    source[BRIDGE_SRC_O] = (e2 & FD_EXT2_O) != 0;

    uint32_t buttons = 0;
    for (unsigned i = 0; i < BRIDGE_INPUT_BUTTONS; i++)
        if (source[i]) buttons |= 1u << i;
    return buttons;
}

void bridge_map(const flydigi_rx_state_t *rx, vader2pro_state_t *out)
{
    bridge_config_t cfg;
    bridge_config_get(&cfg);
    /* Flydigi's EF/XInput signed Y values increase downwards.  SDL's V2
     * backend negates both Y axes before exposing them to applications. */
    bool const swap_left = (cfg.flags & BRIDGE_FLAG_LEFT_SWAP_XY) != 0;
    bool const swap_right = (cfg.flags & BRIDGE_FLAG_RIGHT_SWAP_XY) != 0;
    bridge_stick_u8(swap_left ? rx->ly : rx->lx, swap_left ? rx->lx : rx->ly,
                    (cfg.axis_invert & (1u << 0)) != 0,
                    (cfg.axis_invert & (1u << 1)) == 0, cfg.left_deadzone,
                    cfg.left_curve, (cfg.flags & BRIDGE_FLAG_LEFT_RADIAL_DEADZONE) != 0,
                    &out->lx, &out->ly);
    bridge_stick_u8(swap_right ? rx->ry : rx->rx, swap_right ? rx->rx : rx->ry,
                    (cfg.axis_invert & (1u << 2)) != 0,
                    (cfg.axis_invert & (1u << 3)) == 0, cfg.right_deadzone,
                    cfg.right_curve, (cfg.flags & BRIDGE_FLAG_RIGHT_RADIAL_DEADZONE) != 0,
                    &out->rx, &out->ry);
    out->rt = rx->rt;
    out->lt = rx->lt;
    out->hat = flydigi_hat_from_b1(rx->b1);

    for (unsigned i = 0; i < V2P_BTN_COUNT; i++)
        out->buttons[i] = false;

    uint32_t const source = bridge_source_buttons(rx);

    /* Each source is routed independently.  Multiple sources may target one
     * output slot, which makes layer-like chords possible without changing the
     * Apple-sensitive report layout. */
    for (unsigned i = 0; i < BRIDGE_INPUT_BUTTONS; i++) {
        uint8_t target = cfg.button_map[i];
        if ((source & (1u << i)) && target < V2P_BTN_COUNT) out->buttons[target] = true;
    }
#if defined(GENERIC_GAMEPAD) && !defined(GENERIC_APPLE_IDENTITY) && !defined(CORSAIR_APPLE_IDENTITY)
    if (cfg.flags & BRIDGE_FLAG_GENERIC_STANDARD_BUTTONS) {
        out->buttons[V2P_BTN_LT] |= rx->lt >= 128;
        out->buttons[V2P_BTN_RT] |= rx->rt >= 128;
        if (!(cfg.flags & BRIDGE_FLAG_GENERIC_DPAD_MAPPING)) {
            /* Old Generic Flash maps have these four source slots disabled.
             * Preserve their standard buttons until the user applies a new
             * configuration carrying the explicit mapping bit. */
            out->buttons[V2P_BTN_DPAD_UP] |= (rx->b1 & FD_B1_DPAD_UP) != 0;
            out->buttons[V2P_BTN_DPAD_DOWN] |= (rx->b1 & FD_B1_DPAD_DOWN) != 0;
            out->buttons[V2P_BTN_DPAD_LEFT] |= (rx->b1 & FD_B1_DPAD_LEFT) != 0;
            out->buttons[V2P_BTN_DPAD_RIGHT] |= (rx->b1 & FD_B1_DPAD_RIGHT) != 0;
        }
    }
#endif
}

void bridge_pack(const flydigi_rx_state_t *rx, uint8_t out[VADER2PRO_REPORT_BYTES])
{
    vader2pro_state_t s;
    bridge_map(rx, &s);
    vader2pro_pack(&s, out);
}
