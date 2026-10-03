#ifndef FORTRESS_NANO_H
#define FORTRESS_NANO_H

#ifdef NANO_HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#else
#include "types.h"
#endif

#define NANO_POOL_SIZE     262144  /* 256 KiB text pool */
#define NANO_MAX_ROWS      4096    /* Max line count */
#define NANO_MAX_LINE_LEN  1024    /* Max line length in characters */
#define NANO_MAX_PATH      256     /* Max path length */
#define NANO_MAX_QUERY     64      /* Max search query length */
#define NANO_MAX_CLIPBOARD 1024    /* Max cut/paste buffer length */
#define NANO_TAB_STOP      8       /* Tab expansion columns */

typedef struct {
    uint32_t offset;     /* Offset of line start within s_text_pool */
    uint16_t length;     /* Byte length of this line (excluding newline) */
    uint16_t render_len; /* Visual width on screen (with expanded tabs) */
} nano_row_t;

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_CTRL_A,
    KEY_CTRL_E,
    KEY_CTRL_K,
    KEY_CTRL_L,
    KEY_CTRL_O,
    KEY_CTRL_U,
    KEY_CTRL_W,
    KEY_CTRL_X,
    KEY_CTRL_C
} nano_key_t;

typedef struct {
    char     escape[16];
    uint8_t  escape_len;
} nano_input_state_t;

typedef struct {
    nano_row_t rows[NANO_MAX_ROWS];
    size_t     num_rows;
    size_t     total_bytes;

    /* Cursor in file coordinates (0-indexed) */
    size_t     cx;       /* Character column in current row */
    size_t     cy;       /* Row index */

    /* Viewport scrolling offsets */
    size_t     row_offset; /* Top visible line */
    size_t     col_offset; /* Left visible visual column */

    /* Screen dimensions */
    uint32_t   screen_rows;
    uint32_t   screen_cols;

    /* State flags and file metadata */
    bool       modified;
    char       filename[NANO_MAX_PATH];
    char       status_msg[80];

    /* Line clipboard (Ctrl+K / Ctrl+U) */
    char       cut_buffer[NANO_MAX_CLIPBOARD];
    uint16_t   cut_len;
    bool       has_cut;
} nano_state_t;

/* Core buffer and editing API */
void nano_init(nano_state_t *s, char *pool);
bool nano_load_buffer(nano_state_t *s, char *pool, const char *data, size_t size);
uint16_t nano_calc_render_len(const char *chars, size_t len);
size_t nano_col_to_render(const char *chars, size_t len, size_t col);

bool nano_insert_char(nano_state_t *s, char *pool, char c);
bool nano_split_row(nano_state_t *s, char *pool);
bool nano_delete_char(nano_state_t *s, char *pool);
bool nano_backspace(nano_state_t *s, char *pool);

void nano_move_left(nano_state_t *s, const char *pool);
void nano_move_right(nano_state_t *s, const char *pool);
void nano_move_up(nano_state_t *s, const char *pool);
void nano_move_down(nano_state_t *s, const char *pool);
void nano_move_home(nano_state_t *s);
void nano_move_end(nano_state_t *s);
void nano_page_up(nano_state_t *s, const char *pool);
void nano_page_down(nano_state_t *s, const char *pool);

bool nano_cut_line(nano_state_t *s, char *pool);
bool nano_uncut_line(nano_state_t *s, char *pool);

bool nano_search(nano_state_t *s, const char *pool, const char *query);

/* Viewport update and frame rendering */
void nano_update_viewport(nano_state_t *s, const char *pool);
size_t nano_render_frame(nano_state_t *s, const char *pool, char *out_buf, size_t max_out);

/* Byte-by-byte escape sequence parser */
nano_key_t nano_parse_input(nano_input_state_t *inp, unsigned char c, char *out_char);
void nano_input_reset(nano_input_state_t *inp);

#endif /* FORTRESS_NANO_H */
