#include "console.h"
#include "font.h"
#include "spinlock.h"
#include "string.h"
#include "serial.h"
#include "dmesg.h"

/* Static text cache works before PMM/heap initialization. Covers a 4K text
 * viewport without reading uncached framebuffer memory while scrolling. */
#define CONSOLE_MAX_COLS 512
#define CONSOLE_MAX_ROWS 256

typedef struct {
    uint32_t fg, bg;
    char character;
} console_cell_t;
static console_cell_t cells[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
#ifdef CONSOLE_TEST
static size_t test_glyph_draws, test_scrolls;
#endif

typedef struct {
    volatile uint32_t *fb;
    uint64_t width;
    uint64_t height;
    uint64_t pitch32;
    uint64_t cols;
    uint64_t rows;
    uint64_t cursor_col;
    uint64_t cursor_row;
    uint32_t fg_color;
    uint32_t bg_color;
    bool     initialized;
} fb_console_t;

static fb_console_t g_console = {0};
static spinlock_t   g_console_lock = SPINLOCK_RANKED(5, "console");

bool console_is_initialized(void) {
    return g_console.initialized;
}

static void draw_char_unlocked(uint64_t col, uint64_t row, char c, uint32_t fg, uint32_t bg) {
    if (col >= g_console.cols || row >= g_console.rows) return;

    console_cell_t *cell = &cells[row][col];
    if (cell->character == c && cell->bg == bg && (c == ' ' || cell->fg == fg)) {
        cell->fg = fg;
        return;
    }
    *cell = (console_cell_t){.fg = fg, .bg = bg, .character = c};
#ifdef CONSOLE_TEST
    test_glyph_draws++;
#endif
    uint64_t px = col * FONT_WIDTH;
    uint64_t py = row * FONT_HEIGHT;
    const uint8_t *glyph = font_get_glyph(c);

    for (uint64_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t row_bits = glyph[y];
        volatile uint32_t *line = g_console.fb + (py + y) * g_console.pitch32 + px;
        for (uint64_t x = 0; x < FONT_WIDTH; x++) {
            line[x] = (row_bits & (0x80 >> x)) ? fg : bg;
        }
    }
}

static void scroll_unlocked(void) {
    /* Amortize boot-log scrolling over several new lines. New output remains
     * immediately visible; this does not need a timer or deferred flush. */
    uint64_t count = g_console.rows / 4;
    if (!count) count = 1;
    if (count > 8) count = 8;
#ifdef CONSOLE_TEST
    test_scrolls++;
#endif
    for (uint64_t row = 0; row < g_console.rows - count; row++) {
        for (uint64_t col = 0; col < g_console.cols; col++) {
            console_cell_t source = cells[row + count][col];
            draw_char_unlocked(col, row, source.character, source.fg, source.bg);
        }
    }
    for (uint64_t row = g_console.rows - count; row < g_console.rows; row++) {
        for (uint64_t col = 0; col < g_console.cols; col++)
            draw_char_unlocked(col, row, ' ', g_console.fg_color, g_console.bg_color);
    }
    g_console.cursor_row = g_console.rows - count;
}

static void reset_cells_unlocked(void) {
    for (uint64_t row = 0; row < g_console.rows; row++)
        for (uint64_t col = 0; col < g_console.cols; col++)
            cells[row][col] = (console_cell_t){
                .fg = g_console.fg_color, .bg = g_console.bg_color, .character = ' '
            };
}

static void console_putc_unlocked(char c) {
    if (c == '\r') {
        g_console.cursor_col = 0;
        return;
    }

    if (c == '\n') {
        g_console.cursor_col = 0;
        g_console.cursor_row++;
        if (g_console.cursor_row >= g_console.rows) {
            scroll_unlocked();
        }
        return;
    }

    if (c == '\b') {
        /* Let user-space line editing erase a character across soft wrapping. */
        if (g_console.cursor_col == 0 && g_console.cursor_row > 0) {
            g_console.cursor_row--;
            g_console.cursor_col = g_console.cols;
        }
        if (g_console.cursor_col > 0) {
            g_console.cursor_col--;
            draw_char_unlocked(g_console.cursor_col, g_console.cursor_row, ' ', g_console.fg_color, g_console.bg_color);
        }
        return;
    }

    if (c == '\t') {
        uint64_t next_col = (g_console.cursor_col + 8) & ~7ULL;
        while (g_console.cursor_col < next_col && g_console.cursor_col < g_console.cols) {
            draw_char_unlocked(g_console.cursor_col, g_console.cursor_row, ' ', g_console.fg_color, g_console.bg_color);
            g_console.cursor_col++;
        }
        if (g_console.cursor_col >= g_console.cols) {
            g_console.cursor_col = 0;
            g_console.cursor_row++;
            if (g_console.cursor_row >= g_console.rows) {
                scroll_unlocked();
            }
        }
        return;
    }

    /* Wrap to next row before drawing if already at right edge */
    if (g_console.cursor_col >= g_console.cols) {
        g_console.cursor_col = 0;
        g_console.cursor_row++;
        if (g_console.cursor_row >= g_console.rows) {
            scroll_unlocked();
        }
    }

    /* Draw character glyph */
    draw_char_unlocked(g_console.cursor_col, g_console.cursor_row, c, g_console.fg_color, g_console.bg_color);
    g_console.cursor_col++;

    /* Defer wrapping until the next printable character. A newline following
     * an exactly full line must advance once, not create an extra blank row. */

}

void console_init(const boot_info_t *boot_info) {
    if (!boot_info || !boot_info->has_framebuffer || !boot_info->fb_address) {
        serial_puts("[WARN] Framebuffer console cannot initialize (no framebuffer)\n");
        return;
    }

    if (boot_info->fb_bpp != 32) {
        serial_puts("[WARN] Framebuffer console requires 32 bpp linear framebuffer\n");
        return;
    }
    if (boot_info->fb_width < FONT_WIDTH || boot_info->fb_height < FONT_HEIGHT ||
        boot_info->fb_width > 8192 || boot_info->fb_height > 8192 ||
        boot_info->fb_pitch % 4 || boot_info->fb_pitch < boot_info->fb_width * 4 ||
        boot_info->fb_pitch > (64ULL * 1024 * 1024) / boot_info->fb_height ||
        boot_info->fb_address > UINT64_MAX - boot_info->fb_pitch * boot_info->fb_height) return;

    uint64_t rflags = spin_lock_irqsave(&g_console_lock);

    g_console.fb          = (volatile uint32_t *)boot_info->fb_address;
    g_console.width       = boot_info->fb_width;
    g_console.height      = boot_info->fb_height;
    g_console.pitch32     = boot_info->fb_pitch / 4;
    g_console.cols        = boot_info->fb_width / FONT_WIDTH;
    g_console.rows        = boot_info->fb_height / FONT_HEIGHT;
    if (g_console.cols > CONSOLE_MAX_COLS) g_console.cols = CONSOLE_MAX_COLS;
    if (g_console.rows > CONSOLE_MAX_ROWS) g_console.rows = CONSOLE_MAX_ROWS;
    g_console.cursor_col  = 0;
    g_console.cursor_row  = 0;
    g_console.fg_color    = CONSOLE_DEFAULT_FG;
    g_console.bg_color    = CONSOLE_DEFAULT_BG;
    g_console.initialized = true;

    reset_cells_unlocked();

    /* Fill background */
    for (uint64_t y = 0; y < g_console.height; y++) {
        volatile uint32_t *line = g_console.fb + y * g_console.pitch32;
        for (uint64_t x = 0; x < g_console.width; x++) {
            line[x] = g_console.bg_color;
        }
    }

    spin_unlock_irqrestore(&g_console_lock, rflags);

    serial_puts("[ OK ] Framebuffer text console initialized (Grid: ");
    serial_print_dec(g_console.cols);
    serial_puts("x");
    serial_print_dec(g_console.rows);
    serial_puts(" chars, 8x16 font)\n");
}

void console_clear(void) {
    if (!g_console.initialized) return;

    uint64_t rflags = spin_lock_irqsave(&g_console_lock);

    for (uint64_t y = 0; y < g_console.height; y++) {
        volatile uint32_t *line = g_console.fb + y * g_console.pitch32;
        for (uint64_t x = 0; x < g_console.width; x++) {
            line[x] = g_console.bg_color;
        }
    }
    reset_cells_unlocked();
    g_console.cursor_col = 0;
    g_console.cursor_row = 0;

    spin_unlock_irqrestore(&g_console_lock, rflags);
}

void console_putc(char c) {
    if (!g_console.initialized) return;

    uint64_t rflags = spin_lock_irqsave(&g_console_lock);
    console_putc_unlocked(c);
    spin_unlock_irqrestore(&g_console_lock, rflags);
}

void console_puts(const char *str) {
    if (!g_console.initialized || !str) return;

    uint64_t rflags = spin_lock_irqsave(&g_console_lock);
    while (*str) {
        console_putc_unlocked(*str++);
    }
    spin_unlock_irqrestore(&g_console_lock, rflags);
}

void console_set_color(uint32_t fg, uint32_t bg) {
    uint64_t rflags = spin_lock_irqsave(&g_console_lock);
    g_console.fg_color = fg;
    g_console.bg_color = bg;
    spin_unlock_irqrestore(&g_console_lock, rflags);
}

void console_get_dimensions(uint64_t *out_cols, uint64_t *out_rows) {
    if (out_cols) *out_cols = g_console.cols;
    if (out_rows) *out_rows = g_console.rows;
}

void console_get_cursor(uint64_t *out_col, uint64_t *out_row) {
    if (out_col) *out_col = g_console.cursor_col;
    if (out_row) *out_row = g_console.cursor_row;
}
