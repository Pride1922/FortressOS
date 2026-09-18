#include "logo.h"
#include "font.h"
#include "console.h"

/* Theme Palette (Tokyo Night / Cyber-Security Aesthetic) */
#define COLOR_BG          0x001A1B26  /* Dark Slate Background */
#define COLOR_CYAN_GLOW   0x007DCFFF  /* Bright Cyan Accent */
#define COLOR_CYAN_MID    0x002AC3DE  /* Mid Cyan */
#define COLOR_BLUE_ACCENT 0x007AA2F7  /* Bright Electric Blue */
#define COLOR_BLUE_DARK   0x003D59A1  /* Deep Slate Blue */
#define COLOR_DOOR_SHADOW 0x000F1017  /* Deep Shadow Void */
#define COLOR_TEXT_ICE    0x00C0CAF5  /* Ice White / Light Blue */
#define COLOR_SUBTEXT     0x00565F89  /* Muted Slate Gray */

static inline void put_pixel(volatile uint32_t *fb, uint64_t pitch32,
                             uint64_t width, uint64_t height,
                             int64_t x, int64_t y, uint32_t color) {
    if (x >= 0 && (uint64_t)x < width && y >= 0 && (uint64_t)y < height) {
        fb[y * pitch32 + x] = color;
    }
}

static void fill_rect(volatile uint32_t *fb, uint64_t pitch32,
                      uint64_t width, uint64_t height,
                      int64_t rx, int64_t ry, int64_t rw, int64_t rh, uint32_t color) {
    for (int64_t y = ry; y < ry + rh; y++) {
        if (y < 0 || (uint64_t)y >= height) continue;
        for (int64_t x = rx; x < rx + rw; x++) {
            if (x < 0 || (uint64_t)x >= width) continue;
            fb[y * pitch32 + x] = color;
        }
    }
}

static void draw_scaled_char(volatile uint32_t *fb, uint64_t pitch32,
                             uint64_t width, uint64_t height,
                             int64_t start_x, int64_t start_y, char c,
                             uint32_t color, int scale) {
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                fill_rect(fb, pitch32, width, height,
                          start_x + col * scale, start_y + row * scale,
                          scale, scale, color);
            }
        }
    }
}

static void draw_scaled_string(volatile uint32_t *fb, uint64_t pitch32,
                              uint64_t width, uint64_t height,
                              int64_t center_x, int64_t y,
                              const char *str, uint32_t color, int scale, int spacing) {
    int len = 0;
    for (const char *p = str; *p; p++) len++;
    int total_width = len * (FONT_WIDTH * scale + spacing) - spacing;
    int64_t cur_x = center_x - total_width / 2;

    for (const char *p = str; *p; p++) {
        draw_scaled_char(fb, pitch32, width, height, cur_x, y, *p, color, scale);
        cur_x += FONT_WIDTH * scale + spacing;
    }
}

/* Draws a high-tech fortress emblem (shield + crenellated tower + arched portal) */
static void draw_fortress_emblem(volatile uint32_t *fb, uint64_t pitch32,
                                uint64_t width, uint64_t height,
                                int64_t cx, int64_t cy) {
    /* Dimensions */
    int shield_w = 110;
    int shield_h = 130;
    int top_y = cy - shield_h / 2;
    int bot_y = cy + shield_h / 2;
    int half_w = shield_w / 2;

    /* 1. Shield Background Fill with Subtle Dark Tone */
    for (int y = top_y; y <= bot_y; y++) {
        int w_at_y;
        int dy = y - top_y;
        if (dy < 70) {
            /* Upper shield curved outwards */
            w_at_y = half_w - (dy * dy) / 400;
        } else {
            /* Lower shield tapering to point */
            int rem = bot_y - y;
            w_at_y = (rem * half_w) / 60;
        }
        if (w_at_y > 0) {
            for (int x = cx - w_at_y; x <= cx + w_at_y; x++) {
                put_pixel(fb, pitch32, width, height, x, y, 0x0013141F);
            }
        }
    }

    /* 2. Shield Outer Glowing Border */
    for (int y = top_y; y <= bot_y; y++) {
        int w_at_y;
        int dy = y - top_y;
        if (dy < 70) {
            w_at_y = half_w - (dy * dy) / 400;
        } else {
            int rem = bot_y - y;
            w_at_y = (rem * half_w) / 60;
        }
        if (w_at_y > 0) {
            /* Double-line cyan glow */
            put_pixel(fb, pitch32, width, height, cx - w_at_y, y, COLOR_CYAN_GLOW);
            put_pixel(fb, pitch32, width, height, cx + w_at_y, y, COLOR_CYAN_GLOW);
            put_pixel(fb, pitch32, width, height, cx - w_at_y + 1, y, COLOR_BLUE_ACCENT);
            put_pixel(fb, pitch32, width, height, cx + w_at_y - 1, y, COLOR_BLUE_ACCENT);
        }
    }
    /* Shield top edge */
    for (int x = cx - half_w; x <= cx + half_w; x++) {
        put_pixel(fb, pitch32, width, height, x, top_y, COLOR_CYAN_GLOW);
        put_pixel(fb, pitch32, width, height, x, top_y + 1, COLOR_BLUE_ACCENT);
    }

    /* 3. Fortress Tower (Crenellations & Body) */
    int tower_top = top_y + 24;
    int tower_bot = bot_y - 20;

    /* Battlements (3 Crenellations) */
    int cren_h = 14;
    /* Left */
    fill_rect(fb, pitch32, width, height, cx - 22, tower_top, 10, cren_h, COLOR_CYAN_GLOW);
    /* Middle */
    fill_rect(fb, pitch32, width, height, cx - 5,  tower_top - 3, 10, cren_h + 3, COLOR_CYAN_GLOW);
    /* Right */
    fill_rect(fb, pitch32, width, height, cx + 12, tower_top, 10, cren_h, COLOR_CYAN_GLOW);

    /* Tower Cornice */
    fill_rect(fb, pitch32, width, height, cx - 24, tower_top + cren_h - 2, 48, 6, COLOR_BLUE_ACCENT);

    /* Tower Main Body */
    for (int y = tower_top + cren_h + 4; y <= tower_bot; y++) {
        int progress = y - (tower_top + cren_h + 4);
        int tw = 18 + progress / 6; /* Slightly flared base */
        for (int x = cx - tw; x <= cx + tw; x++) {
            uint32_t tcol = (x == cx - tw || x == cx + tw) ? COLOR_CYAN_MID : COLOR_BLUE_DARK;
            put_pixel(fb, pitch32, width, height, x, y, tcol);
        }
    }

    /* Slit Window (Arrow loop) */
    int win_y = tower_top + 34;
    fill_rect(fb, pitch32, width, height, cx - 2, win_y, 4, 12, COLOR_CYAN_GLOW);

    /* Arched Fortress Portal / Gateway */
    int gate_w = 14;
    int gate_h = 24;
    int gate_top = tower_bot - gate_h;
    for (int y = gate_top; y <= tower_bot; y++) {
        int gw = gate_w / 2;
        if (y < gate_top + 6) {
            int round_off = 6 - (y - gate_top);
            gw -= (round_off * round_off) / 8;
            if (gw < 2) gw = 2;
        }
        for (int x = cx - gw; x <= cx + gw; x++) {
            put_pixel(fb, pitch32, width, height, x, y, COLOR_DOOR_SHADOW);
        }
        /* Portal arch outline */
        put_pixel(fb, pitch32, width, height, cx - gw, y, COLOR_CYAN_GLOW);
        put_pixel(fb, pitch32, width, height, cx + gw, y, COLOR_CYAN_GLOW);
    }
}

void logo_render_boot(const boot_info_t *boot_info) {
    if (!boot_info || !boot_info->has_framebuffer || !boot_info->fb_address) return;
    if (boot_info->fb_bpp != 32 || boot_info->fb_width == 0 || boot_info->fb_height == 0) return;

    volatile uint32_t *fb = (volatile uint32_t *)boot_info->fb_address;
    uint64_t width = boot_info->fb_width;
    uint64_t height = boot_info->fb_height;
    uint64_t pitch32 = boot_info->fb_pitch / 4;

    /* 1. Clear background to dark slate navy */
    for (uint64_t y = 0; y < height; y++) {
        for (uint64_t x = 0; x < width; x++) {
            fb[y * pitch32 + x] = COLOR_BG;
        }
    }

    /* 2. Determine layout coordinates */
    int64_t center_x = width / 2;
    int64_t emblem_y = (height > 600) ? (height / 3) : (height / 2 - 40);

    /* 3. Render Fortress Shield & Tower Emblem */
    draw_fortress_emblem(fb, pitch32, width, height, center_x, emblem_y);

    /* 4. Render Typography: "FORTRESS OS" */
    int64_t title_y = emblem_y + 85;
    draw_scaled_string(fb, pitch32, width, height, center_x, title_y,
                       "FORTRESS OS", COLOR_TEXT_ICE, 2, 4);

    /* 5. Render Subtitle: "PREEMPTIVE x86_64 KERNEL" */
    int64_t sub_y = title_y + 42;
    draw_scaled_string(fb, pitch32, width, height, center_x, sub_y,
                       "PREEMPTIVE x86_64 SECURE MICROKERNEL", COLOR_SUBTEXT, 1, 2);
}
