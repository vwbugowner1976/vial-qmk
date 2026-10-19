#include "keycodes.h"
#include "keycode_config.h"
#include "keymap_japanese.h"
#include "quantum_keycodes.h"

#include "os_key_override.h"

// 
// Perform as a US keyboard on JP systems
//
static const key_override_t *us_key_on_jp_os_overrides[] = {
    // KC_GRV override depends on OS
    w_shift(KC_GRV, JP_TILD),
    w_shift(KC_2, JP_AT),
    w_shift(KC_6, JP_CIRC),
    w_shift(KC_7, JP_AMPR),
    w_shift(KC_8, JP_ASTR),
    w_shift(KC_9, JP_LPRN),
    w_shift(KC_0, JP_RPRN),
    w_shift(KC_MINS, JP_UNDS),
    wo_shift(KC_EQL, JP_EQL),
    w_shift(KC_EQL, JP_PLUS),
    wo_shift(KC_LBRC, JP_LBRC),
    w_shift(KC_LBRC, JP_LCBR),
    wo_shift(KC_RBRC, JP_RBRC),
    w_shift(KC_RBRC, JP_RCBR),
    wo_shift(KC_BSLS, JP_BSLS),
    w_shift(KC_BSLS, JP_PIPE),
    w_shift(KC_SCLN, JP_COLN),
    wo_shift(KC_QUOT, JP_QUOT),
    w_shift(KC_QUOT, JP_DQUO),
};

// On Windows, make KC_GRV send the Japanese half/full-width key
// without requiring Alt. Ignore Shift so the key remains usable with either Shift state.
static const key_override_t *grv_override_jp_win = {wo_shift(KC_GRV, JP_GRV)};

// On Mac, use the same standalone half/full-width behavior.
static const key_override_t *grv_override_jp_mac = {wo_shift(KC_GRV, JP_GRV)};

void register_us_key_on_jp_os_overrides(void) {
    remove_all_os_key_overrides();

    for (int idx = 0; idx < sizeof(us_key_on_jp_os_overrides) / sizeof(us_key_on_jp_os_overrides[0]); idx++) {
        register_os_key_override(us_key_on_jp_os_overrides[idx]);
    }

    if (keymap_config.swap_lalt_lgui) {
        register_os_key_override(grv_override_jp_mac);
    } else {
        register_os_key_override(grv_override_jp_win);
    }
}

/*
 * Only keep the OS-specific half-width/full-width key behavior.
 * Do not register the punctuation remaps from us_key_on_jp_os_overrides.
 */
void register_us_key_on_jp_os_half_width_only_override(void) {
    // Keep the same US-on-JP punctuation mapping as UJ OVR.
    // The unshifted KC_GRV is already the Japanese half/full-width key
    // (JP_ZKHK aliases KC_GRV), so do not override it to JP_GRV.
    remove_all_os_key_overrides();

    for (int idx = 0; idx < sizeof(us_key_on_jp_os_overrides) / sizeof(us_key_on_jp_os_overrides[0]); idx++) {
        register_os_key_override(us_key_on_jp_os_overrides[idx]);
    }
}
