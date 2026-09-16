#include "keyboard.h"

static const char plain[128] = {
    [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='-',[13]='=',[14]='\b',[15]='\t',
    [16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
    [26]='[',[27]=']',[28]='\n',
    [30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
    [39]=';',[40]='\'',[41]='`',[43]='\\',
    [44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',
    [51]=',',[52]='.',[53]='/',[55]='*',[57]=' '
};
static const char shifted[128] = {
    [2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',[10]='(',[11]=')',
    [12]='_',[13]='+',[26]='{',[27]='}',[39]=':',[40]='"',[41]='~',[43]='|',
    [51]='<',[52]='>',[53]='?'
};

char keyboard_decode(keyboard_decoder_t *s, uint8_t code) {
    if (s->pause_remaining) { s->pause_remaining--; return 0; }
    if (code == 0xe1) { s->pause_remaining = 5; s->extended = false; return 0; }
    if (code == 0xe0) { s->extended = true; return 0; }
    bool released = (code & 0x80) != 0;
    code &= 0x7f;
    if (s->extended) {
        s->extended = false;
        /* Ignore arrows and PrintScreen's synthetic shift bytes. */
        return released ? 0 : code == 0x1c ? '\n' : code == 0x35 ? '/' : 0;
    }
    if (code == 42) { s->left_shift = !released; return 0; }
    if (code == 54) { s->right_shift = !released; return 0; }
    if (code == 58) {
        if (!released && !s->caps_down) s->caps = !s->caps;
        s->caps_down = !released;
        return 0;
    }
    if (released) return 0;
    char c = plain[code];
    bool shift = s->left_shift || s->right_shift;
    if (c >= 'a' && c <= 'z') return (shift != s->caps) ? c - 'a' + 'A' : c;
    return shift && shifted[code] ? shifted[code] : c;
}
