#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "keyboard.h"
#include "input_buffer.h"
#include "kbd_dev.h"
#include "spinlock.h"

static unsigned g_poll_wakes = 0;
void poll_wake_notify(void) { g_poll_wakes++; }

#ifdef TEST_SMP_MEMORY
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    pthread_mutex_unlock(&lock->mutex);
}
#endif

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

    /* Structured key events: press/release, modifiers, repeat, extended keys,
     * and byte output identical to keyboard_decode_bytes. */
    keyboard_decoder_t ek = {0};
    key_event_t ev;
    char eb[4];
    assert(keyboard_decode_event(&ek, 0x1e, eb, &ev) == 1 && eb[0] == 'a');
    assert(ev.keycode == KEY_A && ev.state == KEY_STATE_PRESS && ev.ascii == 'a');
    assert(ev.modifiers == KEY_MOD_NUMLOCK && ev.reserved == 0);
    assert(keyboard_decode_event(&ek, 0x1e, eb, &ev) == 1);
    assert(ev.keycode == KEY_A && ev.state == KEY_STATE_REPEAT && ev.ascii == 'a');
    assert(keyboard_decode_event(&ek, 0x9e, eb, &ev) == 0);
    assert(ev.keycode == KEY_A && ev.state == KEY_STATE_RELEASE && ev.ascii == 0);
    /* Modifier presses are events with no bytes; snapshot includes them. */
    assert(keyboard_decode_event(&ek, 0x2a, eb, &ev) == 0);
    assert(ev.keycode == KEY_LEFTSHIFT && ev.state == KEY_STATE_PRESS &&
           (ev.modifiers & KEY_MOD_SHIFT));
    assert(keyboard_decode_event(&ek, 0x1e, eb, &ev) == 1 && ev.ascii == 'A' &&
           (ev.modifiers & KEY_MOD_SHIFT));
    keyboard_decode_event(&ek, 0x9e, eb, &ev);
    assert(keyboard_decode_event(&ek, 0xaa, eb, &ev) == 0);
    assert(ev.keycode == KEY_LEFTSHIFT && ev.state == KEY_STATE_RELEASE &&
           !(ev.modifiers & KEY_MOD_SHIFT));
    /* Ctrl + Alt + Esc: all three modifiers visible on the Esc press. */
    keyboard_decode_event(&ek, 0x1d, eb, &ev);
    assert(ev.keycode == KEY_LEFTCTRL && (ev.modifiers & KEY_MOD_CTRL));
    keyboard_decode_event(&ek, 0x38, eb, &ev);
    assert(ev.keycode == KEY_LEFTALT && (ev.modifiers & KEY_MOD_ALT) &&
           !(ev.modifiers & KEY_MOD_ALTGR));
    assert(keyboard_decode_event(&ek, 0x01, eb, &ev) == 1 && eb[0] == 27);
    assert(ev.keycode == KEY_ESC && ev.state == KEY_STATE_PRESS &&
           (ev.modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT)) == (KEY_MOD_CTRL | KEY_MOD_ALT));
    keyboard_decode_event(&ek, 0x81, eb, &ev);
    keyboard_decode_event(&ek, 0xb8, eb, &ev);
    assert(!(ev.modifiers & KEY_MOD_ALT));
    /* Ctrl+C keeps its control byte as ascii. */
    assert(keyboard_decode_event(&ek, 0x2e, eb, &ev) == 1 && ev.ascii == 3);
    keyboard_decode_event(&ek, 0xae, eb, &ev);
    keyboard_decode_event(&ek, 0x9d, eb, &ev);
    assert(ev.modifiers == KEY_MOD_NUMLOCK);
    /* E0 prefix alone is not an event; arrows carry no ascii but bytes. */
    assert(keyboard_decode_event(&ek, 0xe0, eb, &ev) == 0 && ev.keycode == 0);
    assert(keyboard_decode_event(&ek, 0x48, eb, &ev) == 3 && eb[0] == 27 && eb[2] == 'A');
    assert(ev.keycode == KEY_UP && ev.state == KEY_STATE_PRESS && ev.ascii == 0);
    keyboard_decode_event(&ek, 0xe0, eb, &ev);
    assert(keyboard_decode_event(&ek, 0xc8, eb, &ev) == 0 && ev.keycode == KEY_UP &&
           ev.state == KEY_STATE_RELEASE);
    /* Super keys, right Alt (AltGr) and right Ctrl. */
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0x5b, eb, &ev);
    assert(ev.keycode == KEY_LEFTMETA && (ev.modifiers & KEY_MOD_SUPER));
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0xdb, eb, &ev);
    assert(ev.keycode == KEY_LEFTMETA && ev.state == KEY_STATE_RELEASE &&
           !(ev.modifiers & KEY_MOD_SUPER));
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0x38, eb, &ev);
    assert(ev.keycode == KEY_RIGHTALT &&
           (ev.modifiers & (KEY_MOD_ALT | KEY_MOD_ALTGR)) == (KEY_MOD_ALT | KEY_MOD_ALTGR));
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0xb8, eb, &ev);
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0x1d, eb, &ev);
    assert(ev.keycode == KEY_RIGHTCTRL && (ev.modifiers & KEY_MOD_CTRL));
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0x9d, eb, &ev);
    /* Fake shifts around Print Screen produce no events. */
    keyboard_decode_event(&ek, 0xe0, eb, &ev);
    assert(keyboard_decode_event(&ek, 0x2a, eb, &ev) == 0 && ev.keycode == 0);
    keyboard_decode_event(&ek, 0xe0, eb, &ev); keyboard_decode_event(&ek, 0x37, eb, &ev);
    assert(ev.keycode == KEY_SYSRQ);
    /* Pause: press on E1, release when the six-byte sequence ends. */
    const uint8_t pause_seq[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    unsigned pause_events = 0;
    for (size_t i = 0; i < sizeof(pause_seq); i++) {
        assert(keyboard_decode_event(&ek, pause_seq[i], eb, &ev) == 0);
        if (ev.keycode) {
            assert(ev.keycode == KEY_PAUSE);
            assert(ev.state == (pause_events ? KEY_STATE_RELEASE : KEY_STATE_PRESS));
            pause_events++;
        }
    }
    assert(pause_events == 2);
    /* Caps/NumLock latches; F12 maps to 88; unmapped codes are silent. */
    keyboard_decode_event(&ek, 0x3a, eb, &ev); keyboard_decode_event(&ek, 0xba, eb, &ev);
    assert(ev.modifiers & KEY_MOD_CAPS);
    keyboard_decode_event(&ek, 0x45, eb, &ev); keyboard_decode_event(&ek, 0xc5, eb, &ev);
    assert(!(ev.modifiers & KEY_MOD_NUMLOCK));
    keyboard_decode_event(&ek, 0x58, eb, &ev);
    assert(ev.keycode == 88 && ev.ascii == 0);
    assert(keyboard_decode_event(&ek, 0x7f, eb, &ev) == 0 && ev.keycode == 0);
    /* Bytes are identical to keyboard_decode_bytes for every scancode. */
    keyboard_decoder_t a = {0}, b = {0};
    for (unsigned i = 0; i < 512; i++) {
        uint8_t code = (uint8_t)(i * 37u + 11u);
        char ba[4], bb[4];
        size_t na = keyboard_decode_bytes(&a, code, ba);
        size_t nb = keyboard_decode_event(&b, code, bb, &ev);
        assert(na == nb && !memcmp(ba, bb, na));
        assert(ev.keycode < 128 && ev.state <= KEY_STATE_REPEAT);
    }
    puts("PASS: translated scancodes, modifiers, extended sequences, queue overflow and wraparound");
    puts("PASS: structured key events (press/release/repeat, modifiers, E0/E1, byte parity)");

    /* Test /dev/input/keyboard & /dev/kbd0 FIFO, drop-oldest saturation,
     * open/close grab refcounting, pre-grab flush, and non-blocking VFS reads. */
    vfs_node_t dev_dir = {.type = VFS_DIRECTORY, .name = "dev", .path = "/dev"};
    vfs_node_t input_dir = {.type = VFS_DIRECTORY, .name = "input", .path = "/dev/input", .parent = &dev_dir};
    dev_dir.children = &input_dir;
    kbd_dev_init(&dev_dir);

    vfs_node_t *kbd0 = NULL, *input_kbd = NULL;
    for (vfs_node_t *c = dev_dir.children; c; c = c->next) {
        if (!strcmp(c->name, "kbd0")) kbd0 = c;
    }
    for (vfs_node_t *c = input_dir.children; c; c = c->next) {
        if (!strcmp(c->name, "keyboard")) input_kbd = c;
    }
    assert(kbd0 && input_kbd);
    assert(is_kbd_dev_node(kbd0) && is_kbd_dev_node(input_kbd) && !is_kbd_dev_node(&dev_dir));
    assert(!kbd_dev_grabbed() && !kbd_dev_has_data());

    /* Events pushed before any open are flushed when the first reader opens. */
    kbd_dev_push(&(key_event_t){.keycode = KEY_A, .state = KEY_STATE_PRESS, .ascii = 'a'});
    assert(kbd_dev_has_data());
    assert(input_kbd->open(input_kbd) == 0);
    assert(kbd_dev_grabbed() && !kbd_dev_has_data());

    /* Fill FIFO past KBD_DEV_FIFO_CAP (128) to verify drop-oldest saturation. */
    unsigned wakes_before = g_poll_wakes;
    for (unsigned i = 1; i <= KBD_DEV_FIFO_CAP + 5; i++) {
        kbd_dev_push(&(key_event_t){
            .keycode = (uint16_t)i,
            .modifiers = KEY_MOD_SHIFT,
            .state = KEY_STATE_PRESS,
            .ascii = 'x',
            .reserved = 0xbeef
        });
    }
    assert(g_poll_wakes == wakes_before + KBD_DEV_FIFO_CAP + 5);
    assert(kbd_dev_dropped() == 5 && kbd_dev_has_data());

    /* Drain all 128 events via non-blocking VFS reads; oldest 5 (1..5) were dropped. */
    key_event_t rbuf[32];
    size_t total_read = 0;
    while (kbd_dev_has_data()) {
        int64_t nr = input_kbd->read(input_kbd, 0, rbuf, sizeof(rbuf));
        assert(nr > 0 && (nr % sizeof(key_event_t)) == 0);
        size_t cnt = (size_t)nr / sizeof(key_event_t);
        for (size_t i = 0; i < cnt; i++) {
            assert(rbuf[i].keycode == (uint16_t)(total_read + 6));
            assert(rbuf[i].modifiers == KEY_MOD_SHIFT);
            assert(rbuf[i].state == KEY_STATE_PRESS);
            assert(rbuf[i].ascii == 'x');
            assert(rbuf[i].reserved == 0);
            total_read++;
        }
    }
    assert(total_read == KBD_DEV_FIFO_CAP);
    assert(input_kbd->read(input_kbd, 0, rbuf, sizeof(rbuf)) == 0);
    assert(input_kbd->read(input_kbd, 0, rbuf, 7) == -VFS_EINVAL);

    /* Nested open/close preserves grab until final close. */
    assert(kbd0->open(kbd0) == 0);
    input_kbd->close(input_kbd);
    assert(kbd_dev_grabbed());
    kbd0->close(kbd0);
    assert(!kbd_dev_grabbed());
    puts("PASS: kbd_dev FIFO, drop-oldest saturation, VFS read batching, and console grab");
}
