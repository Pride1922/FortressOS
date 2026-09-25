#ifndef SHELL_LINEEDIT_H
#define SHELL_LINEEDIT_H
#include "types.h"
#define LINE_CAP 4097
#define HISTORY_COUNT 1000
#define HISTORY_BYTES (256 * 1024)
enum edit_result { EDIT_NONE, EDIT_CHANGED, EDIT_ACCEPT, EDIT_CANCEL, EDIT_EOF, EDIT_CLEAR, EDIT_COMPLETE };
typedef struct {
    char text[LINE_CAP], draft[LINE_CAP], saved[LINE_CAP], kill[LINE_CAP], query[LINE_CAP];
    size_t len, cursor, view, draft_cursor, saved_cursor, query_len;
    size_t history_pos, search_pos;
    bool search, search_failed, blocked, paste, review;
    char escape[16];
    unsigned escape_len;
} line_editor_t;
void lineedit_init(line_editor_t *e);
enum edit_result lineedit_byte(line_editor_t *e, unsigned char c);
enum edit_result lineedit_timeout(line_editor_t *e);
void lineedit_lost(line_editor_t *e);
void history_add(const char *line);
void history_clear(void);
size_t history_count(void);
const char *history_get(size_t index);
#endif
