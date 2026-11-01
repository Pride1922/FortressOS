#ifndef FORTRESS_KEYBOARD_H
#define FORTRESS_KEYBOARD_H

#include "types.h"
#include "input_event.h"

#define KBD_LAYOUT_US      0
#define KBD_LAYOUT_AZERTY  1

typedef struct {
    bool left_shift, right_shift, caps, caps_down, extended;
    bool left_ctrl, right_ctrl, altgr;
    bool num_lock_disabled, numlock_down;
    unsigned pause_remaining;
    /* Event-only state (keyboard_decode_event); byte decoding ignores it. */
    bool left_alt, left_super, right_super;
    uint8_t down[16]; /* bitmap of held Linux keycodes 0..127 */
} keyboard_decoder_t;

/* All-CPU atomic layout publication (0 = US QWERTY, 1 = Belgian AZERTY).
 * Decoder state itself remains exclusively owned by BSP ingress. */
void keyboard_set_layout(int layout);
int  keyboard_get_layout(void);

/* Translated set 1. Returns an ASCII byte or zero for no character. */
char keyboard_decode(keyboard_decoder_t *state, uint8_t code);
/* Whole synthesized sequence (maximum 4 bytes), or zero. */
size_t keyboard_decode_bytes(keyboard_decoder_t *state, uint8_t code, char out[4]);
/* keyboard_decode_bytes plus a structured event for the same scancode. The
 * returned bytes are identical to keyboard_decode_bytes. ev->keycode is 0 when
 * the scancode produced no event (E0 prefix, fake shifts, unmapped codes).
 * Pause (E1 sequence) yields KEY_PAUSE press on E1 and release on its end. */
size_t keyboard_decode_event(keyboard_decoder_t *state, uint8_t code, char out[4],
                             key_event_t *ev);
/* Modifier bitmask (KEY_MOD_*) of the current decoder state. */
uint16_t keyboard_modifiers(const keyboard_decoder_t *state);

#endif /* FORTRESS_KEYBOARD_H */
