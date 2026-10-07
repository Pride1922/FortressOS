#include "nano.h"
#include "syscall_abi.h"
#include "terminal.h"
#include "vfs.h"
#include "signal_abi.h"

#include "nano_core.c"

/* Freestanding System Call Wrappers */
static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_read(int fd, void *buf, size_t count) {
    long nr = SYS_READ;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_open(const char *path, uint64_t flags, uint64_t mode) {
    long nr = SYS_OPEN;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)path), "S"(flags), "d"(mode)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_close(int fd) {
    long nr = SYS_CLOSE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_sync(void) {
    long nr = SYS_SYNC;
    __asm__ volatile("syscall" : "+a"(nr) :
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_termctl(uint64_t op, uintptr_t ptr, size_t size) {
    long nr = SYS_TERMCTL;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(op), "S"(ptr), "d"(size)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_input_read(void *buf, size_t count, int64_t timeout_ms) {
    long nr = SYS_INPUT_READ;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf), "S"((uintptr_t)count), "d"((uintptr_t)timeout_ms)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_sigaction(int sig, const signal_action_t *act, signal_action_t *old) {
    long nr = SYS_SIGACTION;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)sig), "S"((uintptr_t)act), "d"((uintptr_t)old)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_termattr(int fd, uint64_t op, terminal_attrs_t *attrs) {
    long nr = SYS_TERMATTR;
    register uintptr_t r10 __asm__("r10") = sizeof(terminal_attrs_t);
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"(op), "d"((uintptr_t)attrs), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

/* Multi-Buffer Static Pools */
static char               s_buffer_pools[NANO_MAX_BUFFERS][NANO_POOL_SIZE];
static nano_state_t       s_buffers[NANO_MAX_BUFFERS];
static uint8_t            s_active_buf = 0;
static uint8_t            s_buf_count = 1;

#define s_state           (s_buffers[s_active_buf])
#define s_text_pool       (s_buffer_pools[s_active_buf])

static nano_input_state_t s_input_state;
static terminal_info_t    s_term;
static terminal_attrs_t   s_orig_attrs;
static bool               s_attrs_saved = false;
static char               s_render_buf[16384];
static char               s_read_chunk[4096];
static char               s_prompt_buf[NANO_MAX_PATH];
static char               s_in_chunk[32];
static bool               s_status_transient = false;

static int write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        long w = sys_write(fd, buf, count);
        if (w <= 0) return -1;
        buf += w;
        count -= (size_t)w;
    }
    return 0;
}

static void set_status_msg(const char *msg) {
    size_t i = 0;
    while (msg && msg[i] && i < sizeof(s_state.status_msg) - 1) {
        s_state.status_msg[i] = msg[i];
        i++;
    }
    s_state.status_msg[i] = '\0';
    s_status_transient = true;
}

static void set_status_wrote(size_t lines, size_t bytes) {
    size_t pos = 0;
    s_state.status_msg[0] = '\0';
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), "[ Wrote ");
    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)lines);
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), " lines, ");
    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)bytes);
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), " bytes ]");
    s_status_transient = true;
}

#define NANO_WARN_BYTES (NANO_POOL_SIZE * 8 / 10)
#define NANO_WARN_ROWS  (NANO_MAX_ROWS * 8 / 10)

static void check_capacity_warning(void) {
    if (s_state.total_bytes >= NANO_WARN_BYTES || s_state.num_rows >= NANO_WARN_ROWS) {
        set_status_msg("[ Warning: Buffer at 80%+ capacity ]");
    }
}

/* Parse /etc/nanorc or /mnt/nanorc settings */
static void load_nanorc_config(void) {
    long fd = sys_open("/etc/nanorc", VFS_O_RDONLY, 0);
    if (fd < 0) {
        fd = sys_open("/mnt/nanorc", VFS_O_RDONLY, 0);
    }
    if (fd < 0) return;

    static char cfg_buf[2048];
    long rd = sys_read((int)fd, cfg_buf, sizeof(cfg_buf) - 1);
    sys_close((int)fd);
    if (rd <= 0) return;
    cfg_buf[rd] = '\0';

    const char *p = cfg_buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }

        const char *line_start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        size_t l_len = (size_t)(p - line_start);

        /* Parse line tokens */
        char line[128];
        if (l_len >= sizeof(line)) l_len = sizeof(line) - 1;
        memcpy(line, line_start, l_len);
        line[l_len] = '\0';

        char *tok = line;
        while (*tok == ' ' || *tok == '\t') tok++;
        if (strncmp(tok, "set ", 4) == 0) {
            tok += 4;
            while (*tok == ' ' || *tok == '\t') tok++;
            if (strncmp(tok, "tabsize ", 8) == 0) {
                tok += 8;
                while (*tok == ' ' || *tok == '\t') tok++;
                uint8_t ts = 0;
                while (*tok >= '0' && *tok <= '9') {
                    ts = (uint8_t)(ts * 10 + (*tok - '0'));
                    tok++;
                }
                if (ts >= 1 && ts <= 16) {
                    for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
                        s_buffers[b].tab_size = ts;
                    }
                }
            } else if (strncmp(tok, "tabstospaces", 12) == 0) {
                for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
                    s_buffers[b].tab_to_spaces = true;
                }
            } else if (strncmp(tok, "linenumbers", 11) == 0) {
                for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
                    s_buffers[b].show_line_numbers = true;
                }
            } else if (strncmp(tok, "casesensitive", 13) == 0) {
                for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
                    s_buffers[b].case_sensitive = true;
                }
            } else if (strncmp(tok, "regex", 5) == 0) {
                for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
                    s_buffers[b].regex_search = true;
                }
            }
        }
    }
}

static bool save_file(const char *path) {
    if (!path || path[0] == '\0') {
        set_status_msg("[ No file name ]");
        return false;
    }

    set_status_msg("[ Writing... ]");
    size_t frame_len = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
    (void)write_all(1, s_render_buf, frame_len);

    long fd = sys_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (fd < 0) {
        set_status_msg("[ Error writing file ]");
        return false;
    }

    size_t total_bytes = 0;
    const char *eol = s_state.dos_mode ? "\r\n" : "\n";
    size_t eol_len = s_state.dos_mode ? 2 : 1;
    for (size_t i = 0; i < s_state.num_rows; i++) {
        uint32_t off = s_state.rows[i].offset;
        uint16_t len = s_state.rows[i].length;
        if (len > 0) {
            if (write_all((int)fd, s_text_pool + off, len) != 0) {
                sys_close((int)fd);
                set_status_msg("[ Write error ]");
                return false;
            }
            total_bytes += len;
        }
        if (write_all((int)fd, eol, eol_len) != 0) {
            sys_close((int)fd);
            set_status_msg("[ Write error ]");
            return false;
        }
        total_bytes += eol_len;
    }

    (void)sys_sync();
    (void)sys_close((int)fd);

    s_state.modified = false;
    size_t plen = strlen(path);
    if (plen >= sizeof(s_state.filename)) plen = sizeof(s_state.filename) - 1;
    memcpy(s_state.filename, path, plen);
    s_state.filename[plen] = '\0';

    set_status_wrote(s_state.num_rows, total_bytes);
    return true;
}

/* Interactive Status Bar Prompt */
static bool prompt_input(const char *prefix, const char *initial, char *out_buf, size_t max_out) {
    size_t len = 0;
    out_buf[0] = '\0';
    if (initial) {
        while (initial[len] && len < max_out - 1) {
            out_buf[len] = initial[len];
            len++;
        }
        out_buf[len] = '\0';
    }

    bool need_render = true;

    for (;;) {
        if (need_render) {
            /* Render prompt on status bar */
            size_t ppos = 0;
            s_state.status_msg[0] = '\0';
            buf_append_str(s_state.status_msg, &ppos, sizeof(s_state.status_msg), prefix);
            buf_append_str(s_state.status_msg, &ppos, sizeof(s_state.status_msg), out_buf);

            size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
            rlen += nano_render_prompt_cursor(&s_state, ppos, s_render_buf + rlen, sizeof(s_render_buf) - rlen);
            (void)write_all(1, s_render_buf, rlen);
            need_render = false;
        }

        char in_ch = 0;
        long n = sys_input_read(&in_ch, 1, 100);
        if (n <= 0) {
            /* Check if terminal dimensions or display generation changed */
            terminal_info_t cur_term;
            if (sys_termctl(TERM_GET, (uintptr_t)&cur_term, sizeof(cur_term)) == 0 && cur_term.mode != TERM_PLAIN) {
                if (cur_term.rows != s_state.screen_rows || cur_term.cols != s_state.screen_cols ||
                    cur_term.generation != s_term.generation) {
                    s_state.screen_rows = cur_term.rows ? cur_term.rows : 25;
                    s_state.screen_cols = cur_term.cols ? cur_term.cols : 80;
                    s_term = cur_term;
                    need_render = true;
                }
            }
            continue;
        }

        unsigned char c = (unsigned char)in_ch;
        char ch = 0;
        nano_key_t k = nano_parse_input(&s_input_state, c, &ch);

        if (k == KEY_ENTER) {
            out_buf[len] = '\0';
            return true;
        } else if (k == KEY_CTRL_C || k == KEY_CTRL_X) {
            set_status_msg("[ Cancelled ]");
            return false;
        } else if (k == KEY_BACKSPACE) {
            if (len > 0) {
                out_buf[--len] = '\0';
                need_render = true;
            }
        } else if (k == KEY_CHAR && ch >= 32 && ch < 127) {
            if (len < max_out - 1) {
                out_buf[len++] = ch;
                out_buf[len] = '\0';
                need_render = true;
            }
        }
    }
}

/* Interactive Search / Replace Input with Alt+C (Case) and Alt+R (RegEx) Toggles */
static bool prompt_input_search(const char *base_prompt, const char *initial, char *out_buf, size_t max_out) {
    size_t len = 0;
    out_buf[0] = '\0';
    if (initial) {
        while (initial[len] && len < max_out - 1) {
            out_buf[len] = initial[len];
            len++;
        }
        out_buf[len] = '\0';
    }

    bool need_render = true;

    for (;;) {
        if (need_render) {
            /* Construct dynamic prefix with flags: e.g. "Search [CS] [RegEx]: " */
            char dyn_prefix[64];
            size_t ppos = 0;
            dyn_prefix[0] = '\0';
            buf_append_str(dyn_prefix, &ppos, sizeof(dyn_prefix), base_prompt);
            if (s_state.case_sensitive) {
                buf_append_str(dyn_prefix, &ppos, sizeof(dyn_prefix), "[CS] ");
            }
            if (s_state.regex_search) {
                buf_append_str(dyn_prefix, &ppos, sizeof(dyn_prefix), "[RegEx] ");
            }
            buf_append_str(dyn_prefix, &ppos, sizeof(dyn_prefix), ": ");

            size_t spos = 0;
            s_state.status_msg[0] = '\0';
            buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), dyn_prefix);
            buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), out_buf);

            size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
            rlen += nano_render_prompt_cursor(&s_state, spos, s_render_buf + rlen, sizeof(s_render_buf) - rlen);
            (void)write_all(1, s_render_buf, rlen);
            need_render = false;
        }

        char in_ch = 0;
        long n = sys_input_read(&in_ch, 1, 100);
        if (n <= 0) {
            terminal_info_t cur_term;
            if (sys_termctl(TERM_GET, (uintptr_t)&cur_term, sizeof(cur_term)) == 0 && cur_term.mode != TERM_PLAIN) {
                if (cur_term.rows != s_state.screen_rows || cur_term.cols != s_state.screen_cols ||
                    cur_term.generation != s_term.generation) {
                    s_state.screen_rows = cur_term.rows ? cur_term.rows : 25;
                    s_state.screen_cols = cur_term.cols ? cur_term.cols : 80;
                    s_term = cur_term;
                    need_render = true;
                }
            }
            continue;
        }

        unsigned char c = (unsigned char)in_ch;
        char ch = 0;
        nano_key_t k = nano_parse_input(&s_input_state, c, &ch);

        if (k == KEY_ENTER) {
            out_buf[len] = '\0';
            return true;
        } else if (k == KEY_CTRL_C || k == KEY_CTRL_X) {
            set_status_msg("[ Cancelled ]");
            return false;
        } else if (k == KEY_BACKSPACE) {
            if (len > 0) {
                out_buf[--len] = '\0';
                need_render = true;
            }
        } else if (k == KEY_ALT_C) {
            s_state.case_sensitive = !s_state.case_sensitive;
            need_render = true;
        } else if (k == KEY_ALT_R) {
            s_state.regex_search = !s_state.regex_search;
            need_render = true;
        } else if (k == KEY_CHAR && ch >= 32 && ch < 127) {
            if (len < max_out - 1) {
                out_buf[len++] = ch;
                out_buf[len] = '\0';
                need_render = true;
            }
        }
    }
}

int nano_main(int argc, char **argv) {
    /* 1. Verify stdout is a terminal */
    long isatty = sys_termctl(TERM_ISATTY, 1, 0);
    long term_res = sys_termctl(TERM_GET, (uintptr_t)&s_term, sizeof(s_term));
    if (isatty != 1 || term_res != 0 || s_term.mode == TERM_PLAIN) {
        static const char notty_msg[] = "nano: standard output is not a terminal\n";
        (void)write_all(2, notty_msg, sizeof(notty_msg) - 1);
        return 1;
    }

    /* 2. Configure terminal attributes: disable TERM_ISIG so Ctrl+C and Ctrl+Z are passed as input bytes */
    if (sys_termattr(0, TERM_GET, &s_orig_attrs) == 0) {
        s_attrs_saved = true;
        terminal_attrs_t raw_attrs = s_orig_attrs;
        raw_attrs.input_flags &= ~TERM_ISIG;
        (void)sys_termattr(0, TERM_SET, &raw_attrs);
    }

    signal_action_t sig_ign = { .handler = SIG_IGN, .mask = 0, .flags = 0, .reserved = 0 };
    (void)sys_sigaction(SIGINT, &sig_ign, 0);

    /* 3. Initialize multi-buffers */
    for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
        nano_init(&s_buffers[b], s_buffer_pools[b]);
        s_buffers[b].screen_rows = s_term.rows ? s_term.rows : 25;
        s_buffers[b].screen_cols = s_term.cols ? s_term.cols : 80;
        s_buffers[b].buffer_idx = (uint8_t)b;
    }
    nano_input_reset(&s_input_state);

    /* Load /etc/nanorc settings */
    load_nanorc_config();

    /* 4. Parse CLI options and load file(s) or stdin */
    const char *filepaths[NANO_MAX_BUFFERS];
    uint8_t num_files = 0;
    bool from_stdin = false;
    bool cli_readonly = false;
    bool cli_linenumbers = false;

    for (int i = 1; i < argc; i++) {
        if (!argv[i] || argv[i][0] == '\0') continue;
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--view") == 0) {
            cli_readonly = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--linenumbers") == 0) {
            cli_linenumbers = true;
        } else if (strcmp(argv[i], "-") == 0) {
            from_stdin = true;
        } else if (argv[i][0] != '-') {
            if (num_files < NANO_MAX_BUFFERS) {
                filepaths[num_files++] = argv[i];
            }
        }
    }

    if (argc > 0 && argv[0] && strstr(argv[0], "view")) {
        cli_readonly = true;
    }

    for (int b = 0; b < NANO_MAX_BUFFERS; b++) {
        if (cli_readonly) s_buffers[b].readonly = true;
        if (cli_linenumbers) s_buffers[b].show_line_numbers = true;
    }

    if (from_stdin) {
        s_buf_count = 1;
        s_active_buf = 0;
        s_buffers[0].buffer_idx = 0;
        s_buffers[0].buffer_count = 1;

        size_t total_loaded = 0;
        bool too_big = false;

        for (;;) {
            long rd = sys_read(0, s_read_chunk, sizeof(s_read_chunk));
            if (rd <= 0) break;
            if (total_loaded + (size_t)rd > NANO_POOL_SIZE) {
                too_big = true;
                break;
            }
            memcpy(s_text_pool + total_loaded, s_read_chunk, (size_t)rd);
            total_loaded += (size_t)rd;
        }

        if (too_big) {
            static const char big_msg[] = "nano: standard input exceeds buffer capacity (max 256 KiB)\n";
            (void)write_all(2, big_msg, sizeof(big_msg) - 1);
            return 1;
        }

        if (!nano_load_buffer(&s_state, s_text_pool, s_text_pool, total_loaded)) {
            static const char load_fail[] = "nano: failed to parse stdin structure\n";
            (void)write_all(2, load_fail, sizeof(load_fail) - 1);
            return 1;
        }

        static const char stdin_name[] = "[Standard Input]";
        memcpy(s_state.filename, stdin_name, sizeof(stdin_name));

        size_t spos = 0;
        s_state.status_msg[0] = '\0';
        buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), "[ Read ");
        buf_append_u32(s_state.status_msg, &spos, sizeof(s_state.status_msg), (uint32_t)s_state.num_rows);
        buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), " lines from stdin ]");
        s_status_transient = true;
        check_capacity_warning();
    } else if (num_files > 0) {
        s_buf_count = num_files;
        s_active_buf = 0;

        for (uint8_t b = 0; b < num_files; b++) {
            s_buffers[b].buffer_idx = b;
            s_buffers[b].buffer_count = num_files;
            const char *fp = filepaths[b];
            size_t path_len = strlen(fp);
            if (path_len >= sizeof(s_buffers[b].filename)) path_len = sizeof(s_buffers[b].filename) - 1;
            memcpy(s_buffers[b].filename, fp, path_len);
            s_buffers[b].filename[path_len] = '\0';

            long fd = sys_open(fp, VFS_O_RDONLY, 0);
            if (fd >= 0) {
                size_t total_loaded = 0;
                bool too_big = false;

                for (;;) {
                    long rd = sys_read((int)fd, s_read_chunk, sizeof(s_read_chunk));
                    if (rd <= 0) break;
                    if (total_loaded + (size_t)rd > NANO_POOL_SIZE) {
                        too_big = true;
                        break;
                    }
                    memcpy(s_buffer_pools[b] + total_loaded, s_read_chunk, (size_t)rd);
                    total_loaded += (size_t)rd;
                }
                sys_close((int)fd);

                if (too_big) {
                    static const char big_msg[] = "nano: file exceeds buffer capacity (max 256 KiB)\n";
                    (void)write_all(2, big_msg, sizeof(big_msg) - 1);
                    return 1;
                }

                if (!nano_load_buffer(&s_buffers[b], s_buffer_pools[b], s_buffer_pools[b], total_loaded)) {
                    static const char load_fail[] = "nano: failed to parse file structure\n";
                    (void)write_all(2, load_fail, sizeof(load_fail) - 1);
                    return 1;
                }
                memcpy(s_buffers[b].filename, fp, path_len);
                s_buffers[b].filename[path_len] = '\0';

                size_t spos = 0;
                s_buffers[b].status_msg[0] = '\0';
                buf_append_str(s_buffers[b].status_msg, &spos, sizeof(s_buffers[b].status_msg), "[ Read ");
                buf_append_u32(s_buffers[b].status_msg, &spos, sizeof(s_buffers[b].status_msg), (uint32_t)s_buffers[b].num_rows);
                buf_append_str(s_buffers[b].status_msg, &spos, sizeof(s_buffers[b].status_msg), " lines ]");
            } else {
                size_t spos = 0;
                s_buffers[b].status_msg[0] = '\0';
                buf_append_str(s_buffers[b].status_msg, &spos, sizeof(s_buffers[b].status_msg), "[ New File ]");
            }
        }
        s_status_transient = true;
        check_capacity_warning();
    } else {
        s_buf_count = 1;
        s_active_buf = 0;
        s_buffers[0].buffer_idx = 0;
        s_buffers[0].buffer_count = 1;
        set_status_msg("[ New Buffer ]");
    }

    /* 5. Main Editor Loop */
    bool need_full_redraw = true;
    bool need_cursor_redraw = false;

    for (;;) {
        /* Update terminal dimensions or redraw if generation changed */
        terminal_info_t cur_term;
        if (sys_termctl(TERM_GET, (uintptr_t)&cur_term, sizeof(cur_term)) == 0 && cur_term.mode != TERM_PLAIN) {
            if (cur_term.rows != s_state.screen_rows || cur_term.cols != s_state.screen_cols ||
                cur_term.generation != s_term.generation) {
                s_state.screen_rows = cur_term.rows ? cur_term.rows : 25;
                s_state.screen_cols = cur_term.cols ? cur_term.cols : 80;
                s_term = cur_term;
                need_full_redraw = true;
            }
        }

        if (need_full_redraw) {
            size_t frame_bytes = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
            if (write_all(1, s_render_buf, frame_bytes) != 0) {
                break;
            }
            need_full_redraw = false;
            need_cursor_redraw = false;
        } else if (need_cursor_redraw) {
            size_t cur_bytes = nano_render_cursor(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
            if (write_all(1, s_render_buf, cur_bytes) != 0) {
                break;
            }
            need_cursor_redraw = false;
        }

        /* Wait for input (100ms timeout preserves responsive window resize detection) */
        long n = sys_input_read(s_in_chunk, sizeof(s_in_chunk), 100);
        if (n <= 0) continue;

        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s_in_chunk[i];
            char ch = 0;
            nano_key_t key = nano_parse_input(&s_input_state, c, &ch);
            if (key == KEY_NONE) continue;

            /* Dismiss transient status message on action */
            if (s_status_transient && key != KEY_CTRL_C) {
                s_state.status_msg[0] = '\0';
                s_status_transient = false;
                need_full_redraw = true;
            }

            size_t prev_row_off = s_state.row_offset;
            size_t prev_col_off = s_state.col_offset;

            switch (key) {
                case KEY_CHAR:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (ch == '\t' && s_state.tab_to_spaces) {
                        uint8_t ts = s_state.tab_size ? s_state.tab_size : 8;
                        const char *rchars = s_text_pool + s_state.rows[s_state.cy].offset;
                        size_t rlen = s_state.rows[s_state.cy].length;
                        size_t rx = nano_col_to_render(&s_state, rchars, rlen, s_state.cx);
                        uint8_t nspaces = ts - (uint8_t)(rx % ts);
                        if (nspaces == 0) nspaces = ts;
                        if (s_state.total_bytes + nspaces > NANO_POOL_SIZE ||
                            s_state.rows[s_state.cy].length + nspaces > NANO_MAX_LINE_LEN) {
                            set_status_msg("[ Buffer full: cannot insert ]");
                            need_full_redraw = true;
                            break;
                        }
                        char sp_buf[16];
                        for (uint8_t s = 0; s < nspaces; s++) sp_buf[s] = ' ';
                        nano_undo_push(&s_state, UNDO_OP_INSERT, s_state.cy, s_state.cx, sp_buf, nspaces);
                        for (uint8_t s = 0; s < nspaces; s++) {
                            (void)nano_insert_char(&s_state, s_text_pool, ' ');
                        }
                        check_capacity_warning();
                        need_full_redraw = true;
                        break;
                    }
                    if (s_state.total_bytes + 1 > NANO_POOL_SIZE || s_state.rows[s_state.cy].length >= NANO_MAX_LINE_LEN) {
                        set_status_msg("[ Buffer full: cannot insert ]");
                        need_full_redraw = true;
                        break;
                    }
                    nano_undo_push(&s_state, UNDO_OP_INSERT, s_state.cy, s_state.cx, &ch, 1);
                    (void)nano_insert_char(&s_state, s_text_pool, ch);
                    check_capacity_warning();
                    need_full_redraw = true;
                    break;
                case KEY_ENTER:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (s_state.num_rows >= NANO_MAX_ROWS) {
                        set_status_msg("[ Buffer full: max lines reached ]");
                        need_full_redraw = true;
                        break;
                    }
                    nano_undo_push(&s_state, UNDO_OP_SPLIT, s_state.cy, s_state.cx, NULL, 0);
                    (void)nano_split_row(&s_state, s_text_pool);
                    check_capacity_warning();
                    need_full_redraw = true;
                    break;
                case KEY_BACKSPACE:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (s_state.cx > 0) {
                        char del_c = s_text_pool[s_state.rows[s_state.cy].offset + s_state.cx - 1];
                        nano_undo_push(&s_state, UNDO_OP_DELETE, s_state.cy, s_state.cx - 1, &del_c, 1);
                    } else if (s_state.cy > 0) {
                        nano_undo_push(&s_state, UNDO_OP_JOIN, s_state.cy - 1, s_state.rows[s_state.cy - 1].length, NULL, 0);
                    }
                    (void)nano_backspace(&s_state, s_text_pool);
                    need_full_redraw = true;
                    break;
                case KEY_DELETE:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (s_state.cx < s_state.rows[s_state.cy].length) {
                        char del_c = s_text_pool[s_state.rows[s_state.cy].offset + s_state.cx];
                        nano_undo_push(&s_state, UNDO_OP_DELETE, s_state.cy, s_state.cx, &del_c, 1);
                    } else if (s_state.cy + 1 < s_state.num_rows) {
                        nano_undo_push(&s_state, UNDO_OP_JOIN, s_state.cy, s_state.cx, NULL, 0);
                    }
                    (void)nano_delete_char(&s_state, s_text_pool);
                    need_full_redraw = true;
                    break;
                case KEY_LEFT:
                    nano_move_left(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_RIGHT:
                    nano_move_right(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_WORD_LEFT:
                    nano_word_left(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_WORD_RIGHT:
                    nano_word_right(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_UP:
                    nano_move_up(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_DOWN:
                    nano_move_down(&s_state, s_text_pool);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_HOME:
                case KEY_CTRL_A:
                    nano_move_home(&s_state);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_END:
                case KEY_CTRL_E:
                    nano_move_end(&s_state);
                    nano_update_viewport(&s_state, s_text_pool);
                    if (s_state.row_offset != prev_row_off || s_state.col_offset != prev_col_off) {
                        need_full_redraw = true;
                    } else if (!need_full_redraw) {
                        need_cursor_redraw = true;
                    }
                    break;
                case KEY_PAGE_UP:
                    nano_page_up(&s_state, s_text_pool);
                    need_full_redraw = true;
                    break;
                case KEY_PAGE_DOWN:
                    nano_page_down(&s_state, s_text_pool);
                    need_full_redraw = true;
                    break;
                case KEY_CTRL_K:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    (void)nano_cut_line(&s_state, s_text_pool);
                    set_status_msg("[ Cut line ]");
                    need_full_redraw = true;
                    break;
                case KEY_CTRL_U:
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (nano_uncut_line(&s_state, s_text_pool)) {
                        set_status_msg("[ Uncut line ]");
                    } else {
                        set_status_msg("[ Nothing to uncut ]");
                    }
                    check_capacity_warning();
                    need_full_redraw = true;
                    break;
                case KEY_CTRL_W: {
                    if (prompt_input_search("Search", "", s_prompt_buf, NANO_MAX_QUERY)) {
                        if (!nano_search(&s_state, s_text_pool, s_prompt_buf, NULL)) {
                            set_status_msg("[ Not found ]");
                        } else {
                            set_status_msg("[ Found match ]");
                        }
                    }
                    need_full_redraw = true;
                    break;
                }
                case KEY_CTRL_R: {
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    static char replace_query[NANO_MAX_QUERY];
                    static char replace_rep[NANO_MAX_QUERY];
                    if (!prompt_input_search("Search to replace", "", replace_query, sizeof(replace_query))) {
                        need_full_redraw = true;
                        break;
                    }
                    if (replace_query[0] == '\0') {
                        need_full_redraw = true;
                        break;
                    }
                    if (!prompt_input("Replace with: ", "", replace_rep, sizeof(replace_rep))) {
                        need_full_redraw = true;
                        break;
                    }

                    size_t count_replaced = 0;
                    bool replace_all = false;
                    bool aborted = false;

                    /* Move cx back by 1 if possible so search finds match right under cursor */
                    if (s_state.cx > 0) s_state.cx--;

                    while (nano_search(&s_state, s_text_pool, replace_query, NULL)) {
                        if (replace_all) {
                            if (nano_replace_current_match(&s_state, s_text_pool, replace_query, replace_rep)) {
                                count_replaced++;
                            } else {
                                break;
                            }
                            continue;
                        }

                        /* Interactive prompt */
                        set_status_msg("Replace this instance? (Y)es, (N)o, (A)ll, (Q)uit: ");
                        size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
                        size_t ppos = strlen(s_state.status_msg);
                        rlen += nano_render_prompt_cursor(&s_state, ppos, s_render_buf + rlen, sizeof(s_render_buf) - rlen);
                        (void)write_all(1, s_render_buf, rlen);

                        bool handled = false;
                        while (!handled) {
                            long rn = sys_input_read(s_in_chunk, sizeof(s_in_chunk), 100);
                            if (rn <= 0) continue;
                            char resp = s_in_chunk[0];
                            if (resp == 'y' || resp == 'Y') {
                                if (nano_replace_current_match(&s_state, s_text_pool, replace_query, replace_rep)) {
                                    count_replaced++;
                                }
                                handled = true;
                            } else if (resp == 'n' || resp == 'N') {
                                handled = true;
                            } else if (resp == 'a' || resp == 'A') {
                                replace_all = true;
                                if (nano_replace_current_match(&s_state, s_text_pool, replace_query, replace_rep)) {
                                    count_replaced++;
                                }
                                handled = true;
                            } else if (resp == 'q' || resp == 'Q' || resp == 3 || resp == 27) {
                                aborted = true;
                                handled = true;
                                break;
                            }
                        }
                        if (aborted) break;
                    }

                    size_t spos = 0;
                    s_state.status_msg[0] = '\0';
                    buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), "[ Replaced ");
                    buf_append_u32(s_state.status_msg, &spos, sizeof(s_state.status_msg), (uint32_t)count_replaced);
                    buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), " occurrences ]");
                    s_status_transient = true;
                    need_full_redraw = true;
                    break;
                }
                case KEY_CTRL_G: {
                    if (prompt_input("Go to line, column: ", "", s_prompt_buf, sizeof(s_prompt_buf))) {
                        s_state.status_msg[0] = '\0';
                        size_t t_line = 0;
                        size_t t_col = 1;
                        const char *p = s_prompt_buf;
                        while (*p == ' ') p++;
                        while (*p >= '0' && *p <= '9') {
                            t_line = t_line * 10 + (size_t)(*p - '0');
                            p++;
                        }
                        while (*p == ' ' || *p == ',' || *p == ':') p++;
                        if (*p >= '0' && *p <= '9') {
                            t_col = 0;
                            while (*p >= '0' && *p <= '9') {
                                t_col = t_col * 10 + (size_t)(*p - '0');
                                p++;
                            }
                        }
                        if (t_line > 0) {
                            nano_go_to_line(&s_state, t_line, t_col);
                        }
                    }
                    need_full_redraw = true;
                    break;
                }
                case KEY_CTRL_Z: {
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (nano_undo(&s_state, s_text_pool)) {
                        set_status_msg("[ Undone ]");
                    } else {
                        set_status_msg("[ Nothing to undo ]");
                    }
                    need_full_redraw = true;
                    break;
                }
                case KEY_CTRL_Y:
                case KEY_ALT_U: {
                    if (s_state.readonly) {
                        set_status_msg("[ Buffer is read-only ]");
                        need_full_redraw = true;
                        break;
                    }
                    if (nano_redo(&s_state, s_text_pool)) {
                        set_status_msg("[ Redone ]");
                    } else {
                        set_status_msg("[ Nothing to redo ]");
                    }
                    need_full_redraw = true;
                    break;
                }
                case KEY_ALT_N:
                    s_state.show_line_numbers = !s_state.show_line_numbers;
                    set_status_msg(s_state.show_line_numbers ? "[ Line numbers enabled ]" : "[ Line numbers disabled ]");
                    need_full_redraw = true;
                    break;
                case KEY_CTRL_O: {
                    if (s_state.readonly) {
                        set_status_msg("[ Cannot write in read-only mode ]");
                        need_full_redraw = true;
                        break;
                    }
                    const char *init_fn = (strcmp(s_state.filename, "[Standard Input]") == 0) ? "" : s_state.filename;
                    if (prompt_input("File Name to Write: ", init_fn, s_prompt_buf, NANO_MAX_PATH)) {
                        (void)save_file(s_prompt_buf);
                    }
                    need_full_redraw = true;
                    break;
                }
                case KEY_CTRL_X: {
                    if (s_state.readonly) {
                        goto cleanup_exit;
                    }
                    if (s_state.modified) {
                        set_status_msg("Save modified buffer? (Y/N/C): ");
                        size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
                        size_t ppos = strlen(s_state.status_msg);
                        rlen += nano_render_prompt_cursor(&s_state, ppos, s_render_buf + rlen, sizeof(s_render_buf) - rlen);
                        (void)write_all(1, s_render_buf, rlen);

                        bool handled = false;
                        while (!handled) {
                            long rn = sys_input_read(s_in_chunk, sizeof(s_in_chunk), 100);
                            if (rn <= 0) continue;
                            char resp = s_in_chunk[0];
                            if (resp == 'y' || resp == 'Y') {
                                if (s_state.filename[0] == '\0') {
                                    if (prompt_input("File Name to Write: ", "", s_prompt_buf, NANO_MAX_PATH)) {
                                        (void)save_file(s_prompt_buf);
                                    }
                                } else {
                                    (void)save_file(s_state.filename);
                                }
                                handled = true;
                                goto cleanup_exit;
                            } else if (resp == 'n' || resp == 'N') {
                                handled = true;
                                goto cleanup_exit;
                            } else if (resp == 'c' || resp == 'C' || resp == 3 || resp == 27) {
                                set_status_msg("[ Cancelled ]");
                                handled = true;
                                need_full_redraw = true;
                                break;
                            }
                        }
                    } else {
                        goto cleanup_exit;
                    }
                    break;
                }
                case KEY_CTRL_L:
                    /* Force clear screen and repaint */
                    (void)write_all(1, "\x1b[2J\x1b[H", 7);
                    need_full_redraw = true;
                    break;
                case KEY_CTRL_C: {
                    size_t pos = 0;
                    s_state.status_msg[0] = '\0';
                    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), "[ line ");
                    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)(s_state.cy + 1));
                    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), "/");
                    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)s_state.num_rows);
                    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), ", col ");
                    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)(s_state.cx + 1));
                    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), "/");
                    size_t cur_len = s_state.rows[s_state.cy].length;
                    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)(cur_len + 1));
                    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), " ]");
                    s_status_transient = true;
                    need_full_redraw = true;
                    break;
                }
                case KEY_ALT_D:
                    s_state.dos_mode = !s_state.dos_mode;
                    s_state.modified = true;
                    set_status_msg(s_state.dos_mode ? "[ DOS format (CRLF) ]" : "[ Unix format (LF) ]");
                    need_full_redraw = true;
                    break;
                case KEY_ALT_PREV_BUF:
                    if (s_buf_count > 1) {
                        s_active_buf = (s_active_buf == 0) ? (s_buf_count - 1) : (s_active_buf - 1);
                        set_status_msg("[ Switched to previous buffer ]");
                        need_full_redraw = true;
                    }
                    break;
                case KEY_ALT_NEXT_BUF:
                    if (s_buf_count > 1) {
                        s_active_buf = (s_active_buf + 1) % s_buf_count;
                        set_status_msg("[ Switched to next buffer ]");
                        need_full_redraw = true;
                    }
                    break;
                default:
                    break;
            }
        }
    }

cleanup_exit:
    if (s_attrs_saved) {
        (void)sys_termattr(0, TERM_SET, &s_orig_attrs);
    }
    /* Clear screen, reset cursor */
    (void)write_all(1, "\x1b[2J\x1b[H\x1b[?25h", 14);
    return 0;
}
