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
    uint16_t rlen = nano_calc_render_len(str, strlen(str));
    assert(rlen == 9);

    size_t col0 = nano_col_to_render(str, strlen(str), 0);
    assert(col0 == 0);

    size_t col1 = nano_col_to_render(str, strlen(str), 1);
    assert(col1 == 1); /* before tab */

    size_t col2 = nano_col_to_render(str, strlen(str), 2);
    assert(col2 == 8); /* after tab */

    size_t col3 = nano_col_to_render(str, strlen(str), 3);
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
    assert(nano_search(&s_test_state, s_test_pool, "ELDER"));
    assert(s_test_state.cy == 1);
    assert(s_test_state.cx == 12);

    /* Search wrap-around for "banana" */
    assert(nano_search(&s_test_state, s_test_pool, "BANANA"));
    assert(s_test_state.cy == 0);
    assert(s_test_state.cx == 6);

    /* Non-existent query */
    assert(!nano_search(&s_test_state, s_test_pool, "watermelon"));
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
    assert(strstr(s_render_buf, "line one") != NULL);
    assert(strstr(s_render_buf, "line two") != NULL);
    assert(strstr(s_render_buf, "[ Wrote 3 lines ]") != NULL);
    assert(strstr(s_render_buf, "^O WriteOut") != NULL);
    assert(strstr(s_render_buf, "^X Exit") != NULL);
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
    printf("=== ALL NANO HOST TESTS PASSED (100%%) ===\n");
    return 0;
}

