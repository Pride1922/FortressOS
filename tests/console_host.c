#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "types.h"

void serial_puts(const char *s) { (void)s; }
void serial_print_dec(uint64_t n) { (void)n; }
#define CONSOLE_TEST
#include "../src/drivers/console.c"

static uint32_t *frame;
static size_t pitch, height, width;
static const uint32_t sentinel = 0xdeadbeef;

static void start(size_t w, size_t h) {
    free(frame);
    width = w; height = h; pitch = w + 7;
    frame = malloc((pitch * height + 32) * sizeof(*frame));
    assert(frame);
    for (size_t i = 0; i < pitch * height + 32; i++) frame[i] = sentinel;
    boot_info_t info = {.has_framebuffer = true, .fb_address = (uintptr_t)frame,
                       .fb_width = w, .fb_height = h, .fb_pitch = pitch * 4, .fb_bpp = 32};
    console_init(&info);
    test_glyph_draws = test_scrolls = 0;
}

static void check_cell(size_t col, size_t row, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_get_glyph(c);
    for (size_t y = 0; y < 16; y++)
        for (size_t x = 0; x < 8; x++)
            assert(frame[(row * 16 + y) * pitch + col * 8 + x] ==
                   ((glyph[y] & (0x80 >> x)) ? fg : bg));
}

static void check_bounds(void) {
    for (size_t y = 0; y < height; y++)
        for (size_t x = width; x < pitch; x++) assert(frame[y * pitch + x] == sentinel);
    for (size_t i = pitch * height; i < pitch * height + 32; i++) assert(frame[i] == sentinel);
}

int main(void) {
    start(131, 163); /* 16 columns, 10 rows; partial pixels and padded pitch. */
    for (unsigned row = 0; row < 10; row++) {
        console_set_color(0x100000U + row, CONSOLE_DEFAULT_BG);
        console_putc('A' + row);
        console_putc('\n');
    }
    assert(test_scrolls == 1 && g_console.cursor_row == 8);
    for (unsigned row = 0; row < 8; row++) {
        check_cell(0, row, 'C' + row, 0x100002U + row, CONSOLE_DEFAULT_BG);
        check_cell(1, row, ' ', 0, CONSOLE_DEFAULT_BG);
    }
    check_cell(0, 8, ' ', 0, CONSOLE_DEFAULT_BG);
    size_t drawn = test_glyph_draws;
    console_putc('\n');
    assert(test_scrolls == 1 && test_glyph_draws == drawn); /* no scroll/redraw on next line */
    console_putc('\n');
    assert(test_scrolls == 2);
    check_cell(0, 0, 'E', 0x100004U, CONSOLE_DEFAULT_BG);
    /* Sparse log draws only occupied cells, not all 160 cells per scroll. */
    assert(test_glyph_draws < 40);
    check_bounds();

    console_clear();
    console_set_color(CONSOLE_DEFAULT_FG, CONSOLE_DEFAULT_BG);
    test_glyph_draws = test_scrolls = 0;
    for (unsigned i = 0; i < 200; i++) console_putc('\n');
    assert(test_scrolls > 0 && test_glyph_draws == 0); /* already blank needs no MMIO writes */
    console_clear();
    console_puts("1234567890123456\nX");
    assert(g_console.cursor_row == 1 && g_console.cursor_col == 1);
    check_cell(0, 1, 'X', CONSOLE_DEFAULT_FG, CONSOLE_DEFAULT_BG);
    console_putc('\b');
    check_cell(0, 1, ' ', 0, CONSOLE_DEFAULT_BG);
    console_putc('\t');
    assert(g_console.cursor_col == 8);
    console_putc('\r');
    console_putc('Y');
    check_cell(0, 1, 'Y', CONSOLE_DEFAULT_FG, CONSOLE_DEFAULT_BG);
    console_set_color(0xffffff, 0x123456);
    console_clear();
    check_cell(0, 0, ' ', 0, 0x123456);
    console_putc('Z');
    check_cell(0, 0, 'Z', 0xffffff, 0x123456);
    check_bounds();

    start(8, 16); /* one-cell console must clear safely on newline */
    console_puts("X\n");
    assert(g_console.cursor_row == 0 && g_console.cursor_col == 0);
    check_cell(0, 0, ' ', 0, CONSOLE_DEFAULT_BG);
    check_bounds();
    free(frame);
    puts("PASS console: cached scrolling, fewer redraws, colours, wrap, controls, pitch and bounds");
}
