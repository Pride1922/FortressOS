#include "nano.h"

#ifdef NANO_HOST_TEST
#include <string.h>
#else
/* Freestanding helper routines */
static void *nano_memcpy(void *dst, const void *src, size_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *nano_memmove(void *dst, const void *src, size_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

__attribute__((unused)) static void *nano_memset(void *dst, int val, size_t n) {
    char *d = (char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (char)val;
    return dst;
}

static size_t nano_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

__attribute__((unused)) static int nano_memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

static int nano_strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static char *nano_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

static int nano_strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') break;
    }
    return 0;
}

#define memcpy nano_memcpy
#define memmove nano_memmove
#define memset nano_memset
#define strlen nano_strlen
#define memcmp nano_memcmp
#define strcmp nano_strcmp
#define strncmp nano_strncmp
#define strstr nano_strstr
#endif

static char to_lower_char(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

void nano_init(nano_state_t *s, char *pool) {
    if (!s) return;
    (void)pool;

    s->num_rows = 1;
    s->total_bytes = 0;
    s->rows[0].offset = 0;
    s->rows[0].length = 0;
    s->rows[0].render_len = 0;

    s->cx = 0;
    s->cy = 0;
    s->row_offset = 0;
    s->col_offset = 0;
    s->screen_rows = 25;
    s->screen_cols = 80;
    s->modified = false;
    s->readonly = false;
    s->dos_mode = false;
    s->show_line_numbers = false;
    s->case_sensitive = false;
    s->regex_search = false;
    s->tab_size = 8;
    s->tab_to_spaces = false;
    s->buffer_idx = 0;
    s->buffer_count = 1;
    s->filename[0] = '\0';
    s->status_msg[0] = '\0';
    s->cut_len = 0;
    s->has_cut = false;
    s->undo_stack.count = 0;
    s->undo_stack.current = 0;
}

size_t nano_gutter_width(const nano_state_t *s) {
    if (!s || !s->show_line_numbers) return 0;
    size_t rows = s->num_rows ? s->num_rows : 1;
    size_t digits = 0;
    while (rows > 0) {
        digits++;
        rows /= 10;
    }
    if (digits < 3) digits = 3; /* Minimum 3 digits width (e.g. "   1 | ") */
    return digits + 3;          /* digits + " | " */
}

uint16_t nano_calc_render_len(const nano_state_t *s, const char *chars, size_t len) {
    size_t tab_stop = (s && s->tab_size >= 1 && s->tab_size <= 16) ? s->tab_size : NANO_TAB_STOP;
    size_t rx = 0;
    for (size_t i = 0; i < len; i++) {
        if (chars[i] == '\t') {
            rx += tab_stop - (rx % tab_stop);
        } else {
            rx++;
        }
    }
    return (uint16_t)(rx > 65535 ? 65535 : rx);
}

size_t nano_col_to_render(const nano_state_t *s, const char *chars, size_t len, size_t col) {
    if (col > len) col = len;
    size_t tab_stop = (s && s->tab_size >= 1 && s->tab_size <= 16) ? s->tab_size : NANO_TAB_STOP;
    size_t rx = 0;
    for (size_t i = 0; i < col; i++) {
        if (chars[i] == '\t') {
            rx += tab_stop - (rx % tab_stop);
        } else {
            rx++;
        }
    }
    return rx;
}

bool nano_load_buffer(nano_state_t *s, char *pool, const char *data, size_t size) {
    if (!s || !pool) return false;
    if (size > NANO_POOL_SIZE) return false;

    s->num_rows = 0;
    s->total_bytes = 0;
    s->cx = 0;
    s->cy = 0;
    s->row_offset = 0;
    s->col_offset = 0;
    s->modified = false;
    s->status_msg[0] = '\0';

    if (!data || size == 0) {
        return true;
    }

    size_t line_start = 0;
    size_t crlf_count = 0;
    size_t lf_count = 0;
    s->num_rows = 0;
    s->total_bytes = 0;

    while (line_start < size) {
        if (s->num_rows >= NANO_MAX_ROWS) {
            return false;
        }

        /* Find newline */
        size_t next_nl = line_start;
        while (next_nl < size && data[next_nl] != '\n' && data[next_nl] != '\r') {
            next_nl++;
        }

        size_t line_len = next_nl - line_start;

        /* If line exceeds max length, split it into chunks of NANO_MAX_LINE_LEN */
        size_t chunk_start = line_start;
        size_t remaining = line_len;

        do {
            if (s->num_rows >= NANO_MAX_ROWS) {
                return false;
            }

            size_t chunk_len = remaining;
            if (chunk_len > NANO_MAX_LINE_LEN) {
                chunk_len = NANO_MAX_LINE_LEN;
            }

            if (s->total_bytes + chunk_len > NANO_POOL_SIZE) {
                return false;
            }

            memmove(pool + s->total_bytes, data + chunk_start, chunk_len);
            s->rows[s->num_rows].offset = (uint32_t)s->total_bytes;
            s->rows[s->num_rows].length = (uint16_t)chunk_len;
            s->rows[s->num_rows].render_len = nano_calc_render_len(s, pool + s->total_bytes, chunk_len);

            s->total_bytes += chunk_len;
            s->num_rows++;

            chunk_start += chunk_len;
            remaining -= chunk_len;
        } while (remaining > 0);

        /* Advance past \r\n or \n or \r */
        if (next_nl < size) {
            if (data[next_nl] == '\r' && next_nl + 1 < size && data[next_nl + 1] == '\n') {
                crlf_count++;
                line_start = next_nl + 2;
            } else {
                lf_count++;
                line_start = next_nl + 1;
            }
            /* If file ends with newline, trailing empty line is handled below */
            if (line_start == size && s->num_rows < NANO_MAX_ROWS) {
                s->rows[s->num_rows].offset = (uint32_t)s->total_bytes;
                s->rows[s->num_rows].length = 0;
                s->rows[s->num_rows].render_len = 0;
                s->num_rows++;
            }
        } else {
            line_start = next_nl;
        }
    }

    if (s->num_rows == 0) {
        s->rows[0].offset = 0;
        s->rows[0].length = 0;
        s->rows[0].render_len = 0;
        s->num_rows = 1;
    }

    s->dos_mode = (crlf_count > lf_count);
    s->modified = false;
    return true;
}

bool nano_insert_char(nano_state_t *s, char *pool, char c) {
    if (!s || !pool) return false;
    if (s->total_bytes + 1 > NANO_POOL_SIZE) return false;
    if (s->cy >= s->num_rows) return false;
    if (s->rows[s->cy].length >= NANO_MAX_LINE_LEN) return false;

    if (s->cx > s->rows[s->cy].length) {
        s->cx = s->rows[s->cy].length;
    }

    uint32_t pos = s->rows[s->cy].offset + (uint32_t)s->cx;
    size_t tail = s->total_bytes - pos;

    if (tail > 0) {
        memmove(pool + pos + 1, pool + pos, tail);
    }
    pool[pos] = c;

    s->rows[s->cy].length++;
    s->total_bytes++;

    for (size_t i = s->cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset++;
    }

    s->rows[s->cy].render_len = nano_calc_render_len(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length);
    s->cx++;
    s->modified = true;
    return true;
}

bool nano_split_row(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    if (s->num_rows >= NANO_MAX_ROWS) return false;
    if (s->cy >= s->num_rows) return false;

    if (s->cx > s->rows[s->cy].length) {
        s->cx = s->rows[s->cy].length;
    }

    uint16_t len1 = (uint16_t)s->cx;
    uint16_t len2 = (uint16_t)(s->rows[s->cy].length - len1);

    /* Make room in rows array */
    for (size_t i = s->num_rows; i > s->cy + 1; i--) {
        s->rows[i] = s->rows[i - 1];
    }

    s->rows[s->cy + 1].offset = s->rows[s->cy].offset + len1;
    s->rows[s->cy + 1].length = len2;
    s->rows[s->cy + 1].render_len = nano_calc_render_len(s, pool + s->rows[s->cy + 1].offset, len2);

    s->rows[s->cy].length = len1;
    s->rows[s->cy].render_len = nano_calc_render_len(s, pool + s->rows[s->cy].offset, len1);

    s->num_rows++;
    s->cy++;
    s->cx = 0;
    s->modified = true;
    return true;
}

bool nano_delete_char(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    if (s->cy >= s->num_rows) return false;

    if (s->cx < s->rows[s->cy].length) {
        uint32_t pos = s->rows[s->cy].offset + (uint32_t)s->cx;
        size_t tail = s->total_bytes - (pos + 1);

        if (tail > 0) {
            memmove(pool + pos, pool + pos + 1, tail);
        }

        s->rows[s->cy].length--;
        s->total_bytes--;

        for (size_t i = s->cy + 1; i < s->num_rows; i++) {
            s->rows[i].offset--;
        }

        s->rows[s->cy].render_len = nano_calc_render_len(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length);
        s->modified = true;
        return true;
    } else if (s->cx == s->rows[s->cy].length && s->cy + 1 < s->num_rows) {
        /* Merge line cy + 1 into cy */
        if (s->rows[s->cy].length + s->rows[s->cy + 1].length > NANO_MAX_LINE_LEN) {
            return false;
        }

        s->rows[s->cy].length += s->rows[s->cy + 1].length;
        s->rows[s->cy].render_len = nano_calc_render_len(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length);

        for (size_t i = s->cy + 1; i < s->num_rows - 1; i++) {
            s->rows[i] = s->rows[i + 1];
        }
        s->num_rows--;
        s->modified = true;
        return true;
    }

    return false;
}

bool nano_backspace(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    if (s->cx > 0) {
        s->cx--;
        return nano_delete_char(s, pool);
    } else if (s->cx == 0 && s->cy > 0) {
        if (s->rows[s->cy - 1].length + s->rows[s->cy].length > NANO_MAX_LINE_LEN) {
            return false;
        }

        size_t new_cx = s->rows[s->cy - 1].length;
        s->rows[s->cy - 1].length += s->rows[s->cy].length;
        s->rows[s->cy - 1].render_len = nano_calc_render_len(s, pool + s->rows[s->cy - 1].offset, s->rows[s->cy - 1].length);

        for (size_t i = s->cy; i < s->num_rows - 1; i++) {
            s->rows[i] = s->rows[i + 1];
        }
        s->num_rows--;
        s->cy--;
        s->cx = new_cx;
        s->modified = true;
        return true;
    }
    return false;
}

void nano_move_left(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    if (s->cx > 0) {
        s->cx--;
    } else if (s->cy > 0) {
        s->cy--;
        s->cx = s->rows[s->cy].length;
    }
}

void nano_move_right(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    if (s->cy < s->num_rows && s->cx < s->rows[s->cy].length) {
        s->cx++;
    } else if (s->cy + 1 < s->num_rows) {
        s->cy++;
        s->cx = 0;
    }
}

void nano_move_up(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    if (s->cy > 0) {
        s->cy--;
        if (s->cx > s->rows[s->cy].length) {
            s->cx = s->rows[s->cy].length;
        }
    }
}

void nano_move_down(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    if (s->cy + 1 < s->num_rows) {
        s->cy++;
        if (s->cx > s->rows[s->cy].length) {
            s->cx = s->rows[s->cy].length;
        }
    }
}

void nano_move_home(nano_state_t *s) {
    if (!s) return;
    s->cx = 0;
}

void nano_move_end(nano_state_t *s) {
    if (!s) return;
    if (s->cy < s->num_rows) {
        s->cx = s->rows[s->cy].length;
    }
}

void nano_page_up(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
    if (s->cy > page) {
        s->cy -= page;
    } else {
        s->cy = 0;
    }
    if (s->cx > s->rows[s->cy].length) {
        s->cx = s->rows[s->cy].length;
    }
}

void nano_page_down(nano_state_t *s, const char *pool) {
    (void)pool;
    if (!s) return;
    size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
    s->cy += page;
    if (s->cy >= s->num_rows) {
        s->cy = s->num_rows - 1;
    }
    if (s->cx > s->rows[s->cy].length) {
        s->cx = s->rows[s->cy].length;
    }
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void nano_word_left(nano_state_t *s, const char *pool) {
    if (!s || !pool) return;
    if (s->cy >= s->num_rows) return;

    if (s->cx == 0) {
        if (s->cy > 0) {
            s->cy--;
            s->cx = s->rows[s->cy].length;
        }
        return;
    }

    const char *line = pool + s->rows[s->cy].offset;
    size_t pos = s->cx;

    /* Skip whitespace before current position */
    while (pos > 0 && (line[pos - 1] == ' ' || line[pos - 1] == '\t')) {
        pos--;
    }

    if (pos > 0) {
        bool in_word = is_word_char(line[pos - 1]);
        while (pos > 0) {
            char prev = line[pos - 1];
            if (prev == ' ' || prev == '\t') break;
            if (is_word_char(prev) != in_word) break;
            pos--;
        }
    }

    s->cx = pos;
}

void nano_word_right(nano_state_t *s, const char *pool) {
    if (!s || !pool) return;
    if (s->cy >= s->num_rows) return;

    size_t len = s->rows[s->cy].length;
    if (s->cx >= len) {
        if (s->cy + 1 < s->num_rows) {
            s->cy++;
            s->cx = 0;
        }
        return;
    }

    const char *line = pool + s->rows[s->cy].offset;
    size_t pos = s->cx;

    /* Move through current word/punctuation block */
    bool in_word = is_word_char(line[pos]);
    while (pos < len) {
        char cur = line[pos];
        if (cur == ' ' || cur == '\t') break;
        if (is_word_char(cur) != in_word) break;
        pos++;
    }

    /* Skip trailing spaces to land on next word start */
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    s->cx = pos;
}

void nano_go_to_line(nano_state_t *s, size_t target_row, size_t target_col) {
    if (!s) return;
    if (s->num_rows == 0) return;

    if (target_row == 0) target_row = 1;
    if (target_row > s->num_rows) target_row = s->num_rows;
    s->cy = target_row - 1;

    size_t line_len = s->rows[s->cy].length;
    if (target_col == 0) target_col = 1;
    if (target_col - 1 > line_len) {
        s->cx = line_len;
    } else {
        s->cx = target_col - 1;
    }

    size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
    s->row_offset = (s->cy > page / 2) ? (s->cy - page / 2) : 0;
}

bool nano_cut_line(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    if (s->cy >= s->num_rows) return false;

    uint32_t pos = s->rows[s->cy].offset;
    uint16_t len = s->rows[s->cy].length;

    size_t copy_len = len;
    if (copy_len >= sizeof(s->cut_buffer)) {
        copy_len = sizeof(s->cut_buffer) - 1;
    }
    memcpy(s->cut_buffer, pool + pos, copy_len);
    s->cut_len = (uint16_t)copy_len;
    s->has_cut = true;

    size_t tail = s->total_bytes - (pos + len);
    if (tail > 0) {
        memmove(pool + pos, pool + pos + len, tail);
    }
    s->total_bytes -= len;

    for (size_t i = s->cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset -= len;
    }

    if (s->num_rows == 1) {
        s->rows[0].length = 0;
        s->rows[0].render_len = 0;
        s->cx = 0;
    } else {
        for (size_t i = s->cy; i < s->num_rows - 1; i++) {
            s->rows[i] = s->rows[i + 1];
        }
        s->num_rows--;
        if (s->cy >= s->num_rows) {
            s->cy = s->num_rows - 1;
        }
        if (s->cx > s->rows[s->cy].length) {
            s->cx = s->rows[s->cy].length;
        }
    }

    s->modified = true;
    return true;
}

bool nano_uncut_line(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    if (!s->has_cut) return false;
    if (s->total_bytes + s->cut_len > NANO_POOL_SIZE) return false;
    if (s->num_rows >= NANO_MAX_ROWS) return false;
    if (s->cy >= s->num_rows) s->cy = s->num_rows - 1;

    uint32_t pos = s->rows[s->cy].offset;
    size_t tail = s->total_bytes - pos;

    if (tail > 0) {
        memmove(pool + pos + s->cut_len, pool + pos, tail);
    }
    memcpy(pool + pos, s->cut_buffer, s->cut_len);
    s->total_bytes += s->cut_len;

    for (size_t i = s->num_rows; i > s->cy; i--) {
        s->rows[i] = s->rows[i - 1];
    }

    s->rows[s->cy].offset = pos;
    s->rows[s->cy].length = s->cut_len;
    s->rows[s->cy].render_len = nano_calc_render_len(s, s->cut_buffer, s->cut_len);
    s->num_rows++;

    for (size_t i = s->cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset += s->cut_len;
    }

    s->cx = 0;
    s->modified = true;
    return true;
}

static inline void set_regex_bit(uint8_t *set, unsigned char b) {
    set[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static inline bool test_regex_bit(const uint8_t *set, unsigned char b) {
    return (set[b >> 3] & (1u << (b & 7))) != 0;
}

static inline void set_regex_bit_case(uint8_t *set, unsigned char b, bool icase) {
    set_regex_bit(set, b);
    if (icase) {
        if (b >= 'a' && b <= 'z') set_regex_bit(set, (unsigned char)(b - ('a' - 'A')));
        else if (b >= 'A' && b <= 'Z') set_regex_bit(set, (unsigned char)(b + ('a' - 'A')));
    }
}

static inline uint64_t nano_nfa_closure(const nano_regex_t *re, uint64_t states) {
    for (int i = 0; i < re->num_tokens; i++) {
        if ((states & (1ULL << i)) && re->tokens[i].star) {
            states |= (1ULL << (i + 1));
        }
    }
    return states;
}

static inline bool nano_match_token(const nano_regex_token_t *tok, unsigned char c, bool icase) {
    if (c == '\0' || c == '\n' || c == '\r') return false;
    switch (tok->kind) {
    case TOK_DOT:
        return true;
    case TOK_LITERAL: {
        char c1 = tok->ch;
        char c2 = (char)c;
        if (icase) {
            c1 = to_lower_char(c1);
            c2 = to_lower_char(c2);
        }
        return c1 == c2;
    }
    case TOK_CLASS:
        return test_regex_bit(tok->cls, c);
    case TOK_NCLASS:
        return !test_regex_bit(tok->cls, c);
    }
    return false;
}

static inline uint64_t nano_nfa_step(const nano_regex_t *re, uint64_t states, unsigned char c) {
    uint64_t next_states = 0;
    for (int i = 0; i < re->num_tokens; i++) {
        if (states & (1ULL << i)) {
            if (nano_match_token(&re->tokens[i], c, re->ignore_case)) {
                if (re->tokens[i].star) {
                    next_states |= (1ULL << i) | (1ULL << (i + 1));
                } else {
                    next_states |= (1ULL << (i + 1));
                }
            }
        }
    }
    return nano_nfa_closure(re, next_states);
}

bool nano_regex_compile(nano_regex_t *re, const char *pattern, bool icase) {
    if (!re || !pattern) return false;
    for (size_t i = 0; i < sizeof(*re); i++) ((char *)re)[i] = 0;
    re->ignore_case = icase;

    const char *p = pattern;
    if (*p == '^') {
        re->anchor_start = true;
        p++;
    }

    while (*p) {
        if (*p == '$' && p[1] == '\0') {
            re->anchor_end = true;
            p++;
            break;
        }

        if (re->num_tokens >= NANO_REGEX_MAX_TOKENS) return false;
        nano_regex_token_t *tok = &re->tokens[re->num_tokens];

        if (*p == '\\' && p[1]) {
            p++;
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            re->num_tokens++;
        } else if (*p == '.') {
            tok->kind = TOK_DOT;
            tok->star = false;
            p++;
            re->num_tokens++;
        } else if (*p == '[') {
            p++;
            bool negated = false;
            if (*p == '^') {
                negated = true;
                p++;
            }
            tok->kind = negated ? TOK_NCLASS : TOK_CLASS;
            tok->star = false;

            if (*p == ']') {
                set_regex_bit_case(tok->cls, (unsigned char)*p, icase);
                p++;
            }

            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    unsigned char start = (unsigned char)p[0];
                    unsigned char end = (unsigned char)p[2];
                    if (start > end) {
                        unsigned char tmp = start; start = end; end = tmp;
                    }
                    for (unsigned i = start; i <= end; i++) {
                        set_regex_bit_case(tok->cls, (unsigned char)i, icase);
                    }
                    p += 3;
                } else {
                    set_regex_bit_case(tok->cls, (unsigned char)*p, icase);
                    p++;
                }
            }
            if (*p == ']') p++;
            re->num_tokens++;
        } else if (*p == '*') {
            if (re->num_tokens > 0) {
                re->tokens[re->num_tokens - 1].star = true;
            }
            while (*p == '*') p++;
            continue;
        } else if (*p == '+') {
            if (re->num_tokens > 0 && re->num_tokens < NANO_REGEX_MAX_TOKENS) {
                /* X+ is X followed by X* */
                re->tokens[re->num_tokens] = re->tokens[re->num_tokens - 1];
                re->tokens[re->num_tokens].star = true;
                re->num_tokens++;
            }
            while (*p == '+') p++;
            continue;
        } else {
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            re->num_tokens++;
        }
    }
    return true;
}

bool nano_regex_match_at(const nano_regex_t *re, const char *text, size_t text_len, size_t pos, size_t *out_match_len) {
    if (!re || !text || pos > text_len) return false;
    if (re->anchor_start && pos != 0) return false;

    uint64_t accept_mask = (1ULL << re->num_tokens);
    uint64_t states = nano_nfa_closure(re, 1ULL << 0);
    size_t last_match_len = 0;
    bool matched = false;

    if (!re->anchor_end && (states & accept_mask)) {
        matched = true;
        last_match_len = 0;
    }

    for (size_t i = pos; i < text_len; i++) {
        states = nano_nfa_step(re, states, (unsigned char)text[i]);
        if (states == 0) break;
        if (states & accept_mask) {
            matched = true;
            last_match_len = (i - pos) + 1;
        }
    }

    if (re->anchor_end && pos + last_match_len != text_len) {
        return false;
    }

    if (matched) {
        if (out_match_len) *out_match_len = last_match_len;
        return true;
    }
    return false;
}

static bool match_query_at_custom(const nano_state_t *s, const char *text, size_t text_len, size_t pos,
                                  const char *query, size_t qlen, const nano_regex_t *re, size_t *out_match_len) {
    if (!s || !text || !query) return false;

    if (s->regex_search && re) {
        return nano_regex_match_at(re, text, text_len, pos, out_match_len);
    }

    if (pos + qlen > text_len) return false;
    bool cs = s->case_sensitive;
    for (size_t i = 0; i < qlen; i++) {
        char c1 = text[pos + i];
        char c2 = query[i];
        if (!cs) {
            c1 = to_lower_char(c1);
            c2 = to_lower_char(c2);
        }
        if (c1 != c2) return false;
    }
    if (out_match_len) *out_match_len = qlen;
    return true;
}

bool nano_search(nano_state_t *s, const char *pool, const char *query, size_t *out_match_len) {
    if (!s || !pool || !query || query[0] == '\0') return false;
    size_t qlen = strlen(query);

    nano_regex_t re;
    nano_regex_t *p_re = NULL;
    if (s->regex_search) {
        if (!nano_regex_compile(&re, query, !s->case_sensitive)) {
            return false;
        }
        p_re = &re;
    }

    /* Search forward starting from cursor + 1 */
    size_t start_col = s->cx + 1;
    for (size_t r = s->cy; r < s->num_rows; r++) {
        const char *line = pool + s->rows[r].offset;
        size_t len = s->rows[r].length;
        for (size_t c = (r == s->cy ? start_col : 0); c <= len; c++) {
            size_t mlen = 0;
            if (match_query_at_custom(s, line, len, c, query, qlen, p_re, &mlen)) {
                s->cy = r;
                s->cx = c;
                size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
                s->row_offset = (s->cy > page / 2) ? (s->cy - page / 2) : 0;
                if (out_match_len) *out_match_len = mlen;
                return true;
            }
        }
    }

    /* Wrap around from top */
    for (size_t r = 0; r <= s->cy; r++) {
        const char *line = pool + s->rows[r].offset;
        size_t len = s->rows[r].length;
        size_t max_col = (r == s->cy ? s->cx : len);
        for (size_t c = 0; c <= len && (r < s->cy || c <= max_col); c++) {
            size_t mlen = 0;
            if (match_query_at_custom(s, line, len, c, query, qlen, p_re, &mlen)) {
                s->cy = r;
                s->cx = c;
                size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
                s->row_offset = (s->cy > page / 2) ? (s->cy - page / 2) : 0;
                if (out_match_len) *out_match_len = mlen;
                return true;
            }
        }
    }

    return false;
}

bool nano_replace_current_match(nano_state_t *s, char *pool, const char *query, const char *replacement) {
    if (!s || !pool || !query || !replacement) return false;
    if (s->cy >= s->num_rows) return false;

    size_t qlen = strlen(query);
    size_t rlen = strlen(replacement);
    size_t line_len = s->rows[s->cy].length;

    nano_regex_t re;
    nano_regex_t *p_re = NULL;
    if (s->regex_search) {
        if (!nano_regex_compile(&re, query, !s->case_sensitive)) {
            return false;
        }
        p_re = &re;
    }

    size_t match_len = 0;
    const char *line = pool + s->rows[s->cy].offset;
    if (!match_query_at_custom(s, line, line_len, s->cx, query, qlen, p_re, &match_len)) {
        return false;
    }
    if (match_len == 0) match_len = qlen;

    /* Check capacity */
    if (rlen > match_len) {
        size_t diff = rlen - match_len;
        if (s->total_bytes + diff > NANO_POOL_SIZE) return false;
        if (line_len + diff > NANO_MAX_LINE_LEN) return false;
    }

    /* Record undo before replacing */
    nano_undo_push(s, UNDO_OP_DELETE, s->cy, s->cx, pool + s->rows[s->cy].offset + s->cx, match_len);

    uint32_t pos = s->rows[s->cy].offset + (uint32_t)s->cx;
    size_t tail = s->total_bytes - (pos + match_len);

    if (rlen != match_len) {
        if (tail > 0) {
            memmove(pool + pos + rlen, pool + pos + match_len, tail);
        }
        if (rlen > match_len) {
            size_t diff = rlen - match_len;
            s->total_bytes += diff;
            s->rows[s->cy].length += (uint16_t)diff;
            for (size_t i = s->cy + 1; i < s->num_rows; i++) {
                s->rows[i].offset += (uint32_t)diff;
            }
        } else {
            size_t diff = match_len - rlen;
            s->total_bytes -= diff;
            s->rows[s->cy].length -= (uint16_t)diff;
            for (size_t i = s->cy + 1; i < s->num_rows; i++) {
                s->rows[i].offset -= (uint32_t)diff;
            }
        }
    }

    if (rlen > 0) {
        memcpy(pool + pos, replacement, rlen);
    }
    s->rows[s->cy].render_len = nano_calc_render_len(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length);
    s->cx += rlen;
    s->modified = true;

    nano_undo_push(s, UNDO_OP_INSERT, s->cy, s->cx - rlen, replacement, rlen);
    return true;
}

void nano_undo_push(nano_state_t *s, nano_undo_op_type_t type, size_t cy, size_t cx, const char *text, size_t len) {
    if (!s || type == UNDO_OP_NONE) return;

    nano_undo_stack_t *st = &s->undo_stack;

    /* Typing coalescing: merge consecutive single-char inserts on same line adjacent cursor */
    if (type == UNDO_OP_INSERT && len == 1 && st->current > 0 && st->current <= st->count) {
        nano_undo_op_t *prev = &st->ops[st->current - 1];
        if (prev->type == UNDO_OP_INSERT && prev->cy == cy && prev->cx + prev->len == cx &&
            prev->len + 1 < sizeof(prev->text)) {
            prev->text[prev->len++] = text[0];
            return;
        }
    }

    /* Coalesce consecutive single-char deletes (backspace backward coalescing) */
    if (type == UNDO_OP_DELETE && len == 1 && st->current > 0 && st->current <= st->count) {
        nano_undo_op_t *prev = &st->ops[st->current - 1];
        if (prev->type == UNDO_OP_DELETE && prev->cy == cy && prev->cx == cx + 1 &&
            prev->len + 1 < sizeof(prev->text)) {
            memmove(prev->text + 1, prev->text, prev->len);
            prev->text[0] = text[0];
            prev->len++;
            prev->cx = cx;
            return;
        }
    }

    /* Invalidate any redo history beyond current index */
    st->count = st->current;

    /* If stack is full, evict oldest entry */
    if (st->count >= NANO_UNDO_STACK_SIZE) {
        for (size_t i = 0; i < NANO_UNDO_STACK_SIZE - 1; i++) {
            st->ops[i] = st->ops[i + 1];
        }
        st->count = NANO_UNDO_STACK_SIZE - 1;
        st->current = st->count;
    }

    nano_undo_op_t *op = &st->ops[st->current];
    op->type = type;
    op->cy = cy;
    op->cx = cx;
    if (len >= sizeof(op->text)) len = sizeof(op->text) - 1;
    op->len = len;
    if (text && len > 0) {
        memcpy(op->text, text, len);
    }
    op->text[len] = '\0';

    st->current++;
    st->count = st->current;
}

static bool nano_raw_insert_span(nano_state_t *s, char *pool, size_t cy, size_t cx, const char *text, size_t len) {
    if (!s || !pool || cy >= s->num_rows) return false;
    if (len == 0) return true;
    if (s->total_bytes + len > NANO_POOL_SIZE) return false;
    if (s->rows[cy].length + len > NANO_MAX_LINE_LEN) return false;

    if (cx > s->rows[cy].length) cx = s->rows[cy].length;
    uint32_t pos = s->rows[cy].offset + (uint32_t)cx;
    size_t tail = s->total_bytes - pos;

    if (tail > 0) {
        memmove(pool + pos + len, pool + pos, tail);
    }
    memcpy(pool + pos, text, len);

    s->rows[cy].length += (uint16_t)len;
    s->total_bytes += len;
    for (size_t i = cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset += (uint32_t)len;
    }
    s->rows[cy].render_len = nano_calc_render_len(s, pool + s->rows[cy].offset, s->rows[cy].length);
    s->cy = cy;
    s->cx = cx + len;
    s->modified = true;
    return true;
}

static bool nano_raw_delete_span(nano_state_t *s, char *pool, size_t cy, size_t cx, size_t len) {
    if (!s || !pool || cy >= s->num_rows) return false;
    if (len == 0) return true;
    if (cx + len > s->rows[cy].length) return false;

    uint32_t pos = s->rows[cy].offset + (uint32_t)cx;
    size_t tail = s->total_bytes - (pos + len);

    if (tail > 0) {
        memmove(pool + pos, pool + pos + len, tail);
    }
    s->rows[cy].length -= (uint16_t)len;
    s->total_bytes -= len;
    for (size_t i = cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset -= (uint32_t)len;
    }
    s->rows[cy].render_len = nano_calc_render_len(s, pool + s->rows[cy].offset, s->rows[cy].length);
    s->cy = cy;
    s->cx = cx;
    s->modified = true;
    return true;
}

static bool nano_raw_split(nano_state_t *s, char *pool, size_t cy, size_t cx) {
    if (!s || !pool || s->num_rows >= NANO_MAX_ROWS || cy >= s->num_rows) return false;
    s->cy = cy;
    s->cx = cx;
    return nano_split_row(s, pool);
}

static bool nano_raw_join(nano_state_t *s, char *pool, size_t cy) {
    if (!s || !pool || cy + 1 >= s->num_rows) return false;
    if (s->rows[cy].length + s->rows[cy + 1].length > NANO_MAX_LINE_LEN) return false;

    size_t prev_len = s->rows[cy].length;
    s->rows[cy].length += s->rows[cy + 1].length;
    s->rows[cy].render_len = nano_calc_render_len(s, pool + s->rows[cy].offset, s->rows[cy].length);

    for (size_t i = cy + 1; i < s->num_rows - 1; i++) {
        s->rows[i] = s->rows[i + 1];
    }
    s->num_rows--;
    s->cy = cy;
    s->cx = prev_len;
    s->modified = true;
    return true;
}

bool nano_undo(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    nano_undo_stack_t *st = &s->undo_stack;
    if (st->current == 0) return false;

    st->current--;
    nano_undo_op_t *op = &st->ops[st->current];

    switch (op->type) {
        case UNDO_OP_INSERT:
            /* Revert insertion: delete inserted span */
            (void)nano_raw_delete_span(s, pool, op->cy, op->cx, op->len);
            s->cy = op->cy;
            s->cx = op->cx;
            break;
        case UNDO_OP_DELETE:
            /* Revert deletion: re-insert deleted text */
            (void)nano_raw_insert_span(s, pool, op->cy, op->cx, op->text, op->len);
            s->cy = op->cy;
            s->cx = op->cx + op->len;
            break;
        case UNDO_OP_SPLIT:
            /* Revert row split: join rows */
            (void)nano_raw_join(s, pool, op->cy);
            s->cy = op->cy;
            s->cx = op->cx;
            break;
        case UNDO_OP_JOIN:
            /* Revert row join: split row at joined boundary */
            (void)nano_raw_split(s, pool, op->cy, op->cx);
            s->cy = op->cy;
            s->cx = op->cx;
            break;
        default:
            return false;
    }
    return true;
}

bool nano_redo(nano_state_t *s, char *pool) {
    if (!s || !pool) return false;
    nano_undo_stack_t *st = &s->undo_stack;
    if (st->current >= st->count) return false;

    nano_undo_op_t *op = &st->ops[st->current];
    st->current++;

    switch (op->type) {
        case UNDO_OP_INSERT:
            (void)nano_raw_insert_span(s, pool, op->cy, op->cx, op->text, op->len);
            s->cy = op->cy;
            s->cx = op->cx + op->len;
            break;
        case UNDO_OP_DELETE:
            (void)nano_raw_delete_span(s, pool, op->cy, op->cx, op->len);
            s->cy = op->cy;
            s->cx = op->cx;
            break;
        case UNDO_OP_SPLIT:
            (void)nano_raw_split(s, pool, op->cy, op->cx);
            break;
        case UNDO_OP_JOIN:
            (void)nano_raw_join(s, pool, op->cy);
            break;
        default:
            return false;
    }
    return true;
}

void nano_update_viewport(nano_state_t *s, const char *pool) {
    if (!s) return;
    size_t text_rows = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
    size_t total_cols = s->screen_cols ? s->screen_cols : 80;
    size_t gutter = nano_gutter_width(s);
    size_t text_cols = (total_cols > gutter) ? (total_cols - gutter) : 1;

    if (s->cy >= s->num_rows) {
        s->cy = s->num_rows ? s->num_rows - 1 : 0;
    }
    if (s->cx > s->rows[s->cy].length) {
        s->cx = s->rows[s->cy].length;
    }

    /* Vertical scroll */
    if (s->cy < s->row_offset) {
        s->row_offset = s->cy;
    }
    if (s->cy >= s->row_offset + text_rows) {
        s->row_offset = s->cy - text_rows + 1;
    }

    /* Horizontal scroll */
    size_t rx = nano_col_to_render(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length, s->cx);
    if (rx < s->col_offset) {
        s->col_offset = rx;
    }
    if (rx >= s->col_offset + text_cols) {
        s->col_offset = rx - text_cols + 1;
    }
}

static void buf_append_str(char *out, size_t *pos, size_t max, const char *str) {
    size_t i = 0;
    while (str && str[i] && *pos < max) {
        out[(*pos)++] = str[i++];
    }
}

static void buf_append_u32(char *out, size_t *pos, size_t max, uint32_t val) {
    if (val == 0) {
        if (*pos < max) out[(*pos)++] = '0';
        return;
    }
    char tmp[12];
    size_t tpos = 0;
    while (val > 0) {
        tmp[tpos++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (tpos > 0 && *pos < max) {
        out[(*pos)++] = tmp[--tpos];
    }
}

size_t nano_render_frame(nano_state_t *s, const char *pool, char *out_buf, size_t max_out) {
    if (!s || !out_buf || max_out < 128) return 0;
    size_t pos = 0;
    uint32_t cols = s->screen_cols ? s->screen_cols : 80;
    uint32_t rows = s->screen_rows ? s->screen_rows : 25;
    uint32_t text_rows = (rows >= 4) ? (rows - 3) : 1;
    size_t gutter = nano_gutter_width(s);
    size_t text_cols = (cols > gutter) ? (cols - gutter) : 1;

    nano_update_viewport(s, pool);

    /* Hide cursor, move home */
    buf_append_str(out_buf, &pos, max_out, "\x1b[?25l\x1b[H");

    /* Row 1: Header (Inverted video) */
    buf_append_str(out_buf, &pos, max_out, "\x1b[7m [ FortressOS Nano 2.0 ]");
    if (s->buffer_count > 1) {
        buf_append_str(out_buf, &pos, max_out, " [");
        buf_append_u32(out_buf, &pos, max_out, (uint32_t)(s->buffer_idx + 1));
        buf_append_str(out_buf, &pos, max_out, "/");
        buf_append_u32(out_buf, &pos, max_out, (uint32_t)s->buffer_count);
        buf_append_str(out_buf, &pos, max_out, "]");
    }
    buf_append_str(out_buf, &pos, max_out, "   File: ");
    const char *fn = s->filename[0] ? s->filename : "[New Buffer]";
    buf_append_str(out_buf, &pos, max_out, fn);
    if (s->readonly) {
        buf_append_str(out_buf, &pos, max_out, " [Read-Only]");
    }
    if (s->modified) {
        buf_append_str(out_buf, &pos, max_out, " [Modified]");
    }
    buf_append_str(out_buf, &pos, max_out, s->dos_mode ? " [DOS]" : " [Unix]");
    /* Clear line while inverted, then reset attributes */
    buf_append_str(out_buf, &pos, max_out, "\x1b[K\x1b[0m\r\n");

    /* Rows 2 .. H-2: Text viewport */
    for (uint32_t i = 0; i < text_rows; i++) {
        size_t line_idx = s->row_offset + i;

        if (line_idx < s->num_rows) {
            /* Render line number gutter if enabled */
            if (s->show_line_numbers && gutter > 0) {
                buf_append_str(out_buf, &pos, max_out, "\x1b[90m");
                size_t lno = line_idx + 1;
                char num_str[16];
                size_t npos = 0;
                while (lno > 0) {
                    num_str[npos++] = (char)('0' + (lno % 10));
                    lno /= 10;
                }
                size_t digits = (gutter >= 3) ? (gutter - 3) : 0;
                for (size_t p = npos; p < digits; p++) {
                    if (pos < max_out) out_buf[pos++] = ' ';
                }
                while (npos > 0) {
                    if (pos < max_out) out_buf[pos++] = num_str[--npos];
                }
                buf_append_str(out_buf, &pos, max_out, " | \x1b[0m");
            }

            const char *chars = pool + s->rows[line_idx].offset;
            size_t len = s->rows[line_idx].length;

            /* Expand tabs and crop to [col_offset, col_offset + text_cols) */
            size_t tab_stop = (s->tab_size >= 1 && s->tab_size <= 16) ? s->tab_size : NANO_TAB_STOP;
            size_t rx = 0;
            for (size_t c = 0; c < len; c++) {
                if (rx >= s->col_offset + text_cols) break;
                if (chars[c] == '\t') {
                    size_t tab_width = tab_stop - (rx % tab_stop);
                    for (size_t t = 0; t < tab_width; t++) {
                        if (rx >= s->col_offset && rx < s->col_offset + text_cols) {
                            if (pos < max_out) out_buf[pos++] = ' ';
                        }
                        rx++;
                    }
                } else {
                    if (rx >= s->col_offset && rx < s->col_offset + text_cols) {
                        if (pos < max_out) out_buf[pos++] = chars[c];
                    }
                    rx++;
                }
            }
        } else {
            if (s->show_line_numbers && gutter > 0) {
                buf_append_str(out_buf, &pos, max_out, "\x1b[90m");
                size_t digits = (gutter >= 3) ? (gutter - 3) : 0;
                for (size_t p = 0; p < digits; p++) {
                    if (pos < max_out) out_buf[pos++] = ' ';
                }
                buf_append_str(out_buf, &pos, max_out, " ~ | \x1b[0m");
            } else {
                buf_append_str(out_buf, &pos, max_out, "~");
            }
        }
        buf_append_str(out_buf, &pos, max_out, "\x1b[K\r\n");
    }

    /* Row H-1: Status bar */
    if (s->status_msg[0]) {
        buf_append_str(out_buf, &pos, max_out, s->status_msg);
    }
    buf_append_str(out_buf, &pos, max_out, "\x1b[K\r\n");

    /* Row H: Shortcut Legend */
    buf_append_str(out_buf, &pos, max_out, "\x1b[7m^O Save  ^X Exit  ^W WhereIs  ^R Replace  ^G GotoLine  ^Z Undo  ^Y Redo\x1b[K\x1b[0m");

    /* Position screen cursor */
    size_t rx = nano_col_to_render(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length, s->cx);
    size_t scr_y = 2 + (s->cy - s->row_offset); /* Header is row 1, so top text line is 2 */
    size_t scr_x = 1 + gutter + (rx >= s->col_offset ? rx - s->col_offset : 0);

    buf_append_str(out_buf, &pos, max_out, "\x1b[");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_y);
    buf_append_str(out_buf, &pos, max_out, ";");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_x);
    buf_append_str(out_buf, &pos, max_out, "H\x1b[?25h");

    return pos;
}

size_t nano_render_cursor(const nano_state_t *s, const char *pool, char *out_buf, size_t max_out) {
    if (!s || !out_buf || max_out < 32) return 0;
    size_t pos = 0;
    size_t gutter = nano_gutter_width(s);
    size_t rx = nano_col_to_render(s, pool + s->rows[s->cy].offset, s->rows[s->cy].length, s->cx);
    size_t scr_y = 2 + (s->cy - s->row_offset);
    size_t scr_x = 1 + gutter + (rx >= s->col_offset ? rx - s->col_offset : 0);

    buf_append_str(out_buf, &pos, max_out, "\x1b[");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_y);
    buf_append_str(out_buf, &pos, max_out, ";");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_x);
    buf_append_str(out_buf, &pos, max_out, "H");
    return pos;
}

size_t nano_render_prompt_cursor(const nano_state_t *s, size_t prompt_len, char *out_buf, size_t max_out) {
    if (!s || !out_buf || max_out < 32) return 0;
    size_t pos = 0;
    uint32_t rows = s->screen_rows ? s->screen_rows : 25;
    uint32_t prompt_row = (rows >= 2) ? (rows - 1) : rows;
    uint32_t prompt_col = 1 + (uint32_t)prompt_len;
    if (s->screen_cols && prompt_col > s->screen_cols) prompt_col = s->screen_cols;

    buf_append_str(out_buf, &pos, max_out, "\x1b[");
    buf_append_u32(out_buf, &pos, max_out, prompt_row);
    buf_append_str(out_buf, &pos, max_out, ";");
    buf_append_u32(out_buf, &pos, max_out, prompt_col);
    buf_append_str(out_buf, &pos, max_out, "H\x1b[?25h");
    return pos;
}

void nano_input_reset(nano_input_state_t *inp) {
    if (!inp) return;
    inp->escape_len = 0;
    inp->escape[0] = '\0';
}

nano_key_t nano_parse_input(nano_input_state_t *inp, unsigned char c, char *out_char) {
    if (!inp) return KEY_NONE;

    if (inp->escape_len > 0) {
        if (inp->escape_len >= sizeof(inp->escape) - 1) {
            nano_input_reset(inp);
            return KEY_NONE;
        }

        inp->escape[inp->escape_len++] = (char)c;
        inp->escape[inp->escape_len] = '\0';

        if (inp->escape_len == 2 && (c == '[' || c == 'O')) {
            return KEY_NONE;
        }

        if (inp->escape_len == 2 || (c >= 0x40 && c <= 0x7E)) {
            nano_key_t k = KEY_NONE;
            const char *esc = inp->escape;

            if (esc[1] == '[' || esc[1] == 'O') {
                if (esc[2] == 'A') k = KEY_UP;
                else if (esc[2] == 'B') k = KEY_DOWN;
                else if (esc[2] == 'C') k = KEY_RIGHT;
                else if (esc[2] == 'D') k = KEY_LEFT;
                else if (esc[2] == 'H') k = KEY_HOME;
                else if (esc[2] == 'F') k = KEY_END;
                else if (esc[2] == '1' && esc[3] == '~') k = KEY_HOME;
                else if (esc[2] == '4' && esc[3] == '~') k = KEY_END;
                else if (esc[2] == '7' && esc[3] == '~') k = KEY_HOME;
                else if (esc[2] == '8' && esc[3] == '~') k = KEY_END;
                else if (esc[2] == '3' && esc[3] == '~') k = KEY_DELETE;
                else if (esc[2] == '5' && esc[3] == '~') k = KEY_PAGE_UP;
                else if (esc[2] == '6' && esc[3] == '~') k = KEY_PAGE_DOWN;
                /* Ctrl+Arrows: ESC [ 1 ; 5 C / D or ESC [ 5 C / D */
                else if (esc[2] == '1' && esc[3] == ';' && esc[4] == '5' && esc[5] == 'D') k = KEY_WORD_LEFT;
                else if (esc[2] == '1' && esc[3] == ';' && esc[4] == '5' && esc[5] == 'C') k = KEY_WORD_RIGHT;
                else if (esc[2] == '5' && esc[3] == 'D') k = KEY_WORD_LEFT;
                else if (esc[2] == '5' && esc[3] == 'C') k = KEY_WORD_RIGHT;
            } else if (inp->escape_len == 2) {
                if (esc[1] == 'd' || esc[1] == 'D') k = KEY_ALT_D;
                else if (esc[1] == 'n' || esc[1] == 'N') k = KEY_ALT_N;
                else if (esc[1] == 'u' || esc[1] == 'U') k = KEY_ALT_U;
                else if (esc[1] == 'b' || esc[1] == 'B') k = KEY_WORD_LEFT;
                else if (esc[1] == 'f' || esc[1] == 'F') k = KEY_WORD_RIGHT;
                else if (esc[1] == 'c' || esc[1] == 'C') k = KEY_ALT_C;
                else if (esc[1] == 'r' || esc[1] == 'R') k = KEY_ALT_R;
                else if (esc[1] == ',' || esc[1] == '<') k = KEY_ALT_PREV_BUF;
                else if (esc[1] == '.' || esc[1] == '>') k = KEY_ALT_NEXT_BUF;
            }

            nano_input_reset(inp);
            return k;
        }
        return KEY_NONE;
    }

    if (c == 27) {
        inp->escape[0] = 27;
        inp->escape[1] = '\0';
        inp->escape_len = 1;
        return KEY_NONE;
    }

    switch (c) {
        case '\r':
        case '\n':
            return KEY_ENTER;
        case 8:
        case 127:
            return KEY_BACKSPACE;
        case 1:
            return KEY_CTRL_A;
        case 3:
            return KEY_CTRL_C;
        case 5:
            return KEY_CTRL_E;
        case 7:
        case 31: /* 0x1F Ctrl+_ */
            return KEY_CTRL_G;
        case 11:
            return KEY_CTRL_K;
        case 12:
            return KEY_CTRL_L;
        case 15:
            return KEY_CTRL_O;
        case 18:
            return KEY_CTRL_R;
        case 21:
            return KEY_CTRL_U;
        case 23:
            return KEY_CTRL_W;
        case 24:
            return KEY_CTRL_X;
        case 25:
            return KEY_CTRL_Y;
        case 26:
            return KEY_CTRL_Z;
        default:
            if (c >= 32 && c < 127) {
                if (out_char) *out_char = (char)c;
                return KEY_CHAR;
            }
            if (c == '\t') {
                if (out_char) *out_char = '\t';
                return KEY_CHAR;
            }
            break;
    }

    return KEY_NONE;
}
