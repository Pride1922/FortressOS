#include <assert.h>
#include <stdio.h>
#include "keyboard.h"
#include "input_buffer.h"

int main(void) {
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
