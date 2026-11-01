#ifndef FORTRESS_INPUT_EVENT_H
#define FORTRESS_INPUT_EVENT_H
/* Structured keyboard events read from /dev/input/keyboard (and /dev/kbd0).
 * Each read() returns whole 8-byte records, little-endian, never a partial
 * record. Mirrored in user/compositor/src/keys.rs.
 *
 * keycode: Linux input-event-codes numbering. Non-extended PC set 1 make
 *          codes 1..88 map to the same number (KEY_ESC=1, KEY_A=30,
 *          KEY_F12=88); E0-prefixed keys use the Linux values (KEY_UP=103,
 *          KEY_LEFT=105, KEY_LEFTMETA=125, ...).
 * modifiers: snapshot after this event is applied (a Shift press carries
 *          KEY_MOD_SHIFT, its release does not).
 * state:   KEY_STATE_RELEASE / PRESS / REPEAT. REPEAT marks a hardware
 *          typematic make for a key that is already down; USB HID has no
 *          hardware repeat, so consumers needing repeat synthesize it.
 * ascii:   the single byte the console decoder produced for this press or
 *          repeat under the active layout, or 0 (releases, modifiers, keys
 *          that produce escape sequences such as the arrows). */
#include "types.h"

#define KEY_STATE_RELEASE 0u
#define KEY_STATE_PRESS   1u
#define KEY_STATE_REPEAT  2u

#define KEY_MOD_SHIFT   (1u << 0)
#define KEY_MOD_CTRL    (1u << 1)
#define KEY_MOD_ALT     (1u << 2) /* either Alt, including AltGr */
#define KEY_MOD_SUPER   (1u << 3)
#define KEY_MOD_CAPS    (1u << 4) /* Caps Lock latched */
#define KEY_MOD_NUMLOCK (1u << 5) /* Num Lock latched */
#define KEY_MOD_ALTGR   (1u << 6) /* right Alt specifically */

/* Linux input-event-codes values used by the kernel translator. */
#define KEY_ESC        1u
#define KEY_TAB        15u
#define KEY_ENTER      28u
#define KEY_LEFTCTRL   29u
#define KEY_A          30u
#define KEY_LEFTSHIFT  42u
#define KEY_RIGHTSHIFT 54u
#define KEY_LEFTALT    56u
#define KEY_CAPSLOCK   58u
#define KEY_NUMLOCK    69u
#define KEY_KPENTER    96u
#define KEY_RIGHTCTRL  97u
#define KEY_KPSLASH    98u
#define KEY_SYSRQ      99u
#define KEY_RIGHTALT   100u
#define KEY_HOME       102u
#define KEY_UP         103u
#define KEY_PAGEUP     104u
#define KEY_LEFT       105u
#define KEY_RIGHT      106u
#define KEY_END        107u
#define KEY_DOWN       108u
#define KEY_PAGEDOWN   109u
#define KEY_INSERT     110u
#define KEY_DELETE     111u
#define KEY_PAUSE      119u
#define KEY_LEFTMETA   125u
#define KEY_RIGHTMETA  126u
#define KEY_COMPOSE    127u

typedef struct {
    uint16_t keycode;
    uint16_t modifiers;
    uint8_t  state;
    uint8_t  ascii;
    uint16_t reserved; /* zero */
} key_event_t;
_Static_assert(sizeof(key_event_t) == 8, "key_event_t ABI size");

#endif
