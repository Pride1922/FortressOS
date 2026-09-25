#include "keyboard.h"

static int g_keyboard_layout = KBD_LAYOUT_AZERTY;

void keyboard_set_layout(int layout) {
    if (layout == KBD_LAYOUT_AZERTY) {
        g_keyboard_layout = KBD_LAYOUT_AZERTY;
    } else {
        g_keyboard_layout = KBD_LAYOUT_US;
    }
}

int keyboard_get_layout(void) {
    return g_keyboard_layout;
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

    const char *plain_table = (g_keyboard_layout == KBD_LAYOUT_AZERTY) ? azerty_plain : us_plain;
    const char *shifted_table = (g_keyboard_layout == KBD_LAYOUT_AZERTY) ? azerty_shifted : us_shifted;

    char c = plain_table[code];
    if (s->altgr && g_keyboard_layout == KBD_LAYOUT_AZERTY) {
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

    if (g_keyboard_layout == KBD_LAYOUT_AZERTY && code >= 2 && code <= 13) {
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
