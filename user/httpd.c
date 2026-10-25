#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "poll.h"
#include "udp_common.h"

#ifdef TEST_HOST
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#define HTTPD_MAX_CLIENTS      8
#define HTTPD_REQ_BUF_SIZE     2048
#define HTTPD_IO_BUF_SIZE      2048
#define HTTPD_HEADER_BUF_SIZE  512
#define HTTPD_PATH_MAX         512
#define HTTPD_TIMEOUT_TICKS    (10 * 1000) /* 10s default in ms */

typedef enum {
    CLIENT_STATE_UNUSED = 0,
    CLIENT_STATE_READING_REQ,
    CLIENT_STATE_SENDING_HEADER,
    CLIENT_STATE_SENDING_BODY,
    CLIENT_STATE_CLOSING
} client_state_t;

typedef struct {
    client_state_t state;
    int            sock;
    int            file_fd;
    uint64_t       last_activity_ms;

    /* Request buffer */
    char           req_buf[HTTPD_REQ_BUF_SIZE + 1];
    size_t         req_len;

    /* Response header */
    char           resp_header[HTTPD_HEADER_BUF_SIZE + 1];
    size_t         resp_header_len;
    size_t         resp_header_sent;

    /* Body streaming */
    uint8_t        file_buf[HTTPD_IO_BUF_SIZE];
    size_t         file_buf_len;
    size_t         file_buf_pos;
    uint64_t       bytes_remaining;
    bool           is_head;
    bool           is_error_body;
} http_client_t;

/* Static BSS storage strictly adhering to Gate G1 (zero dynamic heap allocations) */
static http_client_t s_clients[HTTPD_MAX_CLIENTS];
static struct pollfd s_pfds[1 + HTTPD_MAX_CLIENTS];
static char          s_docroot[HTTPD_PATH_MAX];
static char          s_clean_uri[HTTPD_PATH_MAX];
static char          s_uri[HTTPD_PATH_MAX];
static char          s_fullpath[HTTPD_PATH_MAX];
static vfs_stat_t    s_stat_buf;
static uint16_t      s_port = 8080;
static bool          s_verbose = false;

#ifndef HTTPD_SYSCALL
#define HTTPD_SYSCALL udp_call
#endif

/* Message output helper to stderr */
static void httpd_msg(const char *s) {
    size_t n = udp_length(s);
    size_t at = 0;
    while (at < n) {
        long w = HTTPD_SYSCALL(SYS_WRITE, 2, (uintptr_t)(s + at), n - at, 0, 0, 0);
        if (w <= 0) break;
        at += (size_t)w;
    }
}

static void httpd_print_u64(uint64_t val) {
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
        while (t > 0) buf[idx++] = tmp[--t];
    }
    buf[idx] = '\0';
    httpd_msg(buf);
}

static uint64_t get_now_ms(void) {
    sysinfo_t info;
    if (HTTPD_SYSCALL(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0) == 0 && info.tick_hz > 0) {
        return (info.uptime_ticks * 1000) / info.tick_hz;
    }
    return 0;
}

static size_t format_dec(char *buf, uint64_t val) {
    char tmp[32];
    int t = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (val > 0) {
        tmp[t++] = (char)('0' + (val % 10));
        val /= 10;
    }
    size_t out = 0;
    while (t > 0) {
        buf[out++] = tmp[--t];
    }
    buf[out] = '\0';
    return out;
}

/* =========================================================================
 * MIME Type Resolution
 * ========================================================================= */

static bool str_ends_with(const char *s, const char *suffix) {
    size_t slen = udp_length(s);
    size_t suflen = udp_length(suffix);
    if (slen < suflen) return false;
    const char *p = s + slen - suflen;
    while (*suffix) {
        char c1 = *p++;
        char c2 = *suffix++;
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
        if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
        if (c1 != c2) return false;
    }
    return true;
}

static const char *get_mime_type(const char *path) {
    if (str_ends_with(path, ".html") || str_ends_with(path, ".htm"))
        return "text/html; charset=utf-8";
    if (str_ends_with(path, ".txt") || str_ends_with(path, ".c") ||
        str_ends_with(path, ".h") || str_ends_with(path, ".log") ||
        str_ends_with(path, ".md") || str_ends_with(path, ".conf") ||
        str_ends_with(path, "motd"))
        return "text/plain; charset=utf-8";
    if (str_ends_with(path, ".css"))
        return "text/css";
    if (str_ends_with(path, ".js"))
        return "application/javascript";
    if (str_ends_with(path, ".json"))
        return "application/json";
    if (str_ends_with(path, ".png"))
        return "image/png";
    if (str_ends_with(path, ".jpg") || str_ends_with(path, ".jpeg"))
        return "image/jpeg";
    if (str_ends_with(path, ".gif"))
        return "image/gif";
    if (str_ends_with(path, ".svg"))
        return "image/svg+xml";
    if (str_ends_with(path, ".ico"))
        return "image/x-icon";
    return "application/octet-stream";
}

/* =========================================================================
 * Path Sanitization (Directory Traversal Mitigation)
 * ========================================================================= */

static bool sanitize_path(const char *uri, char *out_path, size_t out_cap) {
    if (!uri || uri[0] != '/') return false;

    /* Check for directory traversal sequence: ".." */
    for (size_t i = 0; uri[i]; i++) {
        if (uri[i] == '.' && uri[i+1] == '.') {
            if (i > 0 && uri[i-1] != '/') continue;
            if (uri[i+2] == '/' || uri[i+2] == '?' || uri[i+2] == '#' || uri[i+2] == '\0') {
                return false; /* Path traversal rejected! */
            }
        }
        if (uri[i] == '\0') break;
    }

    /* Extract path component before query (?) or fragment (#) */
    size_t ulen = 0;
    while (uri[ulen] && uri[ulen] != '?' && uri[ulen] != '#' && ulen < sizeof(s_clean_uri) - 1) {
        s_clean_uri[ulen] = uri[ulen];
        ulen++;
    }
    s_clean_uri[ulen] = '\0';

    /* Build full filesystem path: docroot + s_clean_uri */
    size_t dlen = udp_length(s_docroot);
    size_t pos = 0;
    for (size_t i = 0; i < dlen && pos < out_cap - 1; i++) {
        out_path[pos++] = s_docroot[i];
    }
    /* Avoid double slash */
    if (pos > 0 && out_path[pos-1] == '/' && s_clean_uri[0] == '/') {
        s_clean_uri[0] = '\0';
        size_t c_idx = 1;
        while (s_clean_uri[c_idx] && pos < out_cap - 1) {
            out_path[pos++] = s_clean_uri[c_idx++];
        }
    } else {
        size_t c_idx = 0;
        while (s_clean_uri[c_idx] && pos < out_cap - 1) {
            out_path[pos++] = s_clean_uri[c_idx++];
        }
    }
    out_path[pos] = '\0';

    /* If path ends in slash or is a directory, append index.html */
    if (pos > 0 && out_path[pos-1] == '/') {
        const char *idx = "index.html";
        size_t ilen = udp_length(idx);
        if (pos + ilen < out_cap) {
            for (size_t i = 0; i < ilen; i++) out_path[pos++] = idx[i];
            out_path[pos] = '\0';
        }
    }

    return true;
}

/* =========================================================================
 * Response Formatting
 * ========================================================================= */

static void set_error_response(http_client_t *c, int status_code, const char *status_text, const char *body) {
    c->file_fd = -1;
    c->is_error_body = true;

    size_t body_len = udp_length(body);
    char len_str[32];
    format_dec(len_str, body_len);

    char code_str[16];
    format_dec(code_str, (uint64_t)status_code);

    size_t pos = 0;
    const char *p1 = "HTTP/1.0 ";
    while (*p1 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p1++;
    const char *p2 = code_str;
    while (*p2 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p2++;
    c->resp_header[pos++] = ' ';
    const char *p3 = status_text;
    while (*p3 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p3++;
    const char *p4 = "\r\nServer: FortressOS-httpd/1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
    while (*p4 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p4++;
    const char *p5 = len_str;
    while (*p5 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p5++;
    const char *p6 = "\r\nConnection: close\r\n\r\n";
    while (*p6 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *p6++;
    c->resp_header[pos] = '\0';
    c->resp_header_len = pos;
    c->resp_header_sent = 0;

    /* Copy error body into file_buf */
    size_t b_copy = (body_len < sizeof(c->file_buf)) ? body_len : sizeof(c->file_buf);
    for (size_t i = 0; i < b_copy; i++) c->file_buf[i] = (uint8_t)body[i];
    c->file_buf_len = b_copy;
    c->file_buf_pos = 0;
    c->bytes_remaining = b_copy;

    c->state = CLIENT_STATE_SENDING_HEADER;
}

static void close_client(http_client_t *c) {
    if (c->file_fd >= 0) {
        HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)c->file_fd, 0, 0, 0, 0, 0);
        c->file_fd = -1;
    }
    if (c->sock >= 0) {
        HTTPD_SYSCALL(SYS_SHUTDOWN, (uintptr_t)c->sock, NET_SHUT_WR, 0, 0, 0, 0);
        HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)c->sock, 0, 0, 0, 0, 0);
        c->sock = -1;
    }
    c->state = CLIENT_STATE_UNUSED;
    c->req_len = 0;
}

/* =========================================================================
 * Request Processing
 * ========================================================================= */

static void handle_request(http_client_t *c) {
    /* Parse request line: METHOD URI HTTP/x.x */
    c->req_buf[c->req_len] = '\0';

    const char *p = c->req_buf;
    while (*p == ' ' || *p == '\t') p++;

    bool is_head = false;
    if (p[0] == 'G' && p[1] == 'E' && p[2] == 'T' && (p[3] == ' ' || p[3] == '\t')) {
        p += 4;
    } else if (p[0] == 'H' && p[1] == 'E' && p[2] == 'A' && p[3] == 'D' && (p[4] == ' ' || p[4] == '\t')) {
        is_head = true;
        p += 5;
    } else {
        set_error_response(c, 405, "Method Not Allowed", "405 Method Not Allowed\n");
        return;
    }
    c->is_head = is_head;

    while (*p == ' ' || *p == '\t') p++;

    /* Extract URI */
    size_t ulen = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && ulen < sizeof(s_uri) - 1) {
        s_uri[ulen++] = *p++;
    }
    s_uri[ulen] = '\0';

    if (ulen == 0) {
        set_error_response(c, 400, "Bad Request", "400 Bad Request\n");
        return;
    }

    /* Sanitize and map to docroot */
    if (!sanitize_path(s_uri, s_fullpath, sizeof(s_fullpath))) {
        set_error_response(c, 403, "Forbidden", "403 Forbidden\n");
        return;
    }

    /* Stat target */
    long sres = HTTPD_SYSCALL(SYS_STAT, (uintptr_t)s_fullpath, (uintptr_t)&s_stat_buf, 0, 0, 0, 0);
    if (sres < 0) {
        set_error_response(c, 404, "Not Found", "404 Not Found\n");
        return;
    }

    if (s_stat_buf.type == VFS_DIRECTORY) {
        /* If directory, try index.html inside */
        size_t flen = udp_length(s_fullpath);
        if (flen + 11 < sizeof(s_fullpath)) {
            if (s_fullpath[flen-1] != '/') s_fullpath[flen++] = '/';
            const char *idx = "index.html";
            for (size_t i = 0; i < 10; i++) s_fullpath[flen++] = idx[i];
            s_fullpath[flen] = '\0';
            sres = HTTPD_SYSCALL(SYS_STAT, (uintptr_t)s_fullpath, (uintptr_t)&s_stat_buf, 0, 0, 0, 0);
        }
        if (sres < 0 || s_stat_buf.type != VFS_FILE) {
            set_error_response(c, 403, "Forbidden", "403 Forbidden\n");
            return;
        }
    }

    if (s_stat_buf.type != VFS_FILE) {
        set_error_response(c, 403, "Forbidden", "403 Forbidden\n");
        return;
    }

    /* Open file */
    long fd = HTTPD_SYSCALL(SYS_OPEN, (uintptr_t)s_fullpath, VFS_O_RDONLY | VFS_O_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) {
        set_error_response(c, 403, "Forbidden", "403 Forbidden\n");
        return;
    }
    c->file_fd = (int)fd;
    c->bytes_remaining = s_stat_buf.size;
    c->file_buf_len = 0;
    c->file_buf_pos = 0;
    c->is_error_body = false;

    /* Build 200 OK header */
    const char *mime = get_mime_type(s_fullpath);
    char len_str[32];
    format_dec(len_str, s_stat_buf.size);

    size_t pos = 0;
    const char *h1 = "HTTP/1.0 200 OK\r\nServer: FortressOS-httpd/1.0\r\nContent-Type: ";
    while (*h1 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *h1++;
    const char *h2 = mime;
    while (*h2 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *h2++;
    const char *h3 = "\r\nContent-Length: ";
    while (*h3 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *h3++;
    const char *h4 = len_str;
    while (*h4 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *h4++;
    const char *h5 = "\r\nConnection: close\r\n\r\n";
    while (*h5 && pos < sizeof(c->resp_header) - 1) c->resp_header[pos++] = *h5++;
    c->resp_header[pos] = '\0';
    c->resp_header_len = pos;
    c->resp_header_sent = 0;

    c->state = CLIENT_STATE_SENDING_HEADER;
}

/* =========================================================================
 * Non-Blocking Multiplexing Loop
 * ========================================================================= */

int httpd_run(uint16_t port, const char *docroot) {
    s_port = port;
    size_t dlen = udp_length(docroot);
    if (dlen >= sizeof(s_docroot)) return 1;
    for (size_t i = 0; i <= dlen; i++) s_docroot[i] = docroot[i];

    /* Clean trailing slash on docroot */
    if (dlen > 1 && s_docroot[dlen-1] == '/') {
        s_docroot[dlen-1] = '\0';
    }

    /* Initialize client table */
    for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
        s_clients[i].state = CLIENT_STATE_UNUSED;
        s_clients[i].sock = -1;
        s_clients[i].file_fd = -1;
    }

    /* 1. Create non-blocking listening socket */
    long listener = HTTPD_SYSCALL(SYS_SOCKET, NET_AF_INET, NET_SOCK_STREAM | NET_SOCK_CLOEXEC, 6, 0, 0, 0);
    if (listener >= 0) {
        (void)HTTPD_SYSCALL(SYS_FCNTL, (uintptr_t)listener, F_SETFL, VFS_O_NONBLOCK, 0, 0, 0);
    }
    if (listener < 0) {
        httpd_msg("httpd: failed to create listening socket\n");
        return 1;
    }

    net_sockaddr_in_t addr = {
        .family = NET_AF_INET,
        .port = __builtin_bswap16(port),
        .address = 0
    };
    long bres = HTTPD_SYSCALL(SYS_BIND, (uintptr_t)listener, (uintptr_t)&addr, sizeof(addr), 0, 0, 0);
    if (bres < 0) {
        httpd_msg("httpd: bind failed on port ");
        httpd_print_u64((uint64_t)port);
        httpd_msg("\n");
        HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)listener, 0, 0, 0, 0, 0);
        return 1;
    }

    long lres = HTTPD_SYSCALL(SYS_LISTEN, (uintptr_t)listener, 4, 0, 0, 0, 0);
    if (lres < 0) {
        httpd_msg("httpd: listen failed\n");
        HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)listener, 0, 0, 0, 0, 0);
        return 1;
    }

    httpd_msg("httpd: listening on port ");
    httpd_print_u64((uint64_t)port);
    httpd_msg(", docroot '");
    httpd_msg(s_docroot);
    httpd_msg("'\n");

    /* 2. Main Non-Blocking Event Multiplexing Loop */
    for (;;) {
        uint64_t now = get_now_ms();

        /* Prepare pollfd array */
        s_pfds[0].fd = (int)listener;
        s_pfds[0].events = POLLIN;
        s_pfds[0].revents = 0;

        int pfd_count = 1;
        for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
            http_client_t *c = &s_clients[i];
            if (c->state == CLIENT_STATE_UNUSED) continue;

            /* Check inactivity timeout (10s) */
            if (now > c->last_activity_ms && (now - c->last_activity_ms > HTTPD_TIMEOUT_TICKS)) {
                close_client(c);
                continue;
            }

            s_pfds[pfd_count].fd = c->sock;
            s_pfds[pfd_count].revents = 0;
            if (c->state == CLIENT_STATE_READING_REQ) {
                s_pfds[pfd_count].events = POLLIN;
            } else if (c->state == CLIENT_STATE_SENDING_HEADER || c->state == CLIENT_STATE_SENDING_BODY) {
                s_pfds[pfd_count].events = POLLOUT;
            } else {
                s_pfds[pfd_count].events = 0;
            }
            pfd_count++;
        }

        int pr = poll(s_pfds, (uint64_t)pfd_count, 500);
        if (pr < 0) {
            break;
        }

        /* 3. Check for new connections on listener */
        if (s_pfds[0].revents & POLLIN) {
            for (;;) {
                net_sockaddr_in_t peer;
                uint32_t peer_len = sizeof(peer);
                long csock = HTTPD_SYSCALL(SYS_ACCEPT, (uintptr_t)listener, (uintptr_t)&peer,
                                           (uintptr_t)&peer_len, NET_SOCK_NONBLOCK | NET_SOCK_CLOEXEC, 0, 0);
                if (csock < 0) {
                    /* EAGAIN / EWOULDBLOCK indicates no more pending connections */
                    break;
                }

                /* Find free client slot */
                int slot = -1;
                for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
                    if (s_clients[i].state == CLIENT_STATE_UNUSED) {
                        slot = i;
                        break;
                    }
                }

                if (slot < 0) {
                    /* Table full: close immediately */
                    const char *busy = "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\n\r\n";
                    (void)HTTPD_SYSCALL(SYS_WRITE, (uintptr_t)csock, (uintptr_t)busy, udp_length(busy), 0, 0, 0);
                    HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)csock, 0, 0, 0, 0, 0);
                } else {
                    http_client_t *c = &s_clients[slot];
                    c->state = CLIENT_STATE_READING_REQ;
                    c->sock = (int)csock;
                    c->file_fd = -1;
                    c->req_len = 0;
                    c->last_activity_ms = now;
                }
            }
        }

        /* 4. Process Client Socket Events */
        int cur_pfd = 1;
        for (int i = 0; i < HTTPD_MAX_CLIENTS; i++) {
            http_client_t *c = &s_clients[i];
            if (c->state == CLIENT_STATE_UNUSED) continue;

            short rev = s_pfds[cur_pfd++].revents;
            if (rev == 0) continue;

            c->last_activity_ms = now;

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                /* Client disconnected or socket error */
                close_client(c);
                continue;
            }

            /* State: Reading Request */
            if (c->state == CLIENT_STATE_READING_REQ && (rev & POLLIN)) {
                size_t space = sizeof(c->req_buf) - 1 - c->req_len;
                if (space == 0) {
                    set_error_response(c, 400, "Bad Request", "400 Bad Request: headers too long\n");
                    continue;
                }

                long r = HTTPD_SYSCALL(SYS_READ, (uintptr_t)c->sock, (uintptr_t)(c->req_buf + c->req_len), space, 0, 0, 0);
                if (r <= 0) {
                    if (r == SYSCALL_EAGAIN || r == -11 || r == -VFS_EAGAIN) {
                        /* EAGAIN: wait for next POLLIN */
                    } else {
                        close_client(c);
                    }
                    continue;
                }

                c->req_len += (size_t)r;
                c->req_buf[c->req_len] = '\0';

                /* Check for end of HTTP request headers (\r\n\r\n or \n\n) */
                bool ready = false;
                for (size_t k = 0; k < c->req_len; k++) {
                    if (c->req_buf[k] == '\n' && k > 0 && c->req_buf[k-1] == '\n') {
                        ready = true;
                        break;
                    }
                    if (k >= 3 && c->req_buf[k] == '\n' && c->req_buf[k-1] == '\r' &&
                        c->req_buf[k-2] == '\n' && c->req_buf[k-3] == '\r') {
                        ready = true;
                        break;
                    }
                }

                if (ready) {
                    handle_request(c);
                }
                continue;
            }

            /* State: Sending Header */
            if (c->state == CLIENT_STATE_SENDING_HEADER && (rev & POLLOUT)) {
                size_t left = c->resp_header_len - c->resp_header_sent;
                long w = HTTPD_SYSCALL(SYS_WRITE, (uintptr_t)c->sock,
                                      (uintptr_t)(c->resp_header + c->resp_header_sent), left, 0, 0, 0);
                if (w < 0) {
                    if (w == SYSCALL_EAGAIN || w == -11 || w == -VFS_EAGAIN) {
                        /* EAGAIN: buffer full, retry on next POLLOUT */
                    } else {
                        close_client(c);
                    }
                    continue;
                }

                c->resp_header_sent += (size_t)w;
                if (c->resp_header_sent >= c->resp_header_len) {
                    if (c->is_head || c->bytes_remaining == 0) {
                        close_client(c);
                    } else {
                        c->state = CLIENT_STATE_SENDING_BODY;
                    }
                }
                continue;
            }

            /* State: Sending Body */
            if (c->state == CLIENT_STATE_SENDING_BODY && (rev & POLLOUT)) {
                /* If buffer empty, read next chunk from file */
                if (c->file_buf_pos >= c->file_buf_len) {
                    if (c->is_error_body) {
                        /* Error body was already buffered, now done */
                        close_client(c);
                        continue;
                    }

                    size_t to_read = sizeof(c->file_buf);
                    if ((uint64_t)to_read > c->bytes_remaining) {
                        to_read = (size_t)c->bytes_remaining;
                    }

                    long r = HTTPD_SYSCALL(SYS_READ, (uintptr_t)c->file_fd, (uintptr_t)c->file_buf, to_read, 0, 0, 0);
                    if (r <= 0) {
                        /* EOF or read error */
                        close_client(c);
                        continue;
                    }
                    c->file_buf_len = (size_t)r;
                    c->file_buf_pos = 0;
                }

                /* Non-blocking write to socket */
                size_t left = c->file_buf_len - c->file_buf_pos;
                long w = HTTPD_SYSCALL(SYS_WRITE, (uintptr_t)c->sock,
                                      (uintptr_t)(c->file_buf + c->file_buf_pos), left, 0, 0, 0);
                if (w < 0) {
                    if (w == SYSCALL_EAGAIN || w == -11 || w == -VFS_EAGAIN) {
                        /* EAGAIN: wait for next POLLOUT */
                    } else {
                        close_client(c);
                    }
                    continue;
                }

                c->file_buf_pos += (size_t)w;
                c->bytes_remaining -= (size_t)w;

                if (c->bytes_remaining == 0 && c->file_buf_pos >= c->file_buf_len) {
                    close_client(c);
                }
                continue;
            }
        }
    }

    HTTPD_SYSCALL(SYS_CLOSE, (uintptr_t)listener, 0, 0, 0, 0, 0);
    return 0;
}

/* =========================================================================
 * CLI Entry Point
 * ========================================================================= */

static void print_usage(void) {
    httpd_msg("usage: httpd [options] [port] [docroot]\n"
              "options:\n"
              "  -p, --port <port>    TCP port to bind (default 8080)\n"
              "  -d, --dir <path>     Document root directory (default .)\n"
              "  -v, --verbose        Enable verbose connection logging\n"
              "  -h, --help           Display this help and exit\n");
}

int httpd_main(int argc, char **argv) {
    uint16_t port = 8080;
    const char *docroot = ".";
    int pos_idx = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (udp_equal(arg, "-h") || udp_equal(arg, "--help")) {
            print_usage();
            return 0;
        }
        if (udp_equal(arg, "-v") || udp_equal(arg, "--verbose")) {
            s_verbose = true;
            continue;
        }
        if (udp_equal(arg, "-p") || udp_equal(arg, "--port")) {
            if (++i >= argc) {
                httpd_msg("httpd: option requires an argument -- 'p'\n");
                return 1;
            }
            unsigned val;
            const char *p = argv[i];
            if (!udp_number(&p, 65535, &val) || *p != '\0' || val == 0) {
                httpd_msg("httpd: invalid port: ");
                httpd_msg(argv[i]);
                httpd_msg("\n");
                return 1;
            }
            port = (uint16_t)val;
            continue;
        }
        if (udp_equal(arg, "-d") || udp_equal(arg, "--dir")) {
            if (++i >= argc) {
                httpd_msg("httpd: option requires an argument -- 'd'\n");
                return 1;
            }
            docroot = argv[i];
            continue;
        }
        if (arg[0] == '-') {
            httpd_msg("httpd: unrecognized option '");
            httpd_msg(arg);
            httpd_msg("'\n");
            print_usage();
            return 1;
        }

        /* Positional argument */
        if (pos_idx == 0) {
            unsigned val;
            const char *p = arg;
            if (udp_number(&p, 65535, &val) && *p == '\0' && val > 0) {
                port = (uint16_t)val;
            } else {
                docroot = arg;
            }
            pos_idx++;
        } else if (pos_idx == 1) {
            docroot = arg;
            pos_idx++;
        } else {
            httpd_msg("httpd: extra operand '");
            httpd_msg(arg);
            httpd_msg("'\n");
            return 1;
        }
    }

    return httpd_run(port, docroot);
}
