#include "keyboard.h"

static int g_keyboard_layout = KBD_LAYOUT_US;

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
    [51]=',',[52]='.',[53]='/',[55]='*',[57]=' '
};

static const char us_shifted[128] = {
    [2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',[10]='(',[11]=')',
    [12]='_',[13]='+',[26]='{',[27]='}',[39]=':',[40]='"',[41]='~',[43]='|',
    [51]='<',[52]='>',[53]='?'
};

/* Belgian AZERTY layout mapped to standard 7-bit ASCII */
static const char azerty_plain[128] = {
    [2]='&',[3]='e',[4]='"',[5]='\'',[6]='(',[7]='-',[8]='e',[9]='!',[10]='c',[11]='a',
    [12]=')',[13]='-',[14]='\b',[15]='\t',
    [16]='a',[17]='z',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
    [26]='^',[27]='$',[28]='\n',
    [30]='q',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
    [39]='m',[40]='u',[41]='`',[43]='<',
    [44]='w',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]=',',
    [51]=';',[52]=':',[53]='=',[55]='*',[57]=' '
};

static const char azerty_shifted[128] = {
    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='o',[13]='_',[26]='^',[27]='*',[39]='M',[40]='%',[41]='~',[43]='>',
    [50]='?',[51]='.',[52]='/',[53]='+'
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
        if (code == 0x1c && !released) return '\n'; /* Keypad Enter */
        return 0;
    }
    if (code == 42) { s->left_shift = !released; return 0; }
    if (code == 54) { s->right_shift = !released; return 0; }
    if (code == 58) {
        if (!released && !s->caps_down) s->caps = !s->caps;
        s->caps_down = !released;
        return 0;
    }
    if (released) return 0;

    const char *plain_table = (g_keyboard_layout == KBD_LAYOUT_AZERTY) ? azerty_plain : us_plain;
    const char *shifted_table = (g_keyboard_layout == KBD_LAYOUT_AZERTY) ? azerty_shifted : us_shifted;

    char c = plain_table[code];
    bool shift = s->left_shift || s->right_shift;
    if (c >= 'a' && c <= 'z') return (shift != s->caps) ? c - 'a' + 'A' : c;
    return shift && shifted_table[code] ? shifted_table[code] : c;
}