#ifndef FORTRESS_KEYBOARD_H
#define FORTRESS_KEYBOARD_H
#include "types.h"
typedef struct {
    bool left_shift, right_shift, caps, caps_down, extended;
    unsigned pause_remaining;
} keyboard_decoder_t;
/* Translated set 1, US layout. Returns an ASCII byte or zero for no character. */
char keyboard_decode(keyboard_decoder_t *state, uint8_t code);
#endif
