#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "udp_common.h"
#include "dns.h"
#include "dns_codec.h"
#include "netconf.h"
#include "wget_codec.h"
#include "wget.h"
#include "tls.h"

#ifdef TEST_HOST
#include <stdlib.h>
#endif

#ifndef WGET_CALL
#define WGET_CALL udp_call
#endif

#ifndef NETCONF_SYSCALL
#define NETCONF_SYSCALL(nr, a, b, c) WGET_CALL(nr, a, b, c, 0, 0, 0)
#endif

#ifndef WGET_DEFAULT_ANCHOR_PATH
#define WGET_DEFAULT_ANCHOR_PATH "/etc/ssl/certs/anchors.v1"
#endif

#define WGET_MAX_CUMULATIVE_PAYLOAD 10485760ULL /* 10 MiB limit across all redirects */

/* Static BSS storage strictly adhering to Ring 3 stack budget and Gate G1 */
static uint8_t s_io_buf[MAX_SYSCALL_WRITE_LEN];
static uint8_t s_chunk_buf[MAX_SYSCALL_WRITE_LEN];
static uint8_t s_header_buf[WGET_MAX_HEADER_BYTES + 1];
static wget_url_t s_current_url;
static wget_url_t s_redirect_url;
static wget_response_t s_response;
static wget_chunked_decoder_t s_chunk_dec;
static dns_context_t s_dns_context;
static dns_result_t s_dns_result;
static char s_out_path[WGET_MAX_FILENAME_LEN];
static char s_tmp_path[WGET_MAX_FILENAME_LEN + 64];
static char s_request_buf[2048];
static sysinfo_t s_start_sysinfo;
static sysinfo_t s_end_sysinfo;

static fortress_trust_store_t s_trust_store;
static fortress_tls_t s_tls_session;
static bool s_trust_store_loaded = false;

static void message(const char *text) {
    size_t left = udp_length(text);
    while (left) {
        long n = WGET_CALL(SYS_WRITE, 2, (uintptr_t)text, left, 0, 0, 0);
        if (n <= 0 || (size_t)n > left) break;
        text += n;
        left -= (size_t)n;
    }
}

static void print_dec(uint64_t val) {
    char buf[32];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char tmp[32];
        int t = 0;
        while (val > 0) {
            tmp[t++] = (char)('0' + (val % 10));
            val /= 10;
        }
        while (t > 0) {
            buf[idx++] = tmp[--t];
        }
    }
    buf[idx] = '\0';
    message(buf);
}

static void print_ip(uint32_t ip) {
    uint32_t host_order = __builtin_bswap32(ip);
    print_dec((host_order >> 24) & 0xFF);
    message(".");
    print_dec((host_order >> 16) & 0xFF);
    message(".");
    print_dec((host_order >> 8) & 0xFF);
    message(".");
    print_dec(host_order & 0xFF);
}

static void print_time_and_speed(uint64_t bytes, uint64_t ticks, uint64_t hz) {
    message(" in ");
    if (hz == 0) {
        message("?s");
        return;
    }
    uint64_t elapsed_ms = (ticks * 1000) / hz;
    uint64_t sec = elapsed_ms / 1000;
    uint64_t cs = (elapsed_ms % 1000) / 10;
    print_dec(sec);
    message(".");
    if (cs < 10) message("0");
    print_dec(cs);
    message("s (");

    if (ticks == 0) {
        message("--.- KB/s");
    } else {
        uint64_t bps;
        if (bytes <= UINT64_MAX / hz) {
            bps = (bytes * hz) / ticks;
        } else {
            bps = (bytes / ticks) * hz;
        }

        if (bps < 1024) {
            print_dec(bps);
            message(" B/s");
        } else if (bps < 1024 * 1024) {
            uint64_t kb = bps / 1024;
            uint64_t frac = ((bps % 1024) * 100) / 1024;
            print_dec(kb);
            message(".");
            if (frac < 10) message("0");
            print_dec(frac);
            message(" KB/s");
        } else {
            uint64_t mb = bps / (1024 * 1024);
            uint64_t frac = ((bps % (1024 * 1024)) * 100) / (1024 * 1024);
            print_dec(mb);
            message(".");
            if (frac < 10) message("0");
            print_dec(frac);
            message(" MB/s");
        }
    }
    message(")");
}

static void print_usage(void) {
    message("usage: wget [options] <URL>\n"
            "       wget [destination] <URL>\n"
            "options:\n"
            "  -O <file|dir>    write output to <file|dir> ('-' for stdout)\n"
            "  -P <dir>         save files to directory <dir>\n"
            "  -q, --quiet      quiet mode (suppress status messages on stderr)\n"
            "  -s <server-ip>   override DNS server IPv4 address\n"
            "  -h, --help       display this help and exit\n");
}

static long s_write_error;
static size_t s_write_prefix;

static void body_write_error(uint64_t received) {
    message("wget: write error writing body to output: ");
    switch (-s_write_error) {
        case VFS_ENOSPC: message("no space available or filesystem file-size limit reached"); break;
        case VFS_EFBIG: message("filesystem file-size limit reached"); break;
        case VFS_EROFS: message("read-only filesystem"); break;
        case VFS_EIO: message("storage I/O error"); break;
        case VFS_ENOMEM: message("out of memory"); break;
        default: message("write failed"); break;
    }
    message(" (errno "); print_dec((uint64_t)-s_write_error);
    message(", after "); print_dec(received + s_write_prefix);
    message(" bytes)\n");
}

static bool write_all(long fd, const uint8_t *data, size_t len) {
    size_t at = 0;
    s_write_error = 0; s_write_prefix = 0;
    while (at < len) {
        long n = WGET_CALL(SYS_WRITE, fd, (uintptr_t)(data + at), len - at, 0, 0, 0);
        if (n <= 0 || (size_t)n > len - at) {
            s_write_error = n < 0 ? n : -VFS_EIO;
            s_write_prefix = at;
            return false;
        }
        at += (size_t)n;
    }
    return true;
}

static bool is_url_arg(const char *s) {
    if (!s) return false;
    if (s[0] == '/' || (s[0] == '.' && (s[1] == '/' || s[1] == '\0'))) return false;
    if (s[0] == '-' && s[1] == '\0') return false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == ':' && s[i+1] == '/' && s[i+2] == '/') return true;
    }
    return true;
}

static const char *get_anchor_path(void) {
#ifdef TEST_HOST
    const char *env_path = getenv("FORTRESS_ANCHORS_PATH");
    if (env_path && env_path[0] != '\0') {
        return env_path;
    }
#endif
    return WGET_DEFAULT_ANCHOR_PATH;
}

static uint64_t get_now_ticks(void) {
    sysinfo_t info;
    if (WGET_CALL(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0) == 0) {
        return info.uptime_ticks;
    }
    return 0;
}

static uint64_t calc_clamped_deadline(uint64_t budget_ticks, uint64_t cumulative_dl) {
    uint64_t cur = get_now_ticks();
    if (cur >= cumulative_dl) return cur;
    uint64_t phase = cur + budget_ticks;
    return (phase < cumulative_dl) ? phase : cumulative_dl;
}

static void get_dir_and_base(const char *path, char *dir, size_t dir_cap, char *base, size_t base_cap) {
    size_t len = udp_length(path);
    long last_slash = -1;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = (long)i;
    }
    if (last_slash < 0) {
        if (dir_cap > 1) { dir[0] = '.'; dir[1] = '\0'; }
        size_t b_len = (len < base_cap - 1) ? len : base_cap - 1;
        for (size_t i = 0; i < b_len; i++) base[i] = path[i];
        base[b_len] = '\0';
    } else if (last_slash == 0) {
        if (dir_cap > 1) { dir[0] = '/'; dir[1] = '\0'; }
        size_t b_len = (len - 1 < base_cap - 1) ? len - 1 : base_cap - 1;
        for (size_t i = 0; i < b_len; i++) base[i] = path[1 + i];
        base[b_len] = '\0';
    } else {
        size_t d_len = ((size_t)last_slash < dir_cap - 1) ? (size_t)last_slash : dir_cap - 1;
        for (size_t i = 0; i < d_len; i++) dir[i] = path[i];
        dir[d_len] = '\0';
        size_t b_len = (len - (size_t)last_slash - 1 < base_cap - 1) ? len - (size_t)last_slash - 1 : base_cap - 1;
        for (size_t i = 0; i < b_len; i++) base[i] = path[(size_t)last_slash + 1 + i];
        base[b_len] = '\0';
    }
}

static void hex_encode_32(uint32_t val, char out[9]) {
    const char hex_chars[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex_chars[val & 0x0F];
        val >>= 4;
    }
    out[8] = '\0';
}

static bool build_tmp_path(const char *dir, const char *base, const char *rand_hex,
                           char *out, size_t out_cap)
{
    size_t d_len = udp_length(dir);
    size_t b_len = udp_length(base);
    size_t r_len = udp_length(rand_hex);
    bool need_slash = (d_len > 0 && dir[d_len - 1] != '/' && !udp_equal(dir, "."));
    bool is_dot = udp_equal(dir, ".");

    size_t prefix_len = is_dot ? 0 : d_len + (need_slash ? 1 : 0);
    size_t total_needed = prefix_len + 6 + b_len + 1 + r_len + 4 + 1;
    if (total_needed > out_cap) return false;

    size_t pos = 0;
    if (!is_dot) {
        for (size_t i = 0; i < d_len; i++) out[pos++] = dir[i];
        if (need_slash) out[pos++] = '/';
    }
    const char *p_w = ".wget.";
    while (*p_w) out[pos++] = *p_w++;
    for (size_t i = 0; i < b_len; i++) out[pos++] = base[i];
    out[pos++] = '.';
    for (size_t i = 0; i < r_len; i++) out[pos++] = rand_hex[i];
    const char *p_t = ".tmp";
    while (*p_t) out[pos++] = *p_t++;
    out[pos] = '\0';
    return true;
}

/* Progress bar state */
static uint64_t s_last_progress_bytes = 0;
static unsigned s_last_progress_pct = 101;
static unsigned s_progress_spin = 0;

static void render_progress(uint64_t current, uint64_t total, bool has_total, bool finish, bool quiet) {
    if (quiet) return;

    if (finish) {
        if (s_last_progress_pct != 101) {
            message("\n");
            s_last_progress_pct = 101;
        }
        return;
    }

    unsigned pct = 0;
    if (has_total && total > 0) {
        pct = (unsigned)((current * 100) / total);
        if (pct > 100) pct = 100;
        if (s_last_progress_pct != 101 && pct == s_last_progress_pct &&
            (current >= s_last_progress_bytes && current - s_last_progress_bytes < 16384)) {
            return;
        }
    } else {
        if (s_last_progress_pct != 101 &&
            (current >= s_last_progress_bytes && current - s_last_progress_bytes < 16384)) {
            return;
        }
    }

    s_last_progress_bytes = current;
    s_last_progress_pct = has_total ? pct : 0;

    char buf[128];
    size_t pos = 0;
    buf[pos++] = '\r';

    if (has_total && total > 0) {
        buf[pos++] = '[';
        int bar_width = 30;
        int filled = (int)((current * (uint64_t)bar_width) / total);
        if (filled > bar_width) filled = bar_width;
        for (int i = 0; i < filled; i++) {
            if (i == filled - 1 && filled < bar_width) {
                buf[pos++] = '>';
            } else {
                buf[pos++] = '=';
            }
        }
        for (int i = filled; i < bar_width; i++) {
            buf[pos++] = ' ';
        }
        buf[pos++] = ']';
        buf[pos++] = ' ';

        if (pct < 10) { buf[pos++] = ' '; buf[pos++] = ' '; }
        else if (pct < 100) { buf[pos++] = ' '; }
        char pstr[8];
        int plen = 0;
        unsigned tp = pct;
        if (tp == 0) pstr[plen++] = '0';
        else {
            char tbuf[8]; int tlen = 0;
            while (tp > 0) { tbuf[tlen++] = (char)('0' + (tp % 10)); tp /= 10; }
            while (tlen > 0) pstr[plen++] = tbuf[--tlen];
        }
        for (int i = 0; i < plen; i++) buf[pos++] = pstr[i];
        buf[pos++] = '%';
        buf[pos++] = ' ';

        char cstr[32]; int clen = 0;
        uint64_t tc = current;
        if (tc == 0) cstr[clen++] = '0';
        else {
            char tbuf[32]; int tlen = 0;
            while (tc > 0) { tbuf[tlen++] = (char)('0' + (tc % 10)); tc /= 10; }
            while (tlen > 0) cstr[clen++] = tbuf[--tlen];
        }
        for (int i = 0; i < clen; i++) buf[pos++] = cstr[i];
        buf[pos++] = '/';
        char totstr[32]; int totlen = 0;
        uint64_t tt = total;
        if (tt == 0) totstr[totlen++] = '0';
        else {
            char tbuf[32]; int tlen = 0;
            while (tt > 0) { tbuf[tlen++] = (char)('0' + (tt % 10)); tt /= 10; }
            while (tlen > 0) totstr[totlen++] = tbuf[--tlen];
        }
        for (int i = 0; i < totlen; i++) buf[pos++] = totstr[i];
    } else {
        const char *spinner[] = {"[<=>   ]", "[ <= > ]", "[  <=> ]", "[   <=>]"};
        const char *sp = spinner[s_progress_spin++ % 4];
        while (*sp) buf[pos++] = *sp++;
        buf[pos++] = ' ';
        char cstr[32]; int clen = 0;
        uint64_t tc = current;
        if (tc == 0) cstr[clen++] = '0';
        else {
            char tbuf[32]; int tlen = 0;
            while (tc > 0) { tbuf[tlen++] = (char)('0' + (tc % 10)); tc /= 10; }
            while (tlen > 0) cstr[clen++] = tbuf[--tlen];
        }
        for (int i = 0; i < clen; i++) buf[pos++] = cstr[i];
        const char *suf = " bytes";
        while (*suf) buf[pos++] = *suf++;
    }

    buf[pos] = '\0';
    message(buf);
}

static bool stream_chunk_bytes(long out_fd, const uint8_t *in, size_t in_len,
                               uint64_t *total_received, uint64_t *cumulative_bytes,
                               bool quiet)
{
    size_t in_consumed = 0;
    while (in_consumed < in_len && !s_chunk_dec.complete) {
        size_t chunk_consumed = 0;
        size_t chunk_written = 0;
        int dres = wget_decode_chunked(&s_chunk_dec, in + in_consumed, in_len - in_consumed,
                                      &chunk_consumed, s_chunk_buf, sizeof(s_chunk_buf),
                                      &chunk_written);
        if (dres != WGET_OK && dres != WGET_NEED_MORE_DATA) {
            message("wget: malformed chunked transfer encoding framing\n");
            return false;
        }
        if (chunk_written > 0) {
            *total_received += chunk_written;
            *cumulative_bytes += chunk_written;
            if (*cumulative_bytes > WGET_MAX_CUMULATIVE_PAYLOAD) {
                message("wget: total response payload exceeded 10 MiB limit; aborting\n");
                return false;
            }
            if (!write_all(out_fd, s_chunk_buf, chunk_written)) {
                body_write_error(*total_received);
                return false;
            }
            render_progress(*total_received, 0, false, false, quiet);
        }
        in_consumed += chunk_consumed;
        if (dres == WGET_NEED_MORE_DATA && chunk_consumed == 0) {
            break;
        }
    }
    return true;
}

int wget_run(const char *raw_url, const char *custom_out, bool quiet,
             uint32_t dns_override, bool has_dns_override);

int wget_main(int argc, char **argv) {
    const char *raw_url = NULL;
    const char *custom_out = NULL;
    uint32_t dns_override = 0;
    bool has_dns_override = false;
    bool quiet = false;
    const char *pos_args[4];
    int pos_count = 0;

    for (int i = 1; i < argc; i++) {
        if (udp_equal(argv[i], "-h") || udp_equal(argv[i], "--help")) {
            print_usage();
            return 0;
        } else if (udp_equal(argv[i], "-q") || udp_equal(argv[i], "--quiet")) {
            quiet = true;
        } else if (udp_equal(argv[i], "-O")) {
            if (i + 1 >= argc) {
                message("wget: option -O requires an argument\n");
                print_usage();
                return 1;
            }
            custom_out = argv[++i];
        } else if (udp_equal(argv[i], "-P")) {
            if (i + 1 >= argc) {
                message("wget: option -P requires a directory\n");
                print_usage();
                return 1;
            }
            custom_out = argv[++i];
        } else if (udp_equal(argv[i], "-s")) {
            if (i + 1 >= argc || !udp_ip(argv[i + 1], &dns_override)) {
                message("wget: option -s requires a valid IPv4 address\n");
                print_usage();
                return 1;
            }
            has_dns_override = true;
            i++;
        } else if (argv[i][0] == '-') {
            message("wget: unrecognized option '");
            message(argv[i]);
            message("'\n");
            print_usage();
            return 1;
        } else {
            if (pos_count < 4) {
                pos_args[pos_count++] = argv[i];
            } else {
                message("wget: too many arguments\n");
                print_usage();
                return 1;
            }
        }
    }

    if (pos_count == 0) {
        print_usage();
        return 1;
    } else if (pos_count == 1) {
        raw_url = pos_args[0];
    } else if (pos_count == 2) {
        if (is_url_arg(pos_args[0]) && !is_url_arg(pos_args[1])) {
            raw_url = pos_args[0];
            if (!custom_out) custom_out = pos_args[1];
        } else if (!is_url_arg(pos_args[0]) && is_url_arg(pos_args[1])) {
            raw_url = pos_args[1];
            if (!custom_out) custom_out = pos_args[0];
        } else {
            raw_url = pos_args[0];
            if (!custom_out) custom_out = pos_args[1];
        }
    } else {
        message("wget: too many arguments\n");
        print_usage();
        return 1;
    }

    return wget_run(raw_url, custom_out, quiet, dns_override, has_dns_override);
}

int wget_run(const char *raw_url, const char *custom_out, bool quiet,
             uint32_t dns_override, bool has_dns_override) {
    if (!raw_url) {
        print_usage();
        return 1;
    }

    int parse_status = wget_parse_url(raw_url, &s_current_url);
    if (parse_status != WGET_OK) {
        message("wget: ");
        message(wget_strerror(parse_status));
        message("\n");
        return 1;
    }

    /* Initialize Trust Store on demand for HTTPS */
    if (s_current_url.is_https && !s_trust_store_loaded) {
        int lres = fortress_tls_load_anchors(get_anchor_path(), &s_trust_store);
        if (lres < 0) {
            message("wget: trust store initialization failed: ");
            message(get_anchor_path());
            message(" (");
            message(fortress_tls_strerror(lres));
            message(")\n");
            return 1;
        }
        s_trust_store_loaded = true;
    }

    /* Determine output destination path */
    if (custom_out) {
        bool is_dir = false;
        size_t out_len = udp_length(custom_out);
        if (out_len > 0 && custom_out[out_len - 1] == '/') {
            is_dir = true;
        } else if (udp_equal(custom_out, "/mnt")) {
            is_dir = true;
        } else if (out_len > 0 && !(out_len == 1 && custom_out[0] == '-')) {
            vfs_stat_t st;
            if (WGET_CALL(SYS_STAT, (uintptr_t)custom_out, (uintptr_t)&st, 0, 0, 0, 0) == 0 &&
                st.type == VFS_DIRECTORY) {
                is_dir = true;
            }
        }

        if (is_dir) {
            size_t fn_len = udp_length(s_current_url.filename);
            size_t need_slash = (out_len > 0 && custom_out[out_len - 1] == '/') ? 0 : 1;
            if (out_len + need_slash + fn_len >= sizeof(s_out_path)) {
                message("wget: output filename too long\n");
                return 1;
            }
            size_t pos = 0;
            for (size_t k = 0; k < out_len; k++) s_out_path[pos++] = custom_out[k];
            if (need_slash) s_out_path[pos++] = '/';
            for (size_t k = 0; k <= fn_len; k++) s_out_path[pos++] = s_current_url.filename[k];
        } else {
            if (out_len >= sizeof(s_out_path)) {
                message("wget: output filename too long\n");
                return 1;
            }
            for (size_t k = 0; k <= out_len; k++) s_out_path[k] = custom_out[k];
        }
    } else {
        size_t fn_len = udp_length(s_current_url.filename);
        for (size_t k = 0; k <= fn_len; k++) s_out_path[k] = s_current_url.filename[k];
    }

    /* Session Deadlines and Cumulative Limits (§3.2) */
    WGET_CALL(SYS_SYSINFO, (uintptr_t)&s_start_sysinfo, 0, 0, 0, 0, 0);
    uint64_t tick_hz = s_start_sysinfo.tick_hz ? s_start_sysinfo.tick_hz : 1000;
    uint64_t cumulative_deadline = s_start_sysinfo.uptime_ticks + (60 * tick_hz);
    uint64_t cumulative_payload = 0;

    long sock = -1;
    long out_fd = -1;
    int redirect_hops = 0;
    s_tmp_path[0] = '\0';

    /* Safe exclusive temporary-file creation (§5.3) */
    if (s_out_path[0] == '-' && s_out_path[1] == '\0') {
        out_fd = 1; /* stdout */
        s_tmp_path[0] = '\0';
    } else {
        char dir_buf[WGET_MAX_FILENAME_LEN];
        char base_buf[WGET_MAX_FILENAME_LEN];
        get_dir_and_base(s_out_path, dir_buf, sizeof(dir_buf), base_buf, sizeof(base_buf));

        for (int attempt = 0; attempt < 10; attempt++) {
            uint32_t rnd = 0;
            long gr = WGET_CALL(SYS_GETRANDOM, (uintptr_t)&rnd, sizeof(rnd), 0, 0, 0, 0);
            if (gr != (long)sizeof(rnd)) {
                rnd = (uint32_t)get_now_ticks() ^ (uint32_t)(attempt * 1103515245U + 12345U);
            }
            char rhex[9];
            hex_encode_32(rnd, rhex);
            if (!build_tmp_path(dir_buf, base_buf, rhex, s_tmp_path, sizeof(s_tmp_path))) {
                message("wget: temporary file path too long\n");
                goto failure;
            }

            out_fd = WGET_CALL(SYS_OPEN, (uintptr_t)s_tmp_path,
                               VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_NOFOLLOW, 0644, 0, 0, 0);
            if (out_fd >= 0) break;
            if (out_fd != -VFS_EEXIST && out_fd != -17) {
                /* Non-collision error */
                break;
            }
        }

        if (out_fd < 0) {
            message("wget: failed to create exclusive temporary file for '");
            message(s_out_path);
            message("'\n");
            goto failure;
        }
    }

redirect_loop:
    if (redirect_hops > WGET_MAX_REDIRECTS) {
        message("wget: 5 redirects exceeded; stopping\n");
        goto failure;
    }

    /* 1. Resolve host */
    uint32_t target_ip = 0;
    if (!udp_ip(s_current_url.host, &target_ip)) {
        if (!quiet) {
            message("Resolving ");
            message(s_current_url.host);
            message("...\n");
        }
        size_t host_len = udp_length(s_current_url.host);
        dns_options_t dns_opts = {.server_ipv4 = has_dns_override ? dns_override : 0};
        dns_context_init(&s_dns_context);
        int dns_status = dns_resolve_ipv4(&s_dns_context, &dns_opts, s_current_url.host, host_len, &s_dns_result);
        if (dns_status != 0) {
            if (!has_dns_override && dns_servers_configured(&s_dns_context) == 0) {
                message("wget: no DNS server specified (-s) and none found in network.conf\n");
                goto failure;
            }
            message("wget: host resolution failed: ");
            message(dns_status_name(dns_status));
            message("\n");
            goto failure;
        }
        target_ip = s_dns_result.addresses[0];
    }

    /* 2. Connect via TCP */
    if (!quiet) {
        message("Connecting to ");
        print_ip(target_ip);
        message(":");
        print_dec(s_current_url.port);
        message("...\n");
    }

    sock = -1;
    for (int retry = 0; retry < 50; retry++) {
        sock = WGET_CALL(SYS_SOCKET, NET_AF_INET, NET_SOCK_STREAM | NET_SOCK_CLOEXEC, 6, 0, 0, 0);
        if (sock >= 0) break;
        for (volatile int spin = 0; spin < 500000; spin++) {}
    }
    if (sock < 0) {
        message("wget: socket creation failed\n");
        goto failure;
    }

    net_sockaddr_in_t saddr = {
        .family = NET_AF_INET,
        .port = __builtin_bswap16(s_current_url.port),
        .address = target_ip
    };
    uint64_t conn_deadline = calc_clamped_deadline(10 * tick_hz, cumulative_deadline);
    long conn_res = WGET_CALL(SYS_CONNECT_UNTIL, sock, (uintptr_t)&saddr, sizeof(saddr), conn_deadline, 0, 0);
    if (conn_res < 0) {
        message("wget: connection to server failed (error ");
        print_dec((uint64_t)(-conn_res));
        if (conn_res == -21) {
            message(": TCP reboot quiet time in progress; retry shortly");
        }
        message(")\n");
        goto failure;
    }

    /* 3. TLS Handshake if HTTPS */
    if (s_current_url.is_https) {
        if (!quiet) {
            message("Initiating TLS handshake with ");
            message(s_current_url.host);
            message("...\n");
        }
        int ires = fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count);
        if (ires < 0) {
            message("wget: TLS client init failed: ");
            message(fortress_tls_strerror(ires));
            message("\n");
            goto failure;
        }
        uint64_t hs_deadline = calc_clamped_deadline(15 * tick_hz, cumulative_deadline);
        int tls_status = fortress_tls_connect(&s_tls_session, (int)sock,
                                              s_current_url.host, hs_deadline);
        if (tls_status < 0) {
            message("wget: TLS handshake failed: ");
            if (s_tls_session.br_last_error != 0) {
                message(fortress_tls_strerror((int)s_tls_session.br_last_error));
            } else {
                message(fortress_tls_strerror(tls_status));
            }
            message("\n");
            goto failure;
        }
    }

    if (!quiet) {
        message("connected.\nHTTP request sent, awaiting response... ");
    }

    /* 4. Send HTTP Request */
    size_t req_len = 0;
    const char *p1 = "GET ";
    while (*p1) s_request_buf[req_len++] = *p1++;
    const char *p_path = s_current_url.path;
    while (*p_path) s_request_buf[req_len++] = *p_path++;
    const char *p2 = " HTTP/1.0\r\nHost: ";
    while (*p2) s_request_buf[req_len++] = *p2++;
    const char *p_host = s_current_url.host;
    while (*p_host) s_request_buf[req_len++] = *p_host++;

    bool is_default_port = s_current_url.is_https ? (s_current_url.port == 443) : (s_current_url.port == 80);
    if (!is_default_port) {
        s_request_buf[req_len++] = ':';
        unsigned pt = s_current_url.port;
        int t = 0;
        char tmp[8];
        while (pt > 0) { tmp[t++] = (char)('0' + (pt % 10)); pt /= 10; }
        while (t > 0) s_request_buf[req_len++] = tmp[--t];
    }
    const char *p3 = "\r\nUser-Agent: FortressOS-Wget/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    while (*p3) s_request_buf[req_len++] = *p3++;

    uint64_t wr_deadline = calc_clamped_deadline(10 * tick_hz, cumulative_deadline);
    if (s_current_url.is_https) {
        int wr_res = fortress_tls_write_all(&s_tls_session, (const uint8_t *)s_request_buf, req_len, wr_deadline);
        if (wr_res < 0) {
            message("wget: failed to write HTTPS request\n");
            goto failure;
        }
        int fl_res = fortress_tls_flush(&s_tls_session, wr_deadline);
        if (fl_res < 0) {
            message("wget: failed to flush HTTPS request\n");
            goto failure;
        }
    } else {
        if (!write_all(sock, (const uint8_t *)s_request_buf, req_len)) {
            message("wget: failed to send HTTP request\n");
            goto failure;
        }
        WGET_CALL(SYS_SHUTDOWN, sock, NET_SHUT_WR, 0, 0, 0, 0);
    }

    /* 5. Receive Response Headers */
    size_t total_hdr_read = 0;
    size_t header_len = 0;
    for (;;) {
        uint64_t rd_deadline = calc_clamped_deadline(30 * tick_hz, cumulative_deadline);
        long n;
        if (s_current_url.is_https) {
            int tn = fortress_tls_read(&s_tls_session, s_header_buf + total_hdr_read,
                                       sizeof(s_header_buf) - 1 - total_hdr_read, rd_deadline);
            n = (long)tn;
        } else {
            n = WGET_CALL(SYS_RECV_UNTIL, sock, (uintptr_t)(s_header_buf + total_hdr_read),
                          sizeof(s_header_buf) - 1 - total_hdr_read, 0, rd_deadline, 0);
        }
        if (n < 0) {
            message("wget: read error while reading response headers: ");
            if (s_current_url.is_https) {
                message(fortress_tls_strerror((int)n));
            }
            message("\n");
            goto failure;
        }
        if (n == 0) {
            message("wget: connection closed before headers completed\n");
            goto failure;
        }
        total_hdr_read += (size_t)n;
        s_header_buf[total_hdr_read] = '\0';

        int check = wget_find_header_end(s_header_buf, total_hdr_read, &header_len);
        if (check == WGET_OK) {
            break;
        } else if (check == WGET_ERR_HEADER_OVERFLOW) {
            message("wget: response headers exceed 8192 bytes limit\n");
            goto failure;
        }
    }

    if (wget_parse_response_headers(s_header_buf, header_len, &s_response) != WGET_OK) {
        message("wget: malformed HTTP response headers\n");
        goto failure;
    }

    if (!quiet) {
        print_dec((uint64_t)s_response.status_code);
        message(" ");
        message(s_response.status_text);
        message("\n");
    }

    /* 6. Handle Redirects (301, 302, 303, 307, 308) */
    if (s_response.status_code == 301 || s_response.status_code == 302 ||
        s_response.status_code == 303 ||
        s_response.status_code == 307 || s_response.status_code == 308) {
        if (s_response.location[0] == '\0') {
            message("wget: redirect status received without Location header\n");
            goto failure;
        }
        if (!quiet) {
            message("Location: ");
            message(s_response.location);
            message(" [following]\n");
        }

        /* Cumulative Accounting across Redirects (§3.2) */
        size_t redir_body_bytes = (total_hdr_read > header_len) ? (total_hdr_read - header_len) : 0;
        cumulative_payload += redir_body_bytes;
        if (cumulative_payload > WGET_MAX_CUMULATIVE_PAYLOAD) {
            message("wget: total response payload exceeded 10 MiB limit; aborting\n");
            goto failure;
        }

        if (s_current_url.is_https) {
            uint64_t close_dl = calc_clamped_deadline(3 * tick_hz, cumulative_deadline);
            fortress_tls_close(&s_tls_session, close_dl);
        }
        WGET_CALL(SYS_CLOSE, sock, 0, 0, 0, 0, 0);
        sock = -1;

        int rres = wget_resolve_redirect(&s_current_url, s_response.location, &s_redirect_url);
        if (rres != WGET_OK) {
            message("wget: redirect error: ");
            message(wget_strerror(rres));
            message("\n");
            goto failure;
        }
        s_current_url = s_redirect_url;
        redirect_hops++;
        goto redirect_loop;
    }

    /* 7. Verify Status Code (§5.2) */
    if (s_response.status_code == 206) {
        message("wget: server returned HTTP 206 Partial Content (range not requested); aborting\n");
        goto failure;
    }
    if (s_response.status_code != 200) {
        message("wget: server returned error: HTTP ");
        print_dec((uint64_t)s_response.status_code);
        message(" ");
        message(s_response.status_text);
        message("\n");
        goto failure;
    }

    if (!quiet) {
        if (s_response.has_content_length) {
            message("Length: ");
            print_dec(s_response.content_length);
            message(" bytes\n");
        }
        if (out_fd != 1) {
            message("Saving to: '");
            message(s_out_path);
            message("'\n");
        }
    }

    /* 8. Stream Response Body */
    uint64_t total_received = 0;
    s_last_progress_bytes = 0;
    s_last_progress_pct = 101;
    s_progress_spin = 0;
    if (s_response.is_chunked) {
        wget_chunked_decoder_init(&s_chunk_dec);
    }
    render_progress(0, s_response.content_length, s_response.has_content_length, false, quiet);

    /* Process initial body bytes already read into s_header_buf */
    size_t initial_body_bytes = (total_hdr_read > header_len) ? (total_hdr_read - header_len) : 0;
    if (initial_body_bytes > 0) {
        if (s_response.is_chunked) {
            if (!stream_chunk_bytes(out_fd, s_header_buf + header_len, initial_body_bytes,
                                   &total_received, &cumulative_payload, quiet)) {
                goto failure;
            }
        } else {
            total_received += initial_body_bytes;
            cumulative_payload += initial_body_bytes;
            if (cumulative_payload > WGET_MAX_CUMULATIVE_PAYLOAD) {
                message("wget: total response payload exceeded 10 MiB limit; aborting\n");
                goto failure;
            }
            if (!write_all(out_fd, s_header_buf + header_len, initial_body_bytes)) {
                body_write_error(total_received);
                goto failure;
            }
            render_progress(total_received, s_response.content_length, s_response.has_content_length, false, quiet);
        }
    }

    /* Stream until completion or EOF */
    while (!s_response.is_chunked || !s_chunk_dec.complete) {
        if (!s_response.is_chunked && s_response.has_content_length &&
            total_received >= s_response.content_length) {
            break;
        }

        uint64_t rd_deadline = calc_clamped_deadline(30 * tick_hz, cumulative_deadline);
        long n;
        if (s_current_url.is_https) {
            int tn = fortress_tls_read(&s_tls_session, s_io_buf, sizeof(s_io_buf), rd_deadline);
            n = (long)tn;
        } else {
            n = WGET_CALL(SYS_RECV_UNTIL, sock, (uintptr_t)s_io_buf, sizeof(s_io_buf), 0, rd_deadline, 0);
        }

        if (n < 0) {
            message("wget: read error during body transfer: ");
            if (s_current_url.is_https) {
                message(fortress_tls_strerror((int)n));
            }
            message("\n");
            goto failure;
        }
        if (n == 0) {
            /* EOF reached */
            if (s_response.is_chunked && !s_chunk_dec.complete) {
                message("wget: chunked stream truncated before terminal chunk\n");
                goto failure;
            }
            if (s_response.has_content_length && total_received < s_response.content_length) {
                message("wget: read error: expected ");
                print_dec(s_response.content_length);
                message(" bytes, got ");
                print_dec(total_received);
                message(" (content truncated before expected Content-Length)\n");
                goto failure;
            }
            break;
        }

        if (s_response.is_chunked) {
            if (!stream_chunk_bytes(out_fd, s_io_buf, (size_t)n, &total_received, &cumulative_payload, quiet)) {
                goto failure;
            }
        } else {
            total_received += (size_t)n;
            cumulative_payload += (size_t)n;
            if (cumulative_payload > WGET_MAX_CUMULATIVE_PAYLOAD) {
                message("wget: total response payload exceeded 10 MiB limit; aborting\n");
                goto failure;
            }
            if (!write_all(out_fd, s_io_buf, (size_t)n)) {
                body_write_error(total_received);
                goto failure;
            }
            render_progress(total_received, s_response.content_length, s_response.has_content_length, false, quiet);
        }
    }

    render_progress(total_received, s_response.content_length, s_response.has_content_length, false, quiet);
    render_progress(total_received, s_response.content_length, s_response.has_content_length, true, quiet);

    /* 9. Content-Length and Framing Verification (§5.2) */
    if (s_response.has_content_length) {
        if (wget_validate_content_length(s_response.content_length, total_received, true) != WGET_OK) {
            message("wget: read error: Content-Length mismatch\n");
            goto failure;
        }
    }
    if (s_response.is_chunked && !s_chunk_dec.complete) {
        message("wget: read error: incomplete chunked transfer\n");
        goto failure;
    }

    /* 10. Mandatory Authenticated close_notify Teardown for HTTPS (§3.2, §5.2) */
    if (s_current_url.is_https) {
        uint64_t close_dl = calc_clamped_deadline(3 * tick_hz, cumulative_deadline);
        int cres = fortress_tls_close(&s_tls_session, close_dl);
        if (cres < 0) {
            message("wget: TLS close teardown failed: ");
            message(fortress_tls_strerror(cres));
            message("\n");
            goto failure;
        }
    }

    /* 11. Atomic Commit via Rename (§5.3) */
    if (out_fd >= 0 && out_fd != 1) {
        WGET_CALL(SYS_CLOSE, out_fd, 0, 0, 0, 0, 0);
        out_fd = -1;
        long ren_res = WGET_CALL(SYS_RENAME, (uintptr_t)s_tmp_path, (uintptr_t)s_out_path, 0, 0, 0, 0);
        if (ren_res < 0) {
            message("wget: failed to commit download via atomic rename\n");
            goto failure;
        }
        s_tmp_path[0] = '\0'; /* Committed! */
    }

    WGET_CALL(SYS_SYSINFO, (uintptr_t)&s_end_sysinfo, 0, 0, 0, 0, 0);

    /* Clean close of socket */
    WGET_CALL(SYS_CLOSE, sock, 0, 0, 0, 0, 0);
    sock = -1;

    if (!quiet) {
        message("'");
        message(s_out_path);
        message("' saved [");
        print_dec(total_received);
        if (s_response.has_content_length) {
            message("/");
            print_dec(s_response.content_length);
        }
        message("]");
        if (s_end_sysinfo.tick_hz > 0 && s_start_sysinfo.tick_hz > 0) {
            uint64_t ticks = (s_end_sysinfo.uptime_ticks >= s_start_sysinfo.uptime_ticks) ?
                             (s_end_sysinfo.uptime_ticks - s_start_sysinfo.uptime_ticks) : 0;
            print_time_and_speed(total_received, ticks, s_end_sysinfo.tick_hz);
        }
        message("\n");
    }

    return 0;

failure:
    render_progress(0, 0, false, true, quiet);
    if (sock >= 0) {
        WGET_CALL(SYS_CLOSE, sock, 0, 0, 0, 0, 0);
        sock = -1;
    }
    if (out_fd >= 0 && out_fd != 1) {
        WGET_CALL(SYS_CLOSE, out_fd, 0, 0, 0, 0, 0);
        out_fd = -1;
    }
    if (s_tmp_path[0] != '\0') {
        WGET_CALL(SYS_UNLINK, (uintptr_t)s_tmp_path, 0, 0, 0, 0, 0);
        s_tmp_path[0] = '\0';
    }
    return 1;
}
