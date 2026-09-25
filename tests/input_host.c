#include <assert.h>
#include <stdio.h>
#include "keyboard.h"
#include "input_buffer.h"

int main(void) {
    keyboard_set_layout(KBD_LAYOUT_US);
    keyboard_decoder_t k = {0};
    assert(keyboard_decode(&k, 0x1e) == 'a');
    assert(keyboard_decode(&k, 0x9e) == 0);
    keyboard_decode(&k, 42); keyboard_decode(&k, 54);
    keyboard_decode(&k, 0xaa); /* Releasing left shift must preserve right shift. */
    assert(keyboard_decode(&k, 0x1e) == 'A');
    assert(keyboard_decode(&k, 2) == '!');
    keyboard_decode(&k, 0xb6);
    keyboard_decode(&k, 58); keyboard_decode(&k, 58); /* No repeat toggle. */
    assert(keyboard_decode(&k, 0x1e) == 'A');
    keyboard_decode(&k, 42);
    assert(keyboard_decode(&k, 0x1e) == 'a');
    keyboard_decode(&k, 0xaa);
    keyboard_decode(&k, 0xba); keyboard_decode(&k, 58); keyboard_decode(&k, 0xba);
    const unsigned char printscreen[] = {0xe0, 0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0xe0, 0xaa};
    for (size_t i = 0; i < sizeof(printscreen); i++) assert(!keyboard_decode(&k, printscreen[i]));
    const unsigned char pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    for (size_t i = 0; i < sizeof(pause); i++) assert(!keyboard_decode(&k, pause[i]));
    assert(keyboard_decode(&k, 0x1e) == 'a');
    keyboard_decode(&k, 0xe0); assert(!keyboard_decode(&k, 0x48)); /* Up arrow. */
    keyboard_decode(&k, 0xe0); assert(keyboard_decode(&k, 0x1c) == '\n');
    assert(keyboard_decode(&k, 14) == '\b');
    assert(keyboard_decode(&k, 57) == ' ');
    for (unsigned i = 0; i < 256; i++) (void)keyboard_decode(&k, (uint8_t)i);

    char bytes[4];
    keyboard_decoder_t keys = {0};
    assert(keyboard_decode_bytes(&keys, 0xe0, bytes) == 0);
    assert(keyboard_decode_bytes(&keys, 0x48, bytes) == 3);
    assert(bytes[0] == 27 && bytes[1] == '[' && bytes[2] == 'A');
    keyboard_decode_bytes(&keys, 0x1d, bytes);
    assert(keyboard_decode_bytes(&keys, 0x2e, bytes) == 1 && bytes[0] == 3);
    keyboard_decode_bytes(&keys, 0x9d, bytes);
    input_buffer_t sequence = {0};
    for (unsigned i=0;i<INPUT_CAPACITY-2;i++) input_buffer_push(&sequence,'a');
    assert(!input_buffer_sequence(&sequence,"abc",3));
    assert(sequence.count==INPUT_CAPACITY-2 && sequence.dropped==3);
    /* Test Belgian AZERTY layout */
    keyboard_decoder_t az = {0};
    keyboard_set_layout(KBD_LAYOUT_AZERTY);

    keyboard_decode(&az, 0xe0); keyboard_decode(&az, 0x38);
    assert(keyboard_decode(&az, 2) == '|');
    assert(keyboard_decode(&az, 3) == '@');
    assert(keyboard_decode(&az, 4) == '#');
    assert(keyboard_decode(&az, 5) == '{');
    assert(keyboard_decode(&az, 6) == '[');
    assert(keyboard_decode(&az, 7) == '^');
    assert(keyboard_decode(&az, 10) == '{');
    assert(keyboard_decode(&az, 11) == '}');
    assert(keyboard_decode(&az, 26) == '[');
    assert(keyboard_decode(&az, 27) == ']');
    assert(keyboard_decode(&az, 43) == '`');
    assert(keyboard_decode(&az, 53) == '~');
    assert(keyboard_decode(&az, 86) == '\\');
    keyboard_decode(&az, 0xe0); keyboard_decode(&az, 0xb8);

    /* Test NumPad keys */
    keyboard_decoder_t np = {0};
    /* Default NumLock is active: digits 0..9, ., operators */
    assert(keyboard_decode(&np, 0x52) == '0');
    assert(keyboard_decode(&np, 0x4f) == '1');
    assert(keyboard_decode(&np, 0x50) == '2');
    assert(keyboard_decode(&np, 0x51) == '3');
    assert(keyboard_decode(&np, 0x4b) == '4');
    assert(keyboard_decode(&np, 0x4c) == '5');
    assert(keyboard_decode(&np, 0x4d) == '6');
    assert(keyboard_decode(&np, 0x47) == '7');
    assert(keyboard_decode(&np, 0x48) == '8');
    assert(keyboard_decode(&np, 0x49) == '9');
    assert(keyboard_decode(&np, 0x53) == '.');
    assert(keyboard_decode(&np, 0x4a) == '-');
    assert(keyboard_decode(&np, 0x4e) == '+');
    assert(keyboard_decode(&np, 0x37) == '*');
    /* Extended keypad keys */
    keyboard_decode(&np, 0xe0); assert(keyboard_decode(&np, 0x35) == '/');
    keyboard_decode(&np, 0xe0); assert(keyboard_decode(&np, 0x1c) == '\n');
    /* Toggle NumLock off with 0x45 */
    keyboard_decode(&np, 0x45); keyboard_decode(&np, 0xc5);
    /* With NumLock off, 0x48 (Up) on keypad emits navigation sequence */
    assert(keyboard_decode(&np, 0x48) == 0);
    assert(keyboard_decode_bytes(&np, 0x48, bytes) == 3 && bytes[0] == 27 && bytes[2] == 'A');
    /* Unshifted top row approximations */
    assert(keyboard_decode(&az, 2) == '&');
    assert(keyboard_decode(&az, 3) == 'e');  /* é approx */
    assert(keyboard_decode(&az, 4) == '"');
    assert(keyboard_decode(&az, 5) == '\'');
    assert(keyboard_decode(&az, 6) == '(');
    assert(keyboard_decode(&az, 7) == '-');  /* § approx */
    assert(keyboard_decode(&az, 8) == 'e');  /* è approx */
    assert(keyboard_decode(&az, 9) == '!');
    assert(keyboard_decode(&az, 10) == 'c'); /* ç approx */
    assert(keyboard_decode(&az, 11) == 'a'); /* à approx */
    assert(keyboard_decode(&az, 12) == ')');
    assert(keyboard_decode(&az, 13) == '-');

    /* Shifted top row MUST produce digits (testing H4 bug fix) */
    keyboard_decode(&az, 42); /* Left shift */
    assert(keyboard_decode(&az, 2) == '1');
    assert(keyboard_decode(&az, 3) == '2');
    assert(keyboard_decode(&az, 4) == '3');
    assert(keyboard_decode(&az, 5) == '4');
    assert(keyboard_decode(&az, 6) == '5');
    assert(keyboard_decode(&az, 7) == '6');
    assert(keyboard_decode(&az, 8) == '7');
    assert(keyboard_decode(&az, 9) == '8');
    assert(keyboard_decode(&az, 10) == '9');
    assert(keyboard_decode(&az, 11) == '0');
    assert(keyboard_decode(&az, 12) == 'o'); /* degree approx */
    assert(keyboard_decode(&az, 13) == '_');
    keyboard_decode(&az, 0xaa); /* Release shift */

    /* Caps Lock on Belgian AZERTY acts as Shift-Lock for number row */
    keyboard_decode(&az, 58); /* Caps Lock press */
    keyboard_decode(&az, 0xba); /* Caps Lock release */
    assert(keyboard_decode(&az, 2) == '1');
    assert(keyboard_decode(&az, 3) == '2');
    assert(keyboard_decode(&az, 4) == '3');
    assert(keyboard_decode(&az, 5) == '4');
    assert(keyboard_decode(&az, 6) == '5');
    assert(keyboard_decode(&az, 7) == '6');
    assert(keyboard_decode(&az, 8) == '7');
    assert(keyboard_decode(&az, 9) == '8');
    assert(keyboard_decode(&az, 10) == '9');
    assert(keyboard_decode(&az, 11) == '0');

    /* Shift while Caps Lock is active returns unshifted number row */
    keyboard_decode(&az, 42); /* Shift press */
    assert(keyboard_decode(&az, 3) == 'e');
    assert(keyboard_decode(&az, 10) == 'c');
    keyboard_decode(&az, 0xaa); /* Shift release */

    /* Letters with Caps Lock */
    assert(keyboard_decode(&az, 16) == 'A'); /* 'a' in AZERTY */
    assert(keyboard_decode(&az, 39) == 'M'); /* 'm' in AZERTY */
    keyboard_decode(&az, 42); /* Shift press */
    assert(keyboard_decode(&az, 16) == 'a');
    assert(keyboard_decode(&az, 39) == 'm');
    keyboard_decode(&az, 0xaa); /* Shift release */

    /* Turn Caps Lock off */
    keyboard_decode(&az, 58);
    keyboard_decode(&az, 0xba);
    assert(keyboard_decode(&az, 16) == 'a');
    assert(keyboard_decode(&az, 39) == 'm');

    /* Key 40: 'u' unshifted, '%' shifted */
    assert(keyboard_decode(&az, 40) == 'u');
    keyboard_decode(&az, 42);
    assert(keyboard_decode(&az, 40) == '%');
    keyboard_decode(&az, 0xaa);

    /* Key 86: ISO European '<' / '>' */
    assert(keyboard_decode(&az, 86) == '<');
    keyboard_decode(&az, 42);
    assert(keyboard_decode(&az, 86) == '>');
    keyboard_decode(&az, 0xaa);

    /* Reset to US layout */
    keyboard_set_layout(KBD_LAYOUT_US);

    input_buffer_t q = {0};
    char out[INPUT_CAPACITY];
    for (unsigned cycle = 0; cycle < 20; cycle++) {
        assert(input_buffer_read(&q, out, sizeof(out)) == 0);
        for (unsigned i = 0; i < INPUT_CAPACITY; i++) assert(input_buffer_push(&q, (char)i));
        assert(!input_buffer_push(&q, '!'));
        assert(input_buffer_read(&q, out, 17) == 17);
        for (unsigned i = 0; i < 17; i++) assert((unsigned char)out[i] == i);
        assert(input_buffer_read(&q, out, sizeof(out)) == INPUT_CAPACITY - 17);
        for (unsigned i = 0; i < INPUT_CAPACITY - 17; i++) assert((unsigned char)out[i] == i + 17);
    }
    assert(q.dropped == 20 && q.count == 0);
    puts("PASS: translated scancodes, modifiers, extended sequences, queue overflow and wraparound");
}
