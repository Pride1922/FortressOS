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

static void *nano_memset(void *dst, int val, size_t n) {
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

#define memcpy nano_memcpy
#define memmove nano_memmove
#define memset nano_memset
#define strlen nano_strlen
#define memcmp nano_memcmp
#endif

static char to_lower_char(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

void nano_init(nano_state_t *s, char *pool) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
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
    s->filename[0] = '\0';
    s->status_msg[0] = '\0';
    s->cut_len = 0;
    s->has_cut = false;
}

uint16_t nano_calc_render_len(const char *chars, size_t len) {
    size_t rx = 0;
    for (size_t i = 0; i < len; i++) {
        if (chars[i] == '\t') {
            rx += NANO_TAB_STOP - (rx % NANO_TAB_STOP);
        } else {
            rx++;
        }
    }
    return (uint16_t)(rx > 65535 ? 65535 : rx);
}

size_t nano_col_to_render(const char *chars, size_t len, size_t col) {
    if (col > len) col = len;
    size_t rx = 0;
    for (size_t i = 0; i < col; i++) {
        if (chars[i] == '\t') {
            rx += NANO_TAB_STOP - (rx % NANO_TAB_STOP);
        } else {
            rx++;
        }
    }
    return rx;
}

bool nano_load_buffer(nano_state_t *s, char *pool, const char *data, size_t size) {
    if (!s || !pool) return false;
    if (size > NANO_POOL_SIZE) return false;

    nano_init(s, pool);

    if (!data || size == 0) {
        return true;
    }

    size_t line_start = 0;
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
            s->rows[s->num_rows].render_len = nano_calc_render_len(pool + s->total_bytes, chunk_len);

            s->total_bytes += chunk_len;
            s->num_rows++;

            chunk_start += chunk_len;
            remaining -= chunk_len;
        } while (remaining > 0);

        /* Advance past \r\n or \n or \r */
        if (next_nl < size) {
            if (data[next_nl] == '\r' && next_nl + 1 < size && data[next_nl + 1] == '\n') {
                line_start = next_nl + 2;
            } else {
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

    s->rows[s->cy].render_len = nano_calc_render_len(pool + s->rows[s->cy].offset, s->rows[s->cy].length);
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
    s->rows[s->cy + 1].render_len = nano_calc_render_len(pool + s->rows[s->cy + 1].offset, len2);

    s->rows[s->cy].length = len1;
    s->rows[s->cy].render_len = nano_calc_render_len(pool + s->rows[s->cy].offset, len1);

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

        s->rows[s->cy].render_len = nano_calc_render_len(pool + s->rows[s->cy].offset, s->rows[s->cy].length);
        s->modified = true;
        return true;
    } else if (s->cx == s->rows[s->cy].length && s->cy + 1 < s->num_rows) {
        /* Merge line cy + 1 into cy */
        if (s->rows[s->cy].length + s->rows[s->cy + 1].length > NANO_MAX_LINE_LEN) {
            return false;
        }

        s->rows[s->cy].length += s->rows[s->cy + 1].length;
        s->rows[s->cy].render_len = nano_calc_render_len(pool + s->rows[s->cy].offset, s->rows[s->cy].length);

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
        s->rows[s->cy - 1].render_len = nano_calc_render_len(pool + s->rows[s->cy - 1].offset, s->rows[s->cy - 1].length);

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
    s->rows[s->cy].render_len = nano_calc_render_len(s->cut_buffer, s->cut_len);
    s->num_rows++;

    for (size_t i = s->cy + 1; i < s->num_rows; i++) {
        s->rows[i].offset += s->cut_len;
    }

    s->cx = 0;
    s->modified = true;
    return true;
}

static bool match_query_at(const char *text, size_t text_len, size_t pos, const char *query, size_t qlen) {
    if (pos + qlen > text_len) return false;
    for (size_t i = 0; i < qlen; i++) {
        if (to_lower_char(text[pos + i]) != to_lower_char(query[i])) {
            return false;
        }
    }
    return true;
}

bool nano_search(nano_state_t *s, const char *pool, const char *query) {
    if (!s || !pool || !query || query[0] == '\0') return false;
    size_t qlen = strlen(query);

    /* Search forward starting from cursor + 1 */
    size_t start_col = s->cx + 1;
    for (size_t r = s->cy; r < s->num_rows; r++) {
        const char *line = pool + s->rows[r].offset;
        size_t len = s->rows[r].length;
        for (size_t c = (r == s->cy ? start_col : 0); c + qlen <= len; c++) {
            if (match_query_at(line, len, c, query, qlen)) {
                s->cy = r;
                s->cx = c;
                size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
                s->row_offset = (s->cy > page / 2) ? (s->cy - page / 2) : 0;
                return true;
            }
        }
    }

    /* Wrap around from top */
    for (size_t r = 0; r <= s->cy; r++) {
        const char *line = pool + s->rows[r].offset;
        size_t len = s->rows[r].length;
        size_t max_col = (r == s->cy ? s->cx : len);
        for (size_t c = 0; c + qlen <= len && (r < s->cy || c <= max_col); c++) {
            if (match_query_at(line, len, c, query, qlen)) {
                s->cy = r;
                s->cx = c;
                size_t page = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
                s->row_offset = (s->cy > page / 2) ? (s->cy - page / 2) : 0;
                return true;
            }
        }
    }

    return false;
}

void nano_update_viewport(nano_state_t *s, const char *pool) {
    if (!s) return;
    size_t text_rows = (s->screen_rows >= 4) ? (s->screen_rows - 3) : 1;
    size_t text_cols = s->screen_cols ? s->screen_cols : 80;

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
    size_t rx = nano_col_to_render(pool + s->rows[s->cy].offset, s->rows[s->cy].length, s->cx);
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

    nano_update_viewport(s, pool);

    /* Hide cursor, move home */
    buf_append_str(out_buf, &pos, max_out, "\x1b[?25l\x1b[H");

    /* Row 1: Header (Inverted video) */
    buf_append_str(out_buf, &pos, max_out, "\x1b[7m [ FortressOS Nano 1.0 ]   File: ");
    const char *fn = s->filename[0] ? s->filename : "[New Buffer]";
    buf_append_str(out_buf, &pos, max_out, fn);
    if (s->modified) {
        buf_append_str(out_buf, &pos, max_out, " [Modified]");
    }
    /* Clear line while inverted, then reset attributes */
    buf_append_str(out_buf, &pos, max_out, "\x1b[K\x1b[0m\r\n");

    /* Rows 2 .. H-2: Text viewport */
    for (uint32_t i = 0; i < text_rows; i++) {
        size_t line_idx = s->row_offset + i;
        buf_append_str(out_buf, &pos, max_out, "\x1b[K");

        if (line_idx < s->num_rows) {
            const char *chars = pool + s->rows[line_idx].offset;
            size_t len = s->rows[line_idx].length;

            /* Expand tabs and crop to [col_offset, col_offset + cols) */
            size_t rx = 0;
            for (size_t c = 0; c < len; c++) {
                if (chars[c] == '\t') {
                    size_t tab_width = NANO_TAB_STOP - (rx % NANO_TAB_STOP);
                    for (size_t t = 0; t < tab_width; t++) {
                        if (rx >= s->col_offset && rx < s->col_offset + cols) {
                            if (pos < max_out) out_buf[pos++] = ' ';
                        }
                        rx++;
                    }
                } else {
                    if (rx >= s->col_offset && rx < s->col_offset + cols) {
                        if (pos < max_out) out_buf[pos++] = chars[c];
                    }
                    rx++;
                }
            }
        } else {
            buf_append_str(out_buf, &pos, max_out, "~");
        }
        buf_append_str(out_buf, &pos, max_out, "\r\n");
    }

    /* Row H-1: Status bar */
    buf_append_str(out_buf, &pos, max_out, "\x1b[K");
    if (s->status_msg[0]) {
        buf_append_str(out_buf, &pos, max_out, s->status_msg);
    }
    buf_append_str(out_buf, &pos, max_out, "\r\n");

    /* Row H: Shortcut Legend */
    buf_append_str(out_buf, &pos, max_out, "\x1b[7m^O WriteOut   ^X Exit   ^W WhereIs   ^K Cut   ^U Uncut   ^L Redraw\x1b[K\x1b[0m");

    /* Position screen cursor */
    size_t rx = nano_col_to_render(pool + s->rows[s->cy].offset, s->rows[s->cy].length, s->cx);
    size_t scr_y = 2 + (s->cy - s->row_offset); /* Header is row 1, so top text line is 2 */
    size_t scr_x = 1 + (rx >= s->col_offset ? rx - s->col_offset : 0);

    buf_append_str(out_buf, &pos, max_out, "\x1b[");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_y);
    buf_append_str(out_buf, &pos, max_out, ";");
    buf_append_u32(out_buf, &pos, max_out, (uint32_t)scr_x);
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
        case 11:
            return KEY_CTRL_K;
        case 12:
            return KEY_CTRL_L;
        case 15:
            return KEY_CTRL_O;
        case 21:
            return KEY_CTRL_U;
        case 23:
            return KEY_CTRL_W;
        case 24:
            return KEY_CTRL_X;
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
