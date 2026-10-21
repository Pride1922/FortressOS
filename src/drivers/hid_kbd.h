#ifndef FORTRESS_HID_KBD_H
#define FORTRESS_HID_KBD_H

#include "types.h"

/* USB HID Boot Keyboard Report (8 bytes) */
typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} __attribute__((packed)) hid_boot_kbd_report_t;

/* Modifier Bits */
#define HID_MOD_LCTRL           (1u << 0)
#define HID_MOD_LSHIFT          (1u << 1)
#define HID_MOD_LALT            (1u << 2)
#define HID_MOD_LGUI            (1u << 3)
#define HID_MOD_RCTRL           (1u << 4)
#define HID_MOD_RSHIFT          (1u << 5)
#define HID_MOD_RALT            (1u << 6) /* AltGr */
#define HID_MOD_RGUI            (1u << 7)

#define HID_EXTENDED_FLAG       0x80u
#define HID_ERROR_ROLLOVER      0x01u

/*
 * Static table mapping USB HID Usage Page 7 codes (0..127) to PS/2 Set 1 scancodes.
 * Bits 0..6: Set 1 scancode (1..0x58).
 * Bit 7: HID_EXTENDED_FLAG (set if the key requires PS/2 0xE0 prefix).
 */
static const uint8_t hid_to_ps2_set1[128] = {
    [0x04] = 0x1E, /* a */
    [0x05] = 0x30, /* b */
    [0x06] = 0x2E, /* c */
    [0x07] = 0x20, /* d */
    [0x08] = 0x12, /* e */
    [0x09] = 0x21, /* f */
    [0x0A] = 0x22, /* g */
    [0x0B] = 0x23, /* h */
    [0x0C] = 0x17, /* i */
    [0x0D] = 0x24, /* j */
    [0x0E] = 0x25, /* k */
    [0x0F] = 0x26, /* l */
    [0x10] = 0x32, /* m */
    [0x11] = 0x31, /* n */
    [0x12] = 0x18, /* o */
    [0x13] = 0x19, /* p */
    [0x14] = 0x10, /* q */
    [0x15] = 0x13, /* r */
    [0x16] = 0x1F, /* s */
    [0x17] = 0x14, /* t */
    [0x18] = 0x16, /* u */
    [0x19] = 0x2F, /* v */
    [0x1A] = 0x11, /* w */
    [0x1B] = 0x2D, /* x */
    [0x1C] = 0x15, /* y */
    [0x1D] = 0x2C, /* z */
    [0x1E] = 0x02, /* 1 */
    [0x1F] = 0x03, /* 2 */
    [0x20] = 0x04, /* 3 */
    [0x21] = 0x05, /* 4 */
    [0x22] = 0x06, /* 5 */
    [0x23] = 0x07, /* 6 */
    [0x24] = 0x08, /* 7 */
    [0x25] = 0x09, /* 8 */
    [0x26] = 0x0A, /* 9 */
    [0x27] = 0x0B, /* 0 */
    [0x28] = 0x1C, /* Enter */
    [0x29] = 0x01, /* Escape */
    [0x2A] = 0x0E, /* Backspace */
    [0x2B] = 0x0F, /* Tab */
    [0x2C] = 0x39, /* Space */
    [0x2D] = 0x0C, /* - _ */
    [0x2E] = 0x0D, /* = + */
    [0x2F] = 0x1A, /* [ { */
    [0x30] = 0x1B, /* ] } */
    [0x31] = 0x2B, /* \ | */
    [0x33] = 0x27, /* ; : */
    [0x34] = 0x28, /* ' " */
    [0x35] = 0x29, /* ` ~ */
    [0x36] = 0x33, /* , < */
    [0x37] = 0x34, /* . > */
    [0x38] = 0x35, /* / ? */
    [0x39] = 0x3A, /* Caps Lock */
    [0x3A] = 0x3B, /* F1 */
    [0x3B] = 0x3C, /* F2 */
    [0x3C] = 0x3D, /* F3 */
    [0x3D] = 0x3E, /* F4 */
    [0x3E] = 0x3F, /* F5 */
    [0x3F] = 0x40, /* F6 */
    [0x40] = 0x41, /* F7 */
    [0x41] = 0x42, /* F8 */
    [0x42] = 0x43, /* F9 */
    [0x43] = 0x44, /* F10 */
    [0x44] = 0x57, /* F11 */
    [0x45] = 0x58, /* F12 */
    [0x46] = 0x37 | HID_EXTENDED_FLAG, /* PrintScreen */
    [0x47] = 0x46,                     /* ScrollLock */
    [0x48] = 0x1D | HID_EXTENDED_FLAG, /* Pause */
    [0x49] = 0x52 | HID_EXTENDED_FLAG, /* Insert */
    [0x4A] = 0x47 | HID_EXTENDED_FLAG, /* Home */
    [0x4B] = 0x49 | HID_EXTENDED_FLAG, /* PageUp */
    [0x4C] = 0x53 | HID_EXTENDED_FLAG, /* Delete */
    [0x4D] = 0x4F | HID_EXTENDED_FLAG, /* End */
    [0x4E] = 0x51 | HID_EXTENDED_FLAG, /* PageDown */
    [0x4F] = 0x4D | HID_EXTENDED_FLAG, /* Right Arrow */
    [0x50] = 0x4B | HID_EXTENDED_FLAG, /* Left Arrow */
    [0x51] = 0x50 | HID_EXTENDED_FLAG, /* Down Arrow */
    [0x52] = 0x48 | HID_EXTENDED_FLAG, /* Up Arrow */
    [0x53] = 0x45,                     /* NumLock */
    [0x54] = 0x35 | HID_EXTENDED_FLAG, /* Keypad / */
    [0x55] = 0x37,                     /* Keypad * */
    [0x56] = 0x4A,                     /* Keypad - */
    [0x57] = 0x4E,                     /* Keypad + */
    [0x58] = 0x1C | HID_EXTENDED_FLAG, /* Keypad Enter */
    [0x59] = 0x4F,                     /* Keypad 1 */
    [0x5A] = 0x50,                     /* Keypad 2 */
    [0x5B] = 0x51,                     /* Keypad 3 */
    [0x5C] = 0x4B,                     /* Keypad 4 */
    [0x5D] = 0x4C,                     /* Keypad 5 */
    [0x5E] = 0x4D,                     /* Keypad 6 */
    [0x5F] = 0x47,                     /* Keypad 7 */
    [0x60] = 0x48,                     /* Keypad 8 */
    [0x61] = 0x49,                     /* Keypad 9 */
    [0x62] = 0x52,                     /* Keypad 0 */
    [0x63] = 0x53,                     /* Keypad . */
    [0x64] = 0x56,                     /* Non-US \ and | (ISO key next to LShift; Belgian < >) */
    [0x65] = 0x5D | HID_EXTENDED_FLAG, /* Menu / App key */
};

/* Scancode sink callback: called for each Set 1 scancode event */
typedef void (*hid_scancode_sink_t)(uint8_t scancode, bool extended, void *ctx);

/*
 * Checks if a key code is present in a 6-key array.
 */
static inline bool hid_has_key(const uint8_t keys[6], uint8_t key) {
    if (!key) return false;
    for (int i = 0; i < 6; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

/*
 * Helper to dispatch a modifier make or break event.
 */
static inline void hid_dispatch_mod(uint8_t scancode, bool extended, bool make,
                                    hid_scancode_sink_t sink, void *ctx) {
    uint8_t code = make ? scancode : (uint8_t)(scancode | 0x80u);
    sink(code, extended, ctx);
}

/*
 * Diffs a current HID boot keyboard report against the previous report.
 * Emits make/break Set 1 scancodes via sink callback.
 * Updates *prev = *curr upon completion.
 */
static inline void hid_kbd_process_report(const hid_boot_kbd_report_t *curr,
                                         hid_boot_kbd_report_t *prev,
                                         hid_scancode_sink_t sink,
                                         void *ctx) {
    if (!curr || !prev || !sink) return;

    /* 1. Phantom / ErrorRollOver protection: if any byte is 0x01, drop report */
    for (int i = 0; i < 6; i++) {
        if (curr->keys[i] == HID_ERROR_ROLLOVER) return;
    }

    /* 2. Modifier Diffing */
    uint8_t mod_diff = curr->modifiers ^ prev->modifiers;
    if (mod_diff) {
        if (mod_diff & HID_MOD_LCTRL)
            hid_dispatch_mod(0x1D, false, (curr->modifiers & HID_MOD_LCTRL) != 0, sink, ctx);
        if (mod_diff & HID_MOD_LSHIFT)
            hid_dispatch_mod(0x2A, false, (curr->modifiers & HID_MOD_LSHIFT) != 0, sink, ctx);
        if (mod_diff & HID_MOD_LALT)
            hid_dispatch_mod(0x38, false, (curr->modifiers & HID_MOD_LALT) != 0, sink, ctx);
        if (mod_diff & HID_MOD_LGUI)
            hid_dispatch_mod(0x5B, true, (curr->modifiers & HID_MOD_LGUI) != 0, sink, ctx);
        if (mod_diff & HID_MOD_RCTRL)
            hid_dispatch_mod(0x1D, true, (curr->modifiers & HID_MOD_RCTRL) != 0, sink, ctx);
        if (mod_diff & HID_MOD_RSHIFT)
            hid_dispatch_mod(0x36, false, (curr->modifiers & HID_MOD_RSHIFT) != 0, sink, ctx);
        if (mod_diff & HID_MOD_RALT)
            hid_dispatch_mod(0x38, true, (curr->modifiers & HID_MOD_RALT) != 0, sink, ctx); /* AltGr */
        if (mod_diff & HID_MOD_RGUI)
            hid_dispatch_mod(0x5C, true, (curr->modifiers & HID_MOD_RGUI) != 0, sink, ctx);
    }

    /* 3. Key Releases (keys in prev but not in curr) */
    for (int i = 0; i < 6; i++) {
        uint8_t k = prev->keys[i];
        if (k >= 0x04 && k < 128 && !hid_has_key(curr->keys, k)) {
            uint8_t mapped = hid_to_ps2_set1[k];
            if (mapped) {
                uint8_t scancode = (mapped & 0x7Fu) | 0x80u; /* Break code */
                bool extended = (mapped & HID_EXTENDED_FLAG) != 0;
                sink(scancode, extended, ctx);
            }
        }
    }

    /* 4. Key Presses (keys in curr but not in prev) */
    for (int i = 0; i < 6; i++) {
        uint8_t k = curr->keys[i];
        if (k >= 0x04 && k < 128 && !hid_has_key(prev->keys, k)) {
            uint8_t mapped = hid_to_ps2_set1[k];
            if (mapped) {
                uint8_t scancode = mapped & 0x7Fu; /* Make code */
                bool extended = (mapped & HID_EXTENDED_FLAG) != 0;
                sink(scancode, extended, ctx);
            }
        }
    }

    /* Update previous state */
    *prev = *curr;
}

#endif /* FORTRESS_HID_KBD_H */
