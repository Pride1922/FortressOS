#include "console.h"
#include "font.h"
#include "spinlock.h"
#include "string.h"
#include "serial.h"

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
static spinlock_t   g_console_lock = SPINLOCK_INIT;

bool console_is_initialized(void) {
    return g_console.initialized;
}

static void draw_char_unlocked(uint64_t col, uint64_t row, char c, uint32_t fg, uint32_t bg) {
    if (col >= g_console.cols || row >= g_console.rows) return;

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
    uint64_t scroll_rows = (g_console.rows - 1) * FONT_HEIGHT;

    /* Move pixel rows up by FONT_HEIGHT */
    for (uint64_t y = 0; y < scroll_rows; y++) {
        volatile uint32_t *dst = g_console.fb + y * g_console.pitch32;
        volatile uint32_t *src = g_console.fb + (y + FONT_HEIGHT) * g_console.pitch32;
        memcpy((void *)dst, (const void *)src, g_console.width * sizeof(uint32_t));
    }

    /* Clear bottom text line to background color */
    for (uint64_t y = scroll_rows; y < g_console.rows * FONT_HEIGHT; y++) {
        volatile uint32_t *line = g_console.fb + y * g_console.pitch32;
        for (uint64_t x = 0; x < g_console.width; x++) {
            line[x] = g_console.bg_color;
        }
    }

    g_console.cursor_row = g_console.rows - 1;
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

    /* Wrap if character reached edge */
    if (g_console.cursor_col >= g_console.cols) {
        g_console.cursor_col = 0;
        g_console.cursor_row++;
        if (g_console.cursor_row >= g_console.rows) {
            scroll_unlocked();
        }
    }
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

    uint64_t rflags = spin_lock_irqsave(&g_console_lock);

    g_console.fb          = (volatile uint32_t *)boot_info->fb_address;
    g_console.width       = boot_info->fb_width;
    g_console.height      = boot_info->fb_height;
    g_console.pitch32     = boot_info->fb_pitch / 4;
    g_console.cols        = boot_info->fb_width / FONT_WIDTH;
    g_console.rows        = boot_info->fb_height / FONT_HEIGHT;
    g_console.cursor_col  = 0;
    g_console.cursor_row  = 0;
    g_console.fg_color    = CONSOLE_DEFAULT_FG;
    g_console.bg_color    = CONSOLE_DEFAULT_BG;
    g_console.initialized = true;

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
