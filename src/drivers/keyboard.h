#ifndef FORTRESS_KEYBOARD_H
#define FORTRESS_KEYBOARD_H

#include "types.h"

#define KBD_LAYOUT_US      0
#define KBD_LAYOUT_AZERTY  1

typedef struct {
    bool left_shift, right_shift, caps, caps_down, extended;
    bool left_ctrl, right_ctrl, altgr;
    bool num_lock_disabled, numlock_down;
    unsigned pause_remaining;
} keyboard_decoder_t;

/* Set / get active keyboard layout (0 = US QWERTY, 1 = Belgian AZERTY) */
void keyboard_set_layout(int layout);
int  keyboard_get_layout(void);

/* Translated set 1. Returns an ASCII byte or zero for no character. */
char keyboard_decode(keyboard_decoder_t *state, uint8_t code);
/* Whole synthesized sequence (maximum 4 bytes), or zero. */
size_t keyboard_decode_bytes(keyboard_decoder_t *state, uint8_t code, char out[4]);

#endif /* FORTRESS_KEYBOARD_H */
