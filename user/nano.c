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

/* Static BSS Allocations */
static char               s_text_pool[NANO_POOL_SIZE];
static nano_state_t       s_state;
static nano_input_state_t s_input_state;
static terminal_info_t    s_term;
static char               s_render_buf[8192];
static char               s_read_chunk[4096];
static char               s_prompt_buf[NANO_MAX_PATH];
static char               s_in_chunk[32];

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
}

static void set_status_wrote(size_t lines, size_t bytes) {
    size_t pos = 0;
    s_state.status_msg[0] = '\0';
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), "[ Wrote ");
    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)lines);
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), " lines, ");
    buf_append_u32(s_state.status_msg, &pos, sizeof(s_state.status_msg), (uint32_t)bytes);
    buf_append_str(s_state.status_msg, &pos, sizeof(s_state.status_msg), " bytes ]");
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
        if (write_all((int)fd, "\n", 1) != 0) {
            sys_close((int)fd);
            set_status_msg("[ Write error ]");
            return false;
        }
        total_bytes++;
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

    for (;;) {
        /* Render prompt on status bar */
        size_t ppos = 0;
        s_state.status_msg[0] = '\0';
        buf_append_str(s_state.status_msg, &ppos, sizeof(s_state.status_msg), prefix);
        buf_append_str(s_state.status_msg, &ppos, sizeof(s_state.status_msg), out_buf);

        size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
        (void)write_all(1, s_render_buf, rlen);

        long n = sys_input_read(s_in_chunk, sizeof(s_in_chunk), 100);
        if (n <= 0) continue;

        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s_in_chunk[i];
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
                }
            } else if (k == KEY_CHAR && ch >= 32 && ch < 127) {
                if (len < max_out - 1) {
                    out_buf[len++] = ch;
                    out_buf[len] = '\0';
                }
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

    /* 2. Ignore SIGINT so Ctrl+C cancels prompts without aborting */
    signal_action_t sig_ign = { .handler = SIG_IGN, .mask = 0, .flags = 0, .reserved = 0 };
    (void)sys_sigaction(SIGINT, &sig_ign, 0);

    /* 3. Initialize state */
    nano_init(&s_state, s_text_pool);
    nano_input_reset(&s_input_state);
    s_state.screen_rows = s_term.rows ? s_term.rows : 25;
    s_state.screen_cols = s_term.cols ? s_term.cols : 80;

    /* 4. Open and read file if specified */
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        const char *filepath = argv[1];
        size_t path_len = strlen(filepath);
        if (path_len >= sizeof(s_state.filename)) path_len = sizeof(s_state.filename) - 1;
        memcpy(s_state.filename, filepath, path_len);
        s_state.filename[path_len] = '\0';

        long fd = sys_open(filepath, VFS_O_RDONLY, 0);
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
                memcpy(s_text_pool + total_loaded, s_read_chunk, (size_t)rd);
                total_loaded += (size_t)rd;
            }
            sys_close((int)fd);

            if (too_big) {
                static const char big_msg[] = "nano: file exceeds buffer capacity (max 256 KiB)\n";
                (void)write_all(2, big_msg, sizeof(big_msg) - 1);
                return 1;
            }

            /* Load into row metadata structure */
            if (!nano_load_buffer(&s_state, s_text_pool, s_text_pool, total_loaded)) {
                static const char load_fail[] = "nano: failed to parse file structure\n";
                (void)write_all(2, load_fail, sizeof(load_fail) - 1);
                return 1;
            }

            /* Restore filename */
            memcpy(s_state.filename, filepath, path_len);
            s_state.filename[path_len] = '\0';

            /* Status */
            size_t spos = 0;
            s_state.status_msg[0] = '\0';
            buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), "[ Read ");
            buf_append_u32(s_state.status_msg, &spos, sizeof(s_state.status_msg), (uint32_t)s_state.num_rows);
            buf_append_str(s_state.status_msg, &spos, sizeof(s_state.status_msg), " lines ]");
        } else {
            set_status_msg("[ New File ]");
        }
    } else {
        set_status_msg("[ New Buffer ]");
    }

    /* 5. Main Editor Loop */
    for (;;) {
        /* Update terminal dimensions in case of change */
        long tr = sys_termctl(TERM_GET, (uintptr_t)&s_term, sizeof(s_term));
        if (tr == 0 && s_term.mode != TERM_PLAIN) {
            s_state.screen_rows = s_term.rows ? s_term.rows : 25;
            s_state.screen_cols = s_term.cols ? s_term.cols : 80;
        }

        /* Render frame */
        size_t frame_bytes = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
        if (write_all(1, s_render_buf, frame_bytes) != 0) {
            break;
        }

        /* Wait for input */
        long n = sys_input_read(s_in_chunk, sizeof(s_in_chunk), 100);
        if (n <= 0) continue;

        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s_in_chunk[i];
            char ch = 0;
            nano_key_t key = nano_parse_input(&s_input_state, c, &ch);

            switch (key) {
                case KEY_CHAR:
                    (void)nano_insert_char(&s_state, s_text_pool, ch);
                    break;
                case KEY_ENTER:
                    (void)nano_split_row(&s_state, s_text_pool);
                    break;
                case KEY_BACKSPACE:
                    (void)nano_backspace(&s_state, s_text_pool);
                    break;
                case KEY_DELETE:
                    (void)nano_delete_char(&s_state, s_text_pool);
                    break;
                case KEY_LEFT:
                    nano_move_left(&s_state, s_text_pool);
                    break;
                case KEY_RIGHT:
                    nano_move_right(&s_state, s_text_pool);
                    break;
                case KEY_UP:
                    nano_move_up(&s_state, s_text_pool);
                    break;
                case KEY_DOWN:
                    nano_move_down(&s_state, s_text_pool);
                    break;
                case KEY_HOME:
                case KEY_CTRL_A:
                    nano_move_home(&s_state);
                    break;
                case KEY_END:
                case KEY_CTRL_E:
                    nano_move_end(&s_state);
                    break;
                case KEY_PAGE_UP:
                    nano_page_up(&s_state, s_text_pool);
                    break;
                case KEY_PAGE_DOWN:
                    nano_page_down(&s_state, s_text_pool);
                    break;
                case KEY_CTRL_K:
                    (void)nano_cut_line(&s_state, s_text_pool);
                    set_status_msg("[ Cut line ]");
                    break;
                case KEY_CTRL_U:
                    if (nano_uncut_line(&s_state, s_text_pool)) {
                        set_status_msg("[ Uncut line ]");
                    } else {
                        set_status_msg("[ Nothing to uncut ]");
                    }
                    break;
                case KEY_CTRL_W: {
                    if (prompt_input("Search: ", "", s_prompt_buf, NANO_MAX_QUERY)) {
                        if (!nano_search(&s_state, s_text_pool, s_prompt_buf)) {
                            set_status_msg("[ Not found ]");
                        } else {
                            set_status_msg("[ Found match ]");
                        }
                    }
                    break;
                }
                case KEY_CTRL_O: {
                    if (prompt_input("File Name to Write: ", s_state.filename, s_prompt_buf, NANO_MAX_PATH)) {
                        (void)save_file(s_prompt_buf);
                    }
                    break;
                }
                case KEY_CTRL_X: {
                    if (s_state.modified) {
                        set_status_msg("Save modified buffer? (Y/N/C): ");
                        size_t rlen = nano_render_frame(&s_state, s_text_pool, s_render_buf, sizeof(s_render_buf));
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
                    break;
                default:
                    break;
            }
        }
    }

cleanup_exit:
    /* Clear screen, reset cursor */
    (void)write_all(1, "\x1b[2J\x1b[H\x1b[?25h", 14);
    return 0;
}
