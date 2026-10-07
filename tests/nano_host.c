#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define NANO_HOST_TEST
#include "nano.h"

/* Include nano_core.c implementation directly */
#include "../user/nano_core.c"

static char s_test_pool[NANO_POOL_SIZE];
static nano_state_t s_test_state;
static char s_render_buf[65536];

static void test_init_and_empty(void) {
    printf("[TEST] test_init_and_empty...\n");
    nano_init(&s_test_state, s_test_pool);
    assert(s_test_state.num_rows == 1);
    assert(s_test_state.total_bytes == 0);
    assert(s_test_state.rows[0].offset == 0);
    assert(s_test_state.rows[0].length == 0);
    assert(s_test_state.rows[0].render_len == 0);
    assert(s_test_state.cx == 0);
    assert(s_test_state.cy == 0);
    assert(!s_test_state.modified);
}

static void test_load_buffer_and_crlf(void) {
    printf("[TEST] test_load_buffer_and_crlf...\n");
    const char *sample = "line1\r\nline2 is longer\n\nline4\rline5";
    bool ok = nano_load_buffer(&s_test_state, s_test_pool, sample, strlen(sample));
    assert(ok);
    assert(s_test_state.num_rows == 5);

    /* Verify row 0: "line1" */
    assert(s_test_state.rows[0].length == 5);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "line1", 5) == 0);

    /* Verify row 1: "line2 is longer" */
    assert(s_test_state.rows[1].length == 15);
    assert(memcmp(s_test_pool + s_test_state.rows[1].offset, "line2 is longer", 15) == 0);

    /* Verify row 2: "" (empty line) */
    assert(s_test_state.rows[2].length == 0);

    /* Verify row 3: "line4" */
    assert(s_test_state.rows[3].length == 5);
    assert(memcmp(s_test_pool + s_test_state.rows[3].offset, "line4", 5) == 0);

    /* Verify row 4: "line5" */
    assert(s_test_state.rows[4].length == 5);
    assert(memcmp(s_test_pool + s_test_state.rows[4].offset, "line5", 5) == 0);

    assert(!s_test_state.modified);
    assert(!s_test_state.dos_mode);

    /* Test CRLF-dominant sample */
    const char *crlf_sample = "hello\r\nworld\r\n";
    assert(nano_load_buffer(&s_test_state, s_test_pool, crlf_sample, strlen(crlf_sample)));
    assert(s_test_state.dos_mode);
}

static void test_load_exceed_pool(void) {
    printf("[TEST] test_load_exceed_pool...\n");
    char *huge = malloc(NANO_POOL_SIZE + 100);
    assert(huge);
    memset(huge, 'A', NANO_POOL_SIZE + 100);
    bool ok = nano_load_buffer(&s_test_state, s_test_pool, huge, NANO_POOL_SIZE + 100);
    assert(!ok);
    free(huge);
}

static void test_insert_char(void) {
    printf("[TEST] test_insert_char...\n");
    nano_init(&s_test_state, s_test_pool);

    /* Insert 'H', 'e', 'l', 'l', 'o' */
    assert(nano_insert_char(&s_test_state, s_test_pool, 'H'));
    assert(nano_insert_char(&s_test_state, s_test_pool, 'e'));
    assert(nano_insert_char(&s_test_state, s_test_pool, 'l'));
    assert(nano_insert_char(&s_test_state, s_test_pool, 'l'));
    assert(nano_insert_char(&s_test_state, s_test_pool, 'o'));

    assert(s_test_state.total_bytes == 5);
    assert(s_test_state.rows[0].length == 5);
    assert(s_test_state.cx == 5);
    assert(s_test_state.modified);
    assert(memcmp(s_test_pool, "Hello", 5) == 0);

    /* Move cursor to middle (between 'e' and 'l') and insert 'X' */
    s_test_state.cx = 2;
    assert(nano_insert_char(&s_test_state, s_test_pool, 'X'));
    assert(s_test_state.total_bytes == 6);
    assert(s_test_state.rows[0].length == 6);
    assert(s_test_state.cx == 3);
    assert(memcmp(s_test_pool, "HeXllo", 6) == 0);

    /* Move cursor to beginning and insert '!' */
    s_test_state.cx = 0;
    assert(nano_insert_char(&s_test_state, s_test_pool, '!'));
    assert(s_test_state.total_bytes == 7);
    assert(s_test_state.rows[0].length == 7);
    assert(s_test_state.cx == 1);
    assert(memcmp(s_test_pool, "!HeXllo", 7) == 0);
}

static void test_split_row_and_navigation(void) {
    printf("[TEST] test_split_row_and_navigation...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "FirstLineSecondLine";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* Split at "FirstLine" */
    s_test_state.cx = 9;
    s_test_state.cy = 0;
    assert(nano_split_row(&s_test_state, s_test_pool));

    assert(s_test_state.num_rows == 2);
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 0);
    assert(s_test_state.rows[0].length == 9);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "FirstLine", 9) == 0);
    assert(s_test_state.rows[1].length == 10);
    assert(memcmp(s_test_pool + s_test_state.rows[1].offset, "SecondLine", 10) == 0);

    /* Navigation tests */
    nano_move_up(&s_test_state, s_test_pool);
    assert(s_test_state.cy == 0 && s_test_state.cx == 0);

    nano_move_end(&s_test_state);
    assert(s_test_state.cx == 9);

    nano_move_right(&s_test_state, s_test_pool);
    assert(s_test_state.cy == 1 && s_test_state.cx == 0);

    nano_move_left(&s_test_state, s_test_pool);
    assert(s_test_state.cy == 0 && s_test_state.cx == 9);
}

static void test_backspace_and_delete(void) {
    printf("[TEST] test_backspace_and_delete...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "Line1\nLine2";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* Backspace at beginning of Line2 merges with Line1 */
    s_test_state.cy = 1;
    s_test_state.cx = 0;
    assert(nano_backspace(&s_test_state, s_test_pool));

    assert(s_test_state.num_rows == 1);
    assert(s_test_state.cy == 0);
    assert(s_test_state.cx == 5); /* At old end of Line1 */
    assert(s_test_state.rows[0].length == 10);
    assert(memcmp(s_test_pool, "Line1Line2", 10) == 0);

    /* Delete char in middle */
    s_test_state.cx = 5; /* points to 'L' of Line2 */
    assert(nano_delete_char(&s_test_state, s_test_pool));
    assert(s_test_state.rows[0].length == 9);
    assert(memcmp(s_test_pool, "Line1ine2", 9) == 0);

    /* Split again */
    s_test_state.cx = 5;
    assert(nano_split_row(&s_test_state, s_test_pool));
    assert(s_test_state.num_rows == 2);

    /* Delete at end of Line1 merges with next line */
    s_test_state.cy = 0;
    s_test_state.cx = 5;
    assert(nano_delete_char(&s_test_state, s_test_pool));
    assert(s_test_state.num_rows == 1);
    assert(s_test_state.rows[0].length == 9);
    assert(memcmp(s_test_pool, "Line1ine2", 9) == 0);
}

static void test_tab_expansion_and_render_len(void) {
    printf("[TEST] test_tab_expansion_and_render_len...\n");
    const char *str = "a\tb";
    /* 'a' at col 0, '\t' advances to col 8, 'b' at col 8 -> total length 9 */
    uint16_t rlen = nano_calc_render_len(&s_test_state, str, strlen(str));
    assert(rlen == 9);

    size_t col0 = nano_col_to_render(&s_test_state, str, strlen(str), 0);
    assert(col0 == 0);

    size_t col1 = nano_col_to_render(&s_test_state, str, strlen(str), 1);
    assert(col1 == 1); /* before tab */

    size_t col2 = nano_col_to_render(&s_test_state, str, strlen(str), 2);
    assert(col2 == 8); /* after tab */

    size_t col3 = nano_col_to_render(&s_test_state, str, strlen(str), 3);
    assert(col3 == 9); /* after 'b' */
}

static void test_cut_and_uncut(void) {
    printf("[TEST] test_cut_and_uncut...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "First\nMiddle\nLast";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* Cut middle line */
    s_test_state.cy = 1;
    s_test_state.cx = 2;
    assert(nano_cut_line(&s_test_state, s_test_pool));

    assert(s_test_state.num_rows == 2);
    assert(s_test_state.has_cut);
    assert(s_test_state.cut_len == 6);
    assert(memcmp(s_test_state.cut_buffer, "Middle", 6) == 0);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "First", 5) == 0);
    assert(memcmp(s_test_pool + s_test_state.rows[1].offset, "Last", 4) == 0);

    /* Uncut at row 0 (pastes above / at row 0) */
    s_test_state.cy = 0;
    assert(nano_uncut_line(&s_test_state, s_test_pool));
    assert(s_test_state.num_rows == 3);
    assert(s_test_state.rows[0].length == 6);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "Middle", 6) == 0);
    assert(memcmp(s_test_pool + s_test_state.rows[1].offset, "First", 5) == 0);
    assert(memcmp(s_test_pool + s_test_state.rows[2].offset, "Last", 4) == 0);
}

static void test_search(void) {
    printf("[TEST] test_search...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "Apple banana Cherry\nDragonfruit elderberry\nFig GRAPE";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    s_test_state.cx = 0;
    s_test_state.cy = 0;

    /* Case-insensitive search for "elderberry" */
    assert(nano_search(&s_test_state, s_test_pool, "ELDER", NULL));
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 12);

    /* Search wrap-around for "banana" */
    assert(nano_search(&s_test_state, s_test_pool, "BANANA", NULL));
    assert(s_test_state.cy == 0);
    assert(s_test_state.cx == 6);

    /* Non-existent query */
    assert(!nano_search(&s_test_state, s_test_pool, "watermelon", NULL));
}

static void test_escape_parser(void) {
    printf("[TEST] test_escape_parser...\n");
    nano_input_state_t inp;
    nano_input_reset(&inp);
    char out_char = 0;

    /* Printable char */
    assert(nano_parse_input(&inp, 'A', &out_char) == KEY_CHAR && out_char == 'A');
    assert(nano_parse_input(&inp, 'z', &out_char) == KEY_CHAR && out_char == 'z');

    /* Enter & Backspace */
    assert(nano_parse_input(&inp, '\n', &out_char) == KEY_ENTER);
    assert(nano_parse_input(&inp, 127, &out_char) == KEY_BACKSPACE);

    /* Ctrl keys */
    assert(nano_parse_input(&inp, 15, &out_char) == KEY_CTRL_O);
    assert(nano_parse_input(&inp, 24, &out_char) == KEY_CTRL_X);
    assert(nano_parse_input(&inp, 23, &out_char) == KEY_CTRL_W);
    assert(nano_parse_input(&inp, 11, &out_char) == KEY_CTRL_K);
    assert(nano_parse_input(&inp, 21, &out_char) == KEY_CTRL_U);
    assert(nano_parse_input(&inp, 12, &out_char) == KEY_CTRL_L);

    /* Arrow keys */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'A', &out_char) == KEY_UP);

    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'B', &out_char) == KEY_DOWN);

    /* Delete key \x1b[3~ */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '3', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '~', &out_char) == KEY_DELETE);

    /* Home \x1b[H and End \x1b[F */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'H', &out_char) == KEY_HOME);

    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'F', &out_char) == KEY_END);

    /* Alt+D \x1bd */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'd', &out_char) == KEY_ALT_D);
}

static void test_bounds_saturation_and_limits(void) {
    printf("[TEST] test_bounds_saturation_and_limits...\n");
    nano_init(&s_test_state, s_test_pool);

    /* 1. Line length cap: insert 1024 chars */
    for (size_t i = 0; i < NANO_MAX_LINE_LEN; i++) {
        assert(nano_insert_char(&s_test_state, s_test_pool, 'a'));
    }
    assert(s_test_state.rows[0].length == NANO_MAX_LINE_LEN);
    /* 1025th char must be rejected cleanly */
    assert(!nano_insert_char(&s_test_state, s_test_pool, 'b'));

    /* 2. Boundary backspace / delete at (0, 0) on single empty line */
    nano_init(&s_test_state, s_test_pool);
    assert(!nano_backspace(&s_test_state, s_test_pool));
    assert(!nano_delete_char(&s_test_state, s_test_pool));
}

static void test_viewport_scrolling(void) {
    printf("[TEST] test_viewport_scrolling...\n");
    nano_init(&s_test_state, s_test_pool);
    s_test_state.screen_rows = 10; /* text_rows = 7 */
    s_test_state.screen_cols = 20;

    /* Create 20 lines */
    for (int i = 0; i < 20; i++) {
        assert(nano_split_row(&s_test_state, s_test_pool));
    }
    assert(s_test_state.num_rows == 21);
    assert(s_test_state.cy == 20);

    /* Update viewport: row_offset should scroll down */
    nano_update_viewport(&s_test_state, s_test_pool);
    assert(s_test_state.row_offset > 0);
    assert(s_test_state.cy >= s_test_state.row_offset);
    assert(s_test_state.cy < s_test_state.row_offset + 7);

    /* Page up */
    nano_page_up(&s_test_state, s_test_pool);
    assert(s_test_state.cy == 13);
    nano_page_up(&s_test_state, s_test_pool);
    assert(s_test_state.cy == 6);
    nano_update_viewport(&s_test_state, s_test_pool);
    assert(s_test_state.row_offset <= 6);
}

static void test_render_frame(void) {
    printf("[TEST] test_render_frame...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "line one\nline two\nline three";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));
    memcpy(s_test_state.filename, "test.txt", sizeof("test.txt"));
    s_test_state.modified = true;
    memcpy(s_test_state.status_msg, "[ Wrote 3 lines ]", sizeof("[ Wrote 3 lines ]"));
    s_test_state.screen_rows = 10;
    s_test_state.screen_cols = 40;

    size_t bytes = nano_render_frame(&s_test_state, s_test_pool, s_render_buf, sizeof(s_render_buf));
    assert(bytes > 0);
    assert(bytes < sizeof(s_render_buf));
    s_render_buf[bytes] = '\0';

    /* Verify elements in rendered ANSI output */
    assert(strstr(s_render_buf, "test.txt") != NULL);
    assert(strstr(s_render_buf, "[Modified]") != NULL);
    assert(strstr(s_render_buf, "[Unix]") != NULL);
    assert(strstr(s_render_buf, "line one") != NULL);
    assert(strstr(s_render_buf, "line two") != NULL);
    assert(strstr(s_render_buf, "[ Wrote 3 lines ]") != NULL);
    assert(strstr(s_render_buf, "^O Save") != NULL);
    assert(strstr(s_render_buf, "^X Exit") != NULL);
    assert(strstr(s_render_buf, "^R Replace") != NULL);
    assert(strstr(s_render_buf, "^G GotoLine") != NULL);
    assert(strstr(s_render_buf, "^Z Undo") != NULL);
    assert(strstr(s_render_buf, "^Y Redo") != NULL);

    /* Test Read-Only and DOS badges */
    s_test_state.readonly = true;
    s_test_state.dos_mode = true;
    bytes = nano_render_frame(&s_test_state, s_test_pool, s_render_buf, sizeof(s_render_buf));
    assert(bytes > 0);
    s_render_buf[bytes] = '\0';
    assert(strstr(s_render_buf, "[Read-Only]") != NULL);
    assert(strstr(s_render_buf, "[DOS]") != NULL);

    /* Test line number gutter rendering and viewport calculation */
    s_test_state.show_line_numbers = true;
    assert(nano_gutter_width(&s_test_state) == 6); /* "  1 | " -> 3 chars + 3 */
    bytes = nano_render_frame(&s_test_state, s_test_pool, s_render_buf, sizeof(s_render_buf));
    assert(bytes > 0);
    s_render_buf[bytes] = '\0';
    assert(strstr(s_render_buf, "  1 | ") != NULL);
    assert(strstr(s_render_buf, "  2 | ") != NULL);

    /* Test fast cursor rendering with gutter */
    size_t cbytes = nano_render_cursor(&s_test_state, s_test_pool, s_render_buf, sizeof(s_render_buf));
    assert(cbytes > 0 && cbytes < 32);
    s_render_buf[cbytes] = '\0';
    assert(strstr(s_render_buf, "\x1b[2;7H") != NULL); /* col = 1 + 6 = 7 */

    /* Test prompt cursor rendering */
    size_t pbytes = nano_render_prompt_cursor(&s_test_state, 12, s_render_buf, sizeof(s_render_buf));
    assert(pbytes > 0 && pbytes < 32);
    s_render_buf[pbytes] = '\0';
    assert(strstr(s_render_buf, "\x1b[9;13H\x1b[?25h") != NULL);
}

static void test_phase2_word_motion_and_goto(void) {
    printf("[TEST] test_phase2_word_motion_and_goto...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "hello world_123   foo.bar\nsecond line\nthird line";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* Test word right */
    s_test_state.cx = 0;
    nano_word_right(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 6); /* "world_123" start */
    nano_word_right(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 18); /* "foo" start */
    nano_word_right(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 21); /* "." */
    nano_word_right(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 22); /* "bar" start */

    /* Test word left */
    nano_word_left(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 21); /* "." */
    nano_word_left(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 18); /* "foo" */
    nano_word_left(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 6); /* "world_123" */
    nano_word_left(&s_test_state, s_test_pool);
    assert(s_test_state.cx == 0); /* "hello" */

    /* Test Go To Line */
    nano_go_to_line(&s_test_state, 2, 5);
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 4);

    nano_go_to_line(&s_test_state, 999, 999);
    assert(s_test_state.cy == 2);
    assert(s_test_state.cx == s_test_state.rows[2].length);
}

static void test_phase2_search_and_replace(void) {
    printf("[TEST] test_phase2_search_and_replace...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "apple banana apple cherry";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* Search for "apple" */
    s_test_state.cx = 0;
    assert(nano_search(&s_test_state, s_test_pool, "banana", NULL));
    assert(s_test_state.cx == 6);

    /* Replace current match "banana" with "orange" */
    assert(nano_replace_current_match(&s_test_state, s_test_pool, "banana", "orange"));
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "apple orange apple cherry", 25) == 0);
    assert(s_test_state.rows[0].length == 25);

    /* Replace "cherry" with "grapefruit" */
    assert(nano_search(&s_test_state, s_test_pool, "cherry", NULL));
    assert(nano_replace_current_match(&s_test_state, s_test_pool, "cherry", "grapefruit"));
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "apple orange apple grapefruit", 29) == 0);
}

static void test_phase2_undo_redo(void) {
    printf("[TEST] test_phase2_undo_redo...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "hello world";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    /* 1. Insert characters with undo push */
    s_test_state.cx = 11;
    char c = '!';
    nano_undo_push(&s_test_state, UNDO_OP_INSERT, s_test_state.cy, s_test_state.cx, &c, 1);
    nano_insert_char(&s_test_state, s_test_pool, c);
    assert(s_test_state.rows[0].length == 12);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "hello world!", 12) == 0);

    /* Undo insertion */
    assert(nano_undo(&s_test_state, s_test_pool));
    assert(s_test_state.rows[0].length == 11);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "hello world", 11) == 0);

    /* Redo insertion */
    assert(nano_redo(&s_test_state, s_test_pool));
    assert(s_test_state.rows[0].length == 12);
    assert(memcmp(s_test_pool + s_test_state.rows[0].offset, "hello world!", 12) == 0);

    /* 2. Test split row undo */
    nano_undo_push(&s_test_state, UNDO_OP_SPLIT, s_test_state.cy, 5, NULL, 0);
    s_test_state.cx = 5;
    assert(nano_split_row(&s_test_state, s_test_pool));
    assert(s_test_state.num_rows == 2);
    assert(s_test_state.rows[0].length == 5);
    assert(s_test_state.rows[1].length == 7);

    /* Undo split */
    assert(nano_undo(&s_test_state, s_test_pool));
    assert(s_test_state.num_rows == 1);
    assert(s_test_state.rows[0].length == 12);
}

static void test_phase2_input_parsing(void) {
    printf("[TEST] test_phase2_input_parsing...\n");
    nano_input_state_t inp;
    char out_char = 0;
    nano_input_reset(&inp);

    /* Ctrl+G (7) */
    assert(nano_parse_input(&inp, 7, &out_char) == KEY_CTRL_G);
    /* Ctrl+R (18) */
    assert(nano_parse_input(&inp, 18, &out_char) == KEY_CTRL_R);
    /* Ctrl+Z (26) */
    assert(nano_parse_input(&inp, 26, &out_char) == KEY_CTRL_Z);
    /* Ctrl+Y (25) */
    assert(nano_parse_input(&inp, 25, &out_char) == KEY_CTRL_Y);

    /* Alt+N \x1bn */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'n', &out_char) == KEY_ALT_N);

    /* Alt+B \x1bb */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'b', &out_char) == KEY_WORD_LEFT);

    /* Alt+F \x1bf */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'f', &out_char) == KEY_WORD_RIGHT);

    /* Ctrl+Left \x1b[1;5D */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '[', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '1', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, ';', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '5', &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'D', &out_char) == KEY_WORD_LEFT);
}

static void test_phase3_regex_search(void) {
    printf("[TEST] test_phase3_regex_search...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "apple 123 banana\n456 cherry 789\ngrape 999";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    s_test_state.regex_search = true;
    s_test_state.case_sensitive = false;
    s_test_state.cx = 0;
    s_test_state.cy = 0;

    /* 1. Match digits with character class [0-9]* */
    size_t mlen = 0;
    assert(nano_search(&s_test_state, s_test_pool, "[0-9][0-9]*", &mlen));
    assert(s_test_state.cy == 0);
    assert(s_test_state.cx == 6);
    assert(mlen == 3);

    /* 2. Anchor ^ at start of line */
    s_test_state.cx = 0;
    s_test_state.cy = 0;
    assert(nano_search(&s_test_state, s_test_pool, "^[0-9]", &mlen));
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 0);
    assert(mlen == 1);

    /* 3. Wildcard . and quantifier * */
    s_test_state.cx = 0;
    s_test_state.cy = 0;
    assert(nano_search(&s_test_state, s_test_pool, "ch.*y", &mlen));
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 4);
    assert(mlen == 6); /* "cherry" */

    /* 4. Regex replacement */
    assert(nano_replace_current_match(&s_test_state, s_test_pool, "ch.*y", "kiwi"));
    assert(memcmp(s_test_pool + s_test_state.rows[1].offset, "456 kiwi 789", 12) == 0);
}

static void test_phase3_plus_quantifier_verification(void) {
    printf("[TEST] test_phase3_plus_quantifier_verification...\n");
    nano_regex_t re;
    size_t mlen = 0;

    /* Test 1: + matches one occurrence: "abc", pattern "b+", expected "b", length 1 */
    {
        const char *input = "abc";
        assert(nano_regex_compile(&re, "b+", false));
        mlen = 0;
        bool ok = nano_regex_match_at(&re, input, strlen(input), 1, &mlen);
        printf("  Test 1 ('abc', 'b+'): %s (len=%zu)\n", (ok && mlen == 1) ? "PASS" : "FAIL", mlen);
        assert(ok);
        assert(mlen == 1);
    }

    /* Test 2: + matches multiple occurrences: "abbbc", pattern "b+", expected "bbb", length 3 */
    {
        const char *input = "abbbc";
        assert(nano_regex_compile(&re, "b+", false));
        mlen = 0;
        bool ok = nano_regex_match_at(&re, input, strlen(input), 1, &mlen);
        printf("  Test 2 ('abbbc', 'b+'): %s (len=%zu)\n", (ok && mlen == 3) ? "PASS" : "FAIL", mlen);
        assert(ok);
        assert(mlen == 3);
    }

    /* Test 3: + after character class: "a123b", pattern "[0-9]+", expected "123", length 3 */
    {
        const char *input = "a123b";
        assert(nano_regex_compile(&re, "[0-9]+", false));
        mlen = 0;
        bool ok = nano_regex_match_at(&re, input, strlen(input), 1, &mlen);
        printf("  Test 3 ('a123b', '[0-9]+'): %s (len=%zu)\n", (ok && mlen == 3) ? "PASS" : "FAIL", mlen);
        assert(ok);
        assert(mlen == 3);
    }

    /* Test 4: + in replacement (Ctrl+R): "hello world", search "o+", replace "O", expected "hellO wOrld" */
    {
        nano_init(&s_test_state, s_test_pool);
        const char *input = "hello world";
        nano_load_buffer(&s_test_state, s_test_pool, input, strlen(input));
        s_test_state.regex_search = true;
        s_test_state.case_sensitive = true;

        /* Move cx to start */
        s_test_state.cx = 0;
        s_test_state.cy = 0;

        /* Match 1: first 'o' */
        assert(nano_search(&s_test_state, s_test_pool, "o+", NULL));
        assert(s_test_state.cx == 4);
        assert(nano_replace_current_match(&s_test_state, s_test_pool, "o+", "O"));

        /* Match 2: second 'o' */
        assert(nano_search(&s_test_state, s_test_pool, "o+", NULL));
        assert(s_test_state.cx == 7);
        assert(nano_replace_current_match(&s_test_state, s_test_pool, "o+", "O"));

        /* Ensure no more matches */
        assert(!nano_search(&s_test_state, s_test_pool, "o+", NULL));

        const char *res = s_test_pool + s_test_state.rows[0].offset;
        size_t res_len = s_test_state.rows[0].length;
        bool ok = (res_len == 11 && memcmp(res, "hellO wOrld", 11) == 0);
        printf("  Test 4 (Replace 'o+' -> 'O'): %s (res='%.*s')\n", ok ? "PASS" : "FAIL", (int)res_len, res);
        assert(ok);
    }

    /* Test 5: + with anchors: "aaab", pattern "^a+", expected "aaa", length 3 */
    {
        const char *input = "aaab";
        assert(nano_regex_compile(&re, "^a+", false));
        mlen = 0;
        bool ok = nano_regex_match_at(&re, input, strlen(input), 0, &mlen);
        printf("  Test 5 ('aaab', '^a+'): %s (len=%zu)\n", (ok && mlen == 3) ? "PASS" : "FAIL", mlen);
        assert(ok);
        assert(mlen == 3);
    }

    /* Test 6: Boundary case - + on empty match: "xyz", pattern "b+", expected no match */
    {
        const char *input = "xyz";
        assert(nano_regex_compile(&re, "b+", false));
        mlen = 0;
        bool ok = false;
        for (size_t p = 0; p <= strlen(input); p++) {
            if (nano_regex_match_at(&re, input, strlen(input), p, &mlen)) {
                ok = true;
                break;
            }
        }
        printf("  Test 6 ('xyz', 'b+'): %s (matched=%s)\n", (!ok) ? "PASS" : "FAIL", ok ? "true" : "false");
        assert(!ok);
    }

    /* Test 7: Interaction with *: "abbbbc", pattern "ab+c", expected match "abbbbc" */
    {
        const char *input = "abbbbc";
        assert(nano_regex_compile(&re, "ab+c", false));
        mlen = 0;
        bool ok = nano_regex_match_at(&re, input, strlen(input), 0, &mlen);
        printf("  Test 7 ('abbbbc', 'ab+c'): %s (len=%zu)\n", (ok && mlen == 6) ? "PASS" : "FAIL", mlen);
        assert(ok);
        assert(mlen == 6);
    }
}

static void test_phase3_case_sensitivity(void) {
    printf("[TEST] test_phase3_case_sensitivity...\n");
    nano_init(&s_test_state, s_test_pool);
    const char *text = "apple Target TARGET target";
    nano_load_buffer(&s_test_state, s_test_pool, text, strlen(text));

    s_test_state.regex_search = false;

    /* Case insensitive search from start */
    s_test_state.case_sensitive = false;
    s_test_state.cx = 0;
    s_test_state.cy = 0;
    assert(nano_search(&s_test_state, s_test_pool, "TARGET", NULL));
    assert(s_test_state.cx == 6);

    /* Case sensitive search for TARGET (uppercase) */
    s_test_state.case_sensitive = true;
    s_test_state.cx = 0;
    s_test_state.cy = 0;
    assert(nano_search(&s_test_state, s_test_pool, "TARGET", NULL));
    assert(s_test_state.cx == 13);

    /* Case sensitive search for target (lowercase) */
    assert(nano_search(&s_test_state, s_test_pool, "target", NULL));
    assert(s_test_state.cx == 20);
}

static void test_phase3_tab_options(void) {
    printf("[TEST] test_phase3_tab_options...\n");
    nano_init(&s_test_state, s_test_pool);

    /* Custom tab_size 4 */
    s_test_state.tab_size = 4;
    const char *str = "a\tb";
    /* 'a' at col 0, '\t' advances to col 4, 'b' at col 4 -> total length 5 */
    uint16_t rlen = nano_calc_render_len(&s_test_state, str, strlen(str));
    assert(rlen == 5);

    size_t col1 = nano_col_to_render(&s_test_state, str, strlen(str), 1);
    assert(col1 == 1);
    size_t col2 = nano_col_to_render(&s_test_state, str, strlen(str), 2);
    assert(col2 == 4);

    /* Custom tab_size 8 */
    s_test_state.tab_size = 8;
    rlen = nano_calc_render_len(&s_test_state, str, strlen(str));
    assert(rlen == 9);
}

static void test_phase3_input_parsing(void) {
    printf("[TEST] test_phase3_input_parsing...\n");
    nano_input_state_t inp;
    char out_char = 0;
    nano_input_reset(&inp);

    /* Alt+C \x1bc */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'c', &out_char) == KEY_ALT_C);

    /* Alt+R \x1br */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, 'r', &out_char) == KEY_ALT_R);

    /* Alt+, \x1b, */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, ',', &out_char) == KEY_ALT_PREV_BUF);

    /* Alt+. \x1b. */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '.', &out_char) == KEY_ALT_NEXT_BUF);

    /* Alt+< and Alt+> */
    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '<', &out_char) == KEY_ALT_PREV_BUF);

    assert(nano_parse_input(&inp, 27, &out_char) == KEY_NONE);
    assert(nano_parse_input(&inp, '>', &out_char) == KEY_ALT_NEXT_BUF);
}

int main(void) {
    printf("=== Starting FortressOS Nano Host Tests ===\n");
    test_init_and_empty();
    test_load_buffer_and_crlf();
    test_load_exceed_pool();
    test_insert_char();
    test_split_row_and_navigation();
    test_backspace_and_delete();
    test_tab_expansion_and_render_len();
    test_cut_and_uncut();
    test_search();
    test_escape_parser();
    test_bounds_saturation_and_limits();
    test_viewport_scrolling();
    test_render_frame();
    test_phase2_word_motion_and_goto();
    test_phase2_search_and_replace();
    test_phase2_undo_redo();
    test_phase2_input_parsing();
    test_phase3_regex_search();
    test_phase3_plus_quantifier_verification();
    test_phase3_case_sensitivity();
    test_phase3_tab_options();
    test_phase3_input_parsing();
    printf("=== ALL NANO HOST TESTS PASSED (100%%) ===\n");
    return 0;
}

