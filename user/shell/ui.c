#include "ui.h"
#include "io.h"
#include "terminal.h"
#include "complete.h"
#include "history_persist.h"

static line_editor_t edit;
static terminal_info_t term;
static char input[1];
static size_t input_pos, input_len;

static int64_t g_ui_status = 0;
static char g_ui_cwd[256] = "/";
static bool g_ui_continuation = false;
static char g_prompt_template[64] = "fortress> ";

void shell_set_prompt_template(const char *tmpl) {
    if (!tmpl || !*tmpl) return;
    size_t i = 0;
    while (tmpl[i] && i < sizeof(g_prompt_template) - 1) {
        g_prompt_template[i] = tmpl[i];
        i++;
    }
    g_prompt_template[i] = '\0';
}

const char *shell_get_prompt_template(void) {
    return g_prompt_template;
}

void shell_set_prompt_state(int64_t status, const char *cwd) {
    g_ui_status = status;
    if (cwd && *cwd) {
        size_t n = 0;
        while (cwd[n] && n < sizeof(g_ui_cwd) - 1) {
            g_ui_cwd[n] = cwd[n];
            n++;
        }
        g_ui_cwd[n] = '\0';
    }
}

void shell_terminal(const char *arg) {
    (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
    if (*arg) {
        if (equal(arg, "local")) term.mode = TERM_LOCAL;
        else if (equal(arg, "serial")) term.mode = TERM_SERIAL;
        else if (equal(arg, "mirror")) term.mode = TERM_MIRROR;
        else if (equal(arg, "plain")) term.mode = TERM_PLAIN;
        else { puts("Usage: terminal [local|serial|mirror|plain]\n"); return; }
        term.cols = 0;
        if (call(SYS_TERMCTL, TERM_SET, (uintptr_t)&term, sizeof(term)) < 0) {
            puts("Terminal endpoint unavailable.\n");
            return;
        }
    }
    puts("Terminal mode "); put_dec(term.mode); puts(" width "); put_dec(term.cols); puts("\n");
}

void shell_history(const char *arg) {
    if (equal(arg, "clear")) { history_clear(); return; }
    if (equal(arg, "save")) {
        if (history_save()) puts("History saved to /mnt/.fortress/history\n");
        else puts("Failed to save history\n");
        return;
    }
    if (equal(arg, "load")) {
        if (history_load()) puts("History loaded from /mnt/.fortress/history\n");
        else puts("Failed to load history\n");
        return;
    }
    if (*arg) { puts("Usage: history [clear | save | load]\n"); return; }
    for (size_t i = 0; i < history_count(); i++) {
        put_dec(i + 1); puts("  "); puts(history_get(i)); puts("\n");
    }
}

static void paint(void) {
    size_t width = term.cols > 20 ? term.cols : 20;
    char prompt_buf[128];
    const char *prompt = prompt_buf;

    if (edit.blocked) {
        prompt = "[lost/full: Ctrl+C] ";
    } else if (edit.search) {
        const char *prefix = edit.search_failed ? "[no match: " : "[search: ";
        size_t n = 0;
        while (prefix[n]) { prompt_buf[n] = prefix[n]; n++; }
        size_t limit = width > 40 ? 20 : 4;
        for (size_t i = 0; i < edit.query_len && i < limit; i++) prompt_buf[n++] = edit.query[i];
        prompt_buf[n++] = ']'; prompt_buf[n++] = ' '; prompt_buf[n] = 0;
        prompt = prompt_buf;
    } else if (edit.review) {
        prompt = "[paste: Enter twice] ";
    } else if (g_ui_continuation) {
        prompt = "> ";
    } else {
        if (equal(g_prompt_template, "fortress> ")) {
            prompt = "fortress> ";
        } else {
            size_t n = 0;
            if (g_ui_status != 0) {
                prompt_buf[n++] = '[';
                if (g_ui_status < 0) {
                    prompt_buf[n++] = '-';
                    uint64_t v = (uint64_t)(0 - g_ui_status);
                    char tmp[24]; int tp = 0;
                    while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                    while (tp > 0) prompt_buf[n++] = tmp[--tp];
                } else {
                    uint64_t v = (uint64_t)g_ui_status;
                    char tmp[24]; int tp = 0;
                    while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                    while (tp > 0) prompt_buf[n++] = tmp[--tp];
                }
                prompt_buf[n++] = ']';
                prompt_buf[n++] = ' ';
            }
            const char *pfx = "fortress:";
            while (*pfx) prompt_buf[n++] = *pfx++;
            const char *c = g_ui_cwd;
            while (*c && n < sizeof(prompt_buf) - 5) prompt_buf[n++] = *c++;
            prompt_buf[n++] = ' ';
            prompt_buf[n++] = '$';
            prompt_buf[n++] = ' ';
            prompt_buf[n] = '\0';
        }
    }

    size_t plen = length(prompt);
    if (plen + 3 >= width) { prompt = "> "; plen = 2; }
    size_t room = width - plen - 2;
    if (edit.cursor < edit.view) edit.view = edit.cursor;
    if (edit.cursor >= edit.view + room) edit.view = edit.cursor - room + 1;
    if (edit.view > edit.len) edit.view = edit.len;
    size_t n = edit.len - edit.view; if (n > room) n = room;

    if (term.mode == TERM_PLAIN) {
        puts("\n"); puts(prompt); write_bytes(edit.text, edit.len); return;
    }
    puts("\r\033[?25l"); puts(prompt);
    write_bytes(edit.text + edit.view, n);
    puts("\033[K\r\033["); put_dec(plen + edit.cursor - edit.view); puts("C\033[?25h");
}

bool shell_read_line(char out[LINE_CAP], bool continuation) {
    g_ui_continuation = continuation;
    lineedit_init(&edit);
    (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
    if (term.mode != TERM_PLAIN) puts("\033[?25h\033[?2004h");
    paint();

    for (;;) {
        if (input_pos == input_len) {
            long n = call(SYS_INPUT_READ, (uintptr_t)input, sizeof(input), edit.escape_len ? 100 : (uintptr_t)-1);
            if (n == INPUT_LOST) { lineedit_lost(&edit); paint(); continue; }
            if (n < 0) { puts("Input unavailable.\n"); return false; }
            if (n == 0) { (void)lineedit_timeout(&edit); paint(); continue; }
            input_len = (size_t)n; input_pos = 0;
        }

        uint64_t generation = term.generation, dropped = term.dropped;
        (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
        if (term.dropped != dropped) { lineedit_lost(&edit); paint(); }
        if (term.generation != generation) { puts("\n"); paint(); }

        unsigned char c = (unsigned char)input[input_pos++];
        bool append = edit.cursor == edit.len && !edit.search && !edit.escape_len &&
                      !edit.paste && !edit.blocked && !edit.review &&
                      (term.mode == TERM_PLAIN || edit.len + 11 < term.cols) && c >= 32 && c < 127;

        enum edit_result result = lineedit_byte(&edit, c);
        if (result == EDIT_COMPLETE) {
            int comp = shell_do_completion(&edit);
            if (comp > 0) paint();
            continue;
        }

        if (result == EDIT_ACCEPT || result == EDIT_CANCEL || result == EDIT_EOF) {
            if (term.mode != TERM_PLAIN) puts("\033[?25l\033[?2004l");
            puts("\n");
            if (result == EDIT_EOF) return false;
            if (result == EDIT_CANCEL) { out[0] = 0; return true; }
            for (size_t i = 0; i <= edit.len; i++) out[i] = edit.text[i];
            if (!continuation) {
                history_add(out);
            }
            return true;
        }

        if (result == EDIT_CLEAR && term.mode != TERM_PLAIN) puts("\033[2J\033[H");
        if (append && !edit.blocked) write_bytes((const char *)&c, 1);
        else if (result != EDIT_NONE) paint();
    }
}
