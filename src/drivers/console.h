#ifndef FORTRESS_CONSOLE_H
#define FORTRESS_CONSOLE_H

#include "types.h"
#include "boot_info.h"

/* Default Tokyo Night / Dark Slate Theme Colors (32bpp 0x00RRGGBB) */
#define CONSOLE_DEFAULT_FG 0x00C0CAF5 /* Ice Blue / Silver */
#define CONSOLE_DEFAULT_BG 0x001A1B26 /* Dark Slate Navy */

/* Public Framebuffer Console API */
void console_init(const boot_info_t *boot_info);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *str);
void console_terminal_write(const char *data, size_t count);
uint64_t console_generation(void);
void console_set_color(uint32_t fg, uint32_t bg);
void console_get_dimensions(uint64_t *out_cols, uint64_t *out_rows);
void console_get_cursor(uint64_t *out_col, uint64_t *out_row);
bool console_is_initialized(void);

#endif /* FORTRESS_CONSOLE_H */
