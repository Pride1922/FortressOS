#include "keyboard.h"

static int g_keyboard_layout = KBD_LAYOUT_AZERTY;

void keyboard_set_layout(int layout) {
    __atomic_store_n(&g_keyboard_layout,
        layout==KBD_LAYOUT_AZERTY ? KBD_LAYOUT_AZERTY : KBD_LAYOUT_US,__ATOMIC_RELEASE);
}

int keyboard_get_layout(void) {
    return __atomic_load_n(&g_keyboard_layout,__ATOMIC_ACQUIRE);
}

static const char us_plain[128] = {
    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='-',[13]='=',[14]='\b',[15]='\t',
    [16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
    [26]='[',[27]=']',[28]='\n',
    [30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
    [39]=';',[40]='\'',[41]='`',[43]='\\',
    [44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',
    [51]=',',[52]='.',[53]='/',[55]='*',[57]=' ',
    [74]='-',[78]='+'
};

static const char us_shifted[128] = {
    [2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',[10]='(',[11]=')',
    [12]='_',[13]='+',[26]='{',[27]='}',[39]=':',[40]='"',[41]='~',[43]='|',
    [51]='<',[52]='>',[53]='?'
};

/* Belgian AZERTY layout (Dell Latitude 5590 keyboard), mapped to 7-bit ASCII.
 * In Belgian AZERTY (unlike French AZERTY), the top number row (scancodes 0x02..0x0D)
 * has digits on the shifted layer. Caps Lock acts as a true Shift-Lock for the numeric
 * row (shift ^ s->caps) so digits 1..0 can be typed continuously with Caps Lock active.
 * Accented keys (é, è, ç, à, ù on scancodes 0x03, 0x08, 0x0A, 0x0B, 0x28) are on the
 * unshifted layer. Scancode 86 (0x56) is the physical European ISO key next to Left Shift (< / >).
 */
static const char azerty_plain[128] = {
    [2]='&',[3]='e',[4]='"',[5]='\'',[6]='(',[7]='-',[8]='e',[9]='!',[10]='c',[11]='a',
    [12]=')',[13]='-',[14]='\b',[15]='\t',
    [16]='a',[17]='z',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
    [26]='^',[27]='$',[28]='\n',
    [30]='q',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
    [39]='m',[40]='u',[41]='`',[43]='<',
    [44]='w',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]=',',
    [51]=';',[52]=':',[53]='=',[55]='*',[57]=' ',
    [74]='-',[78]='+',
    [86]='<'
};

static const char azerty_shifted[128] = {
    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='o',[13]='_',[26]='^',[27]='*',[40]='%',[41]='~',[43]='>',
    [50]='?',[51]='.',[52]='/',[53]='+',
    [86]='>'
};

char keyboard_decode(keyboard_decoder_t *s, uint8_t code) {
    if (!s) return 0;
    if (s->pause_remaining) { s->pause_remaining--; return 0; }
    if (code == 0xe1) { s->pause_remaining = 5; return 0; }
    if (code == 0xe0) { s->extended = true; return 0; }
    bool extended = s->extended;
    s->extended = false;
    bool released = (code & 0x80) != 0;
    code &= 0x7f;
    if (extended) {
        if (code == 0x1d) { s->right_ctrl = !released; return 0; }
        if (code == 0x38) { s->altgr = !released; return 0; }
        if (code == 0x1c && !released) return '\n'; /* Keypad Enter */
        if (code == 0x35 && !released) return '/';  /* Keypad / */
        return 0;
    }
    if (code == 0x1d) { s->left_ctrl = !released; return 0; }
    if (code == 42) { s->left_shift = !released; return 0; }
    if (code == 54) { s->right_shift = !released; return 0; }
    if (code == 58) {
        if (!released && !s->caps_down) s->caps = !s->caps;
        s->caps_down = !released;
        return 0;
    }
    if (code == 0x45) { /* NumLock */
        if (!released && !s->numlock_down) s->num_lock_disabled = !s->num_lock_disabled;
        s->numlock_down = !released;
        return 0;
    }
    if (released) return 0;
    if (code == 1) return 27;

    /* Keypad 0..9 and . when NumLock is active */
    if (!extended && code >= 0x47 && code <= 0x53) {
        bool numlock_active = !s->num_lock_disabled ^ (s->left_shift || s->right_shift);
        if (numlock_active) {
            static const char numpad[16] = {
                [0x47 - 0x47] = '7',
                [0x48 - 0x47] = '8',
                [0x49 - 0x47] = '9',
                [0x4A - 0x47] = '-',
                [0x4B - 0x47] = '4',
                [0x4C - 0x47] = '5',
                [0x4D - 0x47] = '6',
                [0x4E - 0x47] = '+',
                [0x4F - 0x47] = '1',
                [0x50 - 0x47] = '2',
                [0x51 - 0x47] = '3',
                [0x52 - 0x47] = '0',
                [0x53 - 0x47] = '.'
            };
            return numpad[code - 0x47];
        }
        return 0;
    }

    int layout=keyboard_get_layout();
    const char *plain_table = (layout == KBD_LAYOUT_AZERTY) ? azerty_plain : us_plain;
    const char *shifted_table = (layout == KBD_LAYOUT_AZERTY) ? azerty_shifted : us_shifted;

    char c = plain_table[code];
    if (s->altgr && layout == KBD_LAYOUT_AZERTY) {
        /* Belgian (Period) KBDBE.DLL official layout specification */
        static const char third[128] = {
            [2]  = '|',   /* & 1 -> | */
            [3]  = '@',   /* é 2 -> @ */
            [4]  = '#',   /* " 3 -> # */
            [5]  = '{',   /* ' 4 -> { */
            [6]  = '[',   /* ( 5 -> [ */
            [7]  = '^',   /* § 6 -> ^ */
            [10] = '{',   /* ç 9 -> { */
            [11] = '}',   /* à 0 -> } */
            [12] = '\\',  /* ) ° -> \ (alias) */
            [26] = '[',   /* ^ ¨ -> [ */
            [27] = ']',   /* $ * -> ] */
            [43] = '`',   /* µ £ -> ` */
            [53] = '~',   /* = + -> ~ */
            [86] = '\\'   /* < > -> \ */
        };
        return third[code];
    }
    if (s->left_ctrl || s->right_ctrl) {
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 1);
        if (c == '[') return 27;
        return 0;
    }
    bool shift = s->left_shift || s->right_shift;

    if (layout == KBD_LAYOUT_AZERTY && code >= 2 && code <= 13) {
        bool num_shifted = shift ^ s->caps;
        if (num_shifted && shifted_table[code]) return shifted_table[code];
        return c;
    }

    if (shift && shifted_table[code]) return shifted_table[code];
    if (c >= 'a' && c <= 'z') return (shift != s->caps) ? c - 'a' + 'A' : c;
    return c;
}

size_t keyboard_decode_bytes(keyboard_decoder_t *s, uint8_t code, char out[4]) {
    bool ext = s->extended;
    char c = keyboard_decode(s, code);
    if (c) { out[0] = c; return 1; }
    if (code & 0x80) return 0;
    if (!ext && (code < 0x47 || code > 0x53)) return 0;
    char final = 0;
    switch (code) {
        case 0x48: final = 'A'; break; /* Up */
        case 0x50: final = 'B'; break; /* Down */
        case 0x4d: final = 'C'; break; /* Right */
        case 0x4b: final = 'D'; break; /* Left */
        case 0x47: final = 'H'; break; /* Home */
        case 0x4f: final = 'F'; break; /* End */
        case 0x53: out[0]=27; out[1]='['; out[2]='3'; out[3]='~'; return 4; /* Del */
        case 0x52: out[0]=27; out[1]='['; out[2]='2'; out[3]='~'; return 4; /* Ins */
        case 0x49: out[0]=27; out[1]='['; out[2]='5'; out[3]='~'; return 4; /* PgUp */
        case 0x51: out[0]=27; out[1]='['; out[2]='6'; out[3]='~'; return 4; /* PgDn */
        default: return 0;
    }
    out[0]=27; out[1]='['; out[2]=final; return 3;
}

uint16_t keyboard_modifiers(const keyboard_decoder_t *s) {
    uint16_t m = 0;
    if (s->left_shift || s->right_shift) m |= KEY_MOD_SHIFT;
    if (s->left_ctrl || s->right_ctrl) m |= KEY_MOD_CTRL;
    if (s->left_alt || s->altgr) m |= KEY_MOD_ALT;
    if (s->left_super || s->right_super) m |= KEY_MOD_SUPER;
    if (s->caps) m |= KEY_MOD_CAPS;
    if (!s->num_lock_disabled) m |= KEY_MOD_NUMLOCK;
    if (s->altgr) m |= KEY_MOD_ALTGR;
    return m;
}

/* E0-prefixed set 1 make code -> Linux keycode; 0 = no event (including the
 * fake shifts E0 2A / E0 36 that surround Print Screen and the arrows). */
static uint16_t extended_keycode(uint8_t code) {
    switch (code) {
        case 0x1c: return KEY_KPENTER;
        case 0x1d: return KEY_RIGHTCTRL;
        case 0x35: return KEY_KPSLASH;
        case 0x37: return KEY_SYSRQ;
        case 0x38: return KEY_RIGHTALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGEUP;
        case 0x4b: return KEY_LEFT;
        case 0x4d: return KEY_RIGHT;
        case 0x4f: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5b: return KEY_LEFTMETA;
        case 0x5c: return KEY_RIGHTMETA;
        case 0x5d: return KEY_COMPOSE;
        default: return 0;
    }
}

size_t keyboard_decode_event(keyboard_decoder_t *s, uint8_t code, char out[4], key_event_t *ev) {
    *ev = (key_event_t){0};
    bool ext = s->extended;
    unsigned pause = s->pause_remaining;
    size_t n = keyboard_decode_bytes(s, code, out);
    if (pause) {
        if (pause == 1) {
            ev->keycode = KEY_PAUSE;
            ev->state = KEY_STATE_RELEASE;
            ev->modifiers = keyboard_modifiers(s);
        }
        return n;
    }
    if (code == 0xe1) {
        ev->keycode = KEY_PAUSE;
        ev->state = KEY_STATE_PRESS;
        ev->modifiers = keyboard_modifiers(s);
        return n;
    }
    if (code == 0xe0) return n;
    bool released = (code & 0x80) != 0;
    uint8_t sc = code & 0x7f;
    /* Linux keycodes 1..88 equal the non-extended set 1 make codes. */
    uint16_t key = ext ? extended_keycode(sc) : (sc >= 1 && sc <= 88 ? sc : 0);
    if (!key) return n;
    if (key == KEY_LEFTALT) s->left_alt = !released;
    else if (key == KEY_LEFTMETA) s->left_super = !released;
    else if (key == KEY_RIGHTMETA) s->right_super = !released;
    uint8_t bit = (uint8_t)(1u << (key & 7));
    bool was_down = (s->down[key >> 3] & bit) != 0;
    if (released) s->down[key >> 3] &= (uint8_t)~bit;
    else s->down[key >> 3] |= bit;
    ev->keycode = key;
    ev->state = released ? KEY_STATE_RELEASE : (was_down ? KEY_STATE_REPEAT : KEY_STATE_PRESS);
    ev->modifiers = keyboard_modifiers(s);
    ev->ascii = (!released && n == 1) ? (uint8_t)out[0] : 0;
    return n;
}
