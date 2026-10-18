#include "ui.h"
#include "io.h"
#include "terminal.h"
#include "complete.h"
#include "history_persist.h"
#include "history_expand.h"
#include "jobs.h"
#include "../tools/userdb.h"
#include "install_abi.h"

static line_editor_t edit;
static char expanded_history[LINE_CAP];
static terminal_info_t term;
static char input[1];
static size_t input_pos, input_len;
static volatile unsigned prompt_interrupt;
static void prompt_sigint(unsigned sig) { (void)sig; prompt_interrupt=1; }
/* Caught handlers reset to default on spawn; SIG_IGN would be inherited and
 * make every launched job immune to Ctrl+Z. The prompt itself must not stop. */
static void prompt_sigtstp(unsigned sig) { (void)sig; }

static int64_t g_ui_status = 0;
static char g_ui_cwd[256] = "/";
static char g_ui_user[32]="user",g_ui_host[64]="fortress";
static userdb_t prompt_database;
static char prompt_accounts[DB_FILE_MAX+1];
static uint32_t g_ui_euid = UINT32_MAX;
static bool g_ui_continuation = false;
static char g_prompt_template[64] = "fortress:<cwd> $ ";

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

static int g_term_fd = 1;

void shell_set_terminal_fd(int fd) {
    g_term_fd = fd;
}

int shell_get_terminal_fd(void) {
    return g_term_fd;
}

void shell_ui_init(void) {
    signal_action_t action={.handler=(uintptr_t)prompt_sigint};
    (void)call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0);
    action.handler=(uintptr_t)prompt_sigtstp;
    (void)call(SYS_SIGACTION,SIGTSTP,(uintptr_t)&action,0);
    action.handler=SIG_IGN;
    (void)call(SYS_SIGACTION,SIGTTOU,(uintptr_t)&action,0);
    /* Retain private controlling terminal handle on high descriptor (31) with CLOEXEC */
    long term = call(SYS_OPEN, (uintptr_t)"/dev/tty", VFS_O_RDWR | VFS_O_CLOEXEC, 0);
    if (term >= 0) {
        if (term != 31) {
            long new_term = call(SYS_FCNTL, (uintptr_t)term, F_DUPFD_CLOEXEC, 31);
            if (new_term >= 0) {
                (void)call(SYS_CLOSE, (uintptr_t)term, 0, 0);
                term = new_term;
            }
        }
        shell_set_terminal_fd((int)term);
    } else {
        /* Fallback: dup existing stdout to 31 with CLOEXEC */
        long term2 = call(SYS_FCNTL, 1, F_DUPFD_CLOEXEC, 31);
        if (term2 >= 0) {
            shell_set_terminal_fd((int)term2);
        }
    }
}

static void ui_puts(const char *s) {
    puts_fd(g_term_fd, s);
}

static void ui_write_bytes(const char *s, size_t n) {
    write_bytes_fd(g_term_fd, s, n);
}

static void ui_put_dec(size_t val) {
    char buf[24];
    size_t i = 0;
    if (val == 0) { ui_puts("0"); return; }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    buf[i] = 0;
    ui_puts(buf);
}

void shell_set_prompt_state(int64_t status, const char *cwd) {
    g_ui_status = status;
    uint32_t real, effective, saved;
    g_ui_euid = call(SYS_GETRESUID, (uintptr_t)&real, (uintptr_t)&effective, (uintptr_t)&saved) == 0 ? effective : UINT32_MAX;
    /* Resolve the effective UID from the account file, not a mutable USER
     * environment variable. Bounded files and safe names cannot inject escapes. */
    long fd=call(SYS_OPEN,(uintptr_t)"/etc/passwd",VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    const db_user_t *user=NULL;
    if (fd>=0) {
        long n=call(SYS_READ,fd,(uintptr_t)prompt_accounts,sizeof(prompt_accounts));
        (void)call(SYS_CLOSE,fd,0,0);
        if (n>0 && n<=DB_FILE_MAX && db_passwd(&prompt_database,prompt_accounts,(size_t)n)) user=db_user_id(&prompt_database,g_ui_euid);
    }
    const char *name=user ? user->name : g_ui_euid==0 ? "root" : "user";
    unsigned un=0;while (name[un] && un<31) {g_ui_user[un]=name[un];un++;}g_ui_user[un]=0;
    const char *default_host="fortress";for (unsigned i=0;i<9;i++) g_ui_host[i]=default_host[i];
    fd=call(SYS_OPEN,(uintptr_t)"/etc/hostname",VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (fd>=0) {
        char text[65]={0};long n=call(SYS_READ,fd,(uintptr_t)text,sizeof(text));(void)call(SYS_CLOSE,fd,0,0);
        if (n>1 && n<=64 && text[n-1]=='\n') {
            text[n-1]=0;
            if (install_hostname_valid(text)) {unsigned i=0;for (;text[i];i++) g_ui_host[i]=text[i];g_ui_host[i]=0;}
        }
    }
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
    if (equal(arg, "clear") || equal(arg, "-c")) {
        history_clear();
        if (!history_save()) {
            history_mark_dirty();
        }
        return;
    }
    if (equal(arg, "save")) {
        if (history_save()) puts("History saved\n");
        else puts("Failed to save history\n");
        return;
    }
    if (equal(arg, "load")) {
        if (history_load()) puts("History loaded\n");
        else puts("Failed to load history\n");
        return;
    }
    if (*arg) { puts("Usage: history [-c | clear | save | load]\n"); return; }
    for (size_t i = 0; i < history_count(); i++) {
        put_dec(i + 1); puts("  "); puts(history_get(i)); puts("\n");
    }
}

static size_t edit_columns(size_t from, size_t to) {
    size_t cols=0;
    for (size_t i=from;i<to;i++) cols+=edit.text[i]=='\n' ? 2 : 1;
    return cols;
}

static void paint_text(size_t from, size_t count) {
    size_t end=from+count;
    while (from<end) {
        size_t next=from;
        while (next<end && edit.text[next]!='\n') next++;
        if (next>from) ui_write_bytes(edit.text+from,next-from);
        if (next<end) { ui_puts("\\n");next++; }
        from=next;
    }
}

static void paint(void) {
    size_t width = term.cols > 20 ? term.cols : 20;
    char prompt_buf[384];
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
        size_t at=0;
        const char *parts[]={g_ui_user,"@",g_ui_host,":",g_ui_cwd,g_ui_euid==0 ? "# " : "$ "};
        for (unsigned i=0;i<6;i++) for (size_t j=0;parts[i][j] && at<sizeof(prompt_buf)-1;j++) prompt_buf[at++]=parts[i][j];
        prompt_buf[at]=0;
        /* Keep the end of a deep path visible while leaving command-edit room. */
        if (at+12>=width && width>20) {
            size_t names=width/4,un=length(g_ui_user),hn=length(g_ui_host);
            if (un>names) un=names;
            if (hn>names) hn=names;
            at=0;for (size_t i=0;i<un;i++) prompt_buf[at++]=g_ui_user[i];prompt_buf[at++]='@';
            for (size_t i=0;i<hn;i++) prompt_buf[at++]=g_ui_host[i];
            prompt_buf[at++]=':';
            size_t keep=width-at-14,n=length(g_ui_cwd);
            if (n>keep) {prompt_buf[at++]='.';prompt_buf[at++]='.';keep=keep>2 ? keep-2 : 0;}
            else keep=n;
            for (size_t i=n-keep;i<n;i++) prompt_buf[at++]=g_ui_cwd[i];
            prompt_buf[at++]=g_ui_euid==0 ? '#' : '$';prompt_buf[at++]=' ';prompt_buf[at]=0;
        }
        prompt=prompt_buf;
    }

    size_t plen = length(prompt);
    if (plen + 3 >= width) { prompt = g_ui_euid == 0 ? "# " : "$ "; plen = length(prompt); }
    size_t room = width - plen - 2;
    if (edit.cursor < edit.view) edit.view = edit.cursor;
    while (edit.view<edit.cursor && edit_columns(edit.view,edit.cursor)>=room) edit.view++;
    if (edit.view > edit.len) edit.view = edit.len;
    size_t n=0, drawn=0;
    while (edit.view+n<edit.len) {
        size_t cols=edit.text[edit.view+n]=='\n' ? 2 : 1;
        if (drawn+cols>room) break;
        drawn+=cols;n++;
    }

    if (term.mode == TERM_PLAIN) {
        ui_puts("\n"); ui_puts(prompt); paint_text(0, edit.len);
        (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
        return;
    }
    ui_puts("\r\033[?25l"); ui_puts(prompt);
    paint_text(edit.view, n);
    ui_puts("\033[K\r\033["); ui_put_dec(plen + edit_columns(edit.view,edit.cursor)); ui_puts("C\033[?25h");
    (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
}

static bool shell_edit_line(char out[LINE_CAP], bool continuation) {
    prompt_interrupt=0;
    g_ui_continuation = continuation;
    lineedit_init(&edit);
    (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
    if (term.mode != TERM_PLAIN) ui_puts("\033[?25h\033[?2004h");
    paint();

    for (;;) {
        if (prompt_interrupt) {
            prompt_interrupt=0; input_pos=input_len=0; out[0]=0;
            if (term.mode!=TERM_PLAIN) ui_puts("\033[?25l\033[?2004l");
            ui_puts("\n"); return true;
        }
        if (input_pos == input_len) {
            /* Bounded wait also covers a signal handled just before read entry. */
            long n = call(SYS_INPUT_READ, (uintptr_t)input, sizeof(input), 100);
            if (n==SYSCALL_EINTR || n==0) {
                /* Scan even without a CHLD flag: delivery can precede sleep.
                 * Keep the BSS editor (including cursor/history/paste) intact. */
                bool repaint = jobs_reap_prompt();
                if (n==0 && edit.escape_len) {
                    (void)lineedit_timeout(&edit);
                    repaint = true;
                }
                terminal_info_t cur_term;
                if (call(SYS_TERMCTL, TERM_GET, (uintptr_t)&cur_term, sizeof(cur_term)) == 0) {
                    if (cur_term.generation != term.generation) {
                        /* Background output occurred: advance line and redraw prompt */
                        ui_puts("\n");
                        repaint = true;
                    }
                    if (cur_term.dropped != term.dropped) {
                        lineedit_lost(&edit);
                        repaint = true;
                    }
                    term = cur_term;
                }
                if (repaint && !prompt_interrupt) paint();
                continue;
            }
            if (prompt_interrupt) continue;
            if (n == INPUT_LOST) { lineedit_lost(&edit); paint(); continue; }
            if (n < 0) { ui_puts("Input unavailable.\n"); return false; }
            input_len = (size_t)n; input_pos = 0;
        }

        terminal_info_t cur_term;
        if (call(SYS_TERMCTL, TERM_GET, (uintptr_t)&cur_term, sizeof(cur_term)) == 0) {
            bool repaint = false;
            if (cur_term.dropped != term.dropped) { lineedit_lost(&edit); repaint = true; }
            if (cur_term.generation != term.generation) { ui_puts("\n"); repaint = true; }
            term = cur_term;
            if (repaint) paint();
        }

        unsigned char c = (unsigned char)input[input_pos++];
        bool append = edit.cursor == edit.len && !edit.search && !edit.escape_len &&
                      !edit.paste && !edit.blocked && !edit.review &&
                      term.mode == TERM_PLAIN && c >= 32 && c < 127;

        enum edit_result result = lineedit_byte(&edit, c);
        if (result == EDIT_COMPLETE) {
            int comp = shell_do_completion(&edit);
            if (comp > 0) paint();
            continue;
        }

        if (result == EDIT_ACCEPT || result == EDIT_CANCEL || result == EDIT_EOF) {
            if (term.mode != TERM_PLAIN) ui_puts("\033[?25l\033[?2004l");
            ui_puts("\n");
            if (result == EDIT_EOF) return false;
            if (result == EDIT_CANCEL) { out[0] = 0; return true; }
            for (size_t i = 0; i <= edit.len; i++) out[i] = edit.text[i];
            if (!continuation) {
                size_t count=history_count();
                int expansion=history_expand_sudo(out,count ? history_get(count-1) : NULL,
                                                 expanded_history,sizeof(expanded_history));
                if (expansion<0) {
                    ui_puts(expansion==-1 ? "shell: no previous command\n" : "shell: previous command is too long to elevate\n");
                    out[0]=0;return true;
                }
                if (expansion==1) {
                    size_t i=0;do {out[i]=expanded_history[i];} while (expanded_history[i++]);
                    ui_puts(out);ui_puts("\n");
                }
                history_add(out);
                history_mark_dirty();
            }
            return true;
        }

        if (result == EDIT_CLEAR && term.mode != TERM_PLAIN) ui_puts("\033[2J\033[H");
        if (append && !edit.blocked) {
            ui_write_bytes((const char *)&c, 1);
            (void)call(SYS_TERMCTL, TERM_GET, (uintptr_t)&term, sizeof(term));
        }
        else if (result != EDIT_NONE) paint();
    }
}

/* The line editor renders input itself. Preserve job settings while suppressing
 * PTY line-discipline echo at the prompt, including nested sudo shells. */
#ifdef SHELL_TERMATTR_HOST_TEST
extern long shell_termattr_call(long, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define ui_call4 shell_termattr_call
#else
static long ui_call4(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    register uintptr_t r10 __asm__("r10")=d;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif

bool shell_read_line(char out[LINE_CAP], bool continuation) {
    terminal_attrs_t saved, editing;
    bool changed=false;
    if (ui_call4(SYS_TERMATTR,g_term_fd,TERM_GET,(uintptr_t)&saved,sizeof(saved))==0) {
        editing=saved;
        editing.input_flags &= ~TERM_ECHO;
        changed=ui_call4(SYS_TERMATTR,g_term_fd,TERM_SET,(uintptr_t)&editing,sizeof(editing))==0;
    }
    bool result=shell_edit_line(out,continuation);
    if (changed) (void)ui_call4(SYS_TERMATTR,g_term_fd,TERM_SET,(uintptr_t)&saved,sizeof(saved));
    return result;
}
