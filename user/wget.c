#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "udp_common.h"
#include "dns.h"
#include "dns_codec.h"
#include "netconf.h"
#include "wget_codec.h"

#ifndef WGET_CALL
#define WGET_CALL udp_call
#endif

#ifndef NETCONF_SYSCALL
#define NETCONF_SYSCALL(nr, a, b, c) WGET_CALL(nr, a, b, c, 0, 0, 0)
#endif

/* Static BSS storage strictly adhering to Ring 3 stack budget */
static uint8_t s_io_buf[4096];
static uint8_t s_header_buf[WGET_MAX_HEADER_BYTES + 1];
static wget_url_t s_current_url;
static wget_url_t s_redirect_url;
static wget_response_t s_response;
static dns_context_t s_dns_context;
static dns_result_t s_dns_result;
static char s_out_path[WGET_MAX_FILENAME_LEN];
static char s_request_buf[2048];

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

static void print_usage(void) {
    message("usage: wget [options] <URL>\n"
            "options:\n"
            "  -O <file>        write output to <file> ('-' for stdout)\n"
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

int wget_main(int argc, char **argv) {
    const char *raw_url = NULL;
    const char *custom_out = NULL;
    uint32_t dns_override = 0;
    bool has_dns_override = false;
    bool quiet = false;

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
            if (!raw_url) {
                raw_url = argv[i];
            } else {
                message("wget: multiple URLs specified\n");
                print_usage();
                return 1;
            }
        }
    }

    if (!raw_url) {
        print_usage();
        return 1;
    }

    int parse_status = wget_parse_url(raw_url, &s_current_url);
    if (parse_status != WGET_OK) {
        message("wget: ");
        message(wget_strerror(parse_status));
        message("\n");
        if (parse_status == WGET_ERR_HTTPS) {
            message("wget: note: FortressOS does not support TLS/HTTPS yet; please use http://\n");
        }
        return 1;
    }

    /* Determine output filename */
    if (custom_out) {
        size_t out_len = udp_length(custom_out);
        if (out_len >= sizeof(s_out_path)) {
            message("wget: output filename too long\n");
            return 1;
        }
        for (size_t k = 0; k <= out_len; k++) s_out_path[k] = custom_out[k];
    } else {
        size_t fn_len = udp_length(s_current_url.filename);
        for (size_t k = 0; k <= fn_len; k++) s_out_path[k] = s_current_url.filename[k];
    }

    long sock = -1;
    long out_fd = -1;
    int redirect_hops = 0;

redirect_loop:
    if (redirect_hops > WGET_MAX_REDIRECTS) {
        message("wget: 5 redirects exceeded; stopping\n");
        goto failure;
    }

    /* 1. Resolve host */
    uint32_t target_ip = 0;
    if (!udp_ip(s_current_url.host, &target_ip)) {
        uint32_t dns_server = dns_override;
        if (!has_dns_override) {
            if (netconf_read_dns(0, &dns_server) != 0) {
                message("wget: no DNS server specified (-s) and none found in network.conf\n");
                goto failure;
            }
        }
        if (!quiet) {
            message("Resolving ");
            message(s_current_url.host);
            message("...\n");
        }
        size_t host_len = udp_length(s_current_url.host);
        dns_options_t dns_opts = {.server_ipv4 = dns_server};
        dns_context_init(&s_dns_context);
        int dns_status = dns_resolve_ipv4(&s_dns_context, &dns_opts, s_current_url.host, host_len, &s_dns_result);
        if (dns_status != 0) {
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
    long conn_res = WGET_CALL(SYS_CONNECT, sock, (uintptr_t)&saddr, sizeof(saddr), 0, 0, 0);
    if (conn_res < 0) {
        message("wget: connection to server failed (error ");
        print_dec((uint64_t)(-conn_res));
        if (conn_res == -21) {
            message(": TCP reboot quiet time in progress; retry shortly");
        }
        message(")\n");
        goto failure;
    }

    if (!quiet) {
        message("connected.\nHTTP request sent, awaiting response... ");
    }

    /* 3. Send HTTP Request */
    size_t req_len = 0;
    const char *p1 = "GET ";
    while (*p1) s_request_buf[req_len++] = *p1++;
    const char *p_path = s_current_url.path;
    while (*p_path) s_request_buf[req_len++] = *p_path++;
    const char *p2 = " HTTP/1.0\r\nHost: ";
    while (*p2) s_request_buf[req_len++] = *p2++;
    const char *p_host = s_current_url.host;
    while (*p_host) s_request_buf[req_len++] = *p_host++;
    if (s_current_url.port != 80) {
        s_request_buf[req_len++] = ':';
        unsigned pt = s_current_url.port;
        int t = 0;
        char tmp[8];
        while (pt > 0) { tmp[t++] = (char)('0' + (pt % 10)); pt /= 10; }
        while (t > 0) s_request_buf[req_len++] = tmp[--t];
    }
    const char *p3 = "\r\nUser-Agent: FortressOS-Wget/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    while (*p3) s_request_buf[req_len++] = *p3++;

    if (!write_all(sock, (const uint8_t *)s_request_buf, req_len)) {
        message("wget: failed to send HTTP request\n");
        goto failure;
    }
    WGET_CALL(SYS_SHUTDOWN, sock, NET_SHUT_WR, 0, 0, 0, 0);

    /* 4. Receive Response Headers */
    size_t total_hdr_read = 0;
    size_t header_len = 0;
    for (;;) {
        long n = WGET_CALL(SYS_READ, sock, (uintptr_t)(s_header_buf + total_hdr_read),
                           sizeof(s_header_buf) - 1 - total_hdr_read, 0, 0, 0);
        if (n < 0) {
            message("wget: read error while reading response headers\n");
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

    /* 5. Handle Redirects (301, 302, 307, 308) */
    if (s_response.status_code == 301 || s_response.status_code == 302 ||
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

    /* 6. Verify Status Code */
    if (s_response.status_code < 200 || s_response.status_code >= 300) {
        message("wget: server returned error: HTTP ");
        print_dec((uint64_t)s_response.status_code);
        message(" ");
        message(s_response.status_text);
        message("\n");
        goto failure;
    }

    /* 7. Open Destination */
    if (s_out_path[0] == '-' && s_out_path[1] == '\0') {
        out_fd = 1; /* stdout */
    } else {
        out_fd = WGET_CALL(SYS_OPEN, (uintptr_t)s_out_path,
                           VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644, 0, 0, 0);
        if (out_fd < 0) {
            message("wget: failed to open output file: '");
            message(s_out_path);
            message("'\n");
            goto failure;
        }
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

    /* Write any body bytes already read into s_header_buf */
    size_t initial_body_bytes = total_hdr_read - header_len;
    if (initial_body_bytes > 0) {
        if (!write_all(out_fd, s_header_buf + header_len, initial_body_bytes)) {
            body_write_error(total_received);
            goto failure;
        }
        total_received += (uint64_t)initial_body_bytes;
    }

    /* Stream until server closes connection (recv == 0) */
    for (;;) {
        long n = WGET_CALL(SYS_READ, sock, (uintptr_t)s_io_buf, sizeof(s_io_buf), 0, 0, 0);
        if (n < 0) {
            message("wget: read error during body transfer\n");
            goto failure;
        }
        if (n == 0) {
            /* EOF reached */
            break;
        }
        if (!write_all(out_fd, s_io_buf, (size_t)n)) {
            body_write_error(total_received);
            goto failure;
        }
        total_received += (uint64_t)n;
    }

    /* Clean close of socket and output file */
    WGET_CALL(SYS_CLOSE, sock, 0, 0, 0, 0, 0);
    sock = -1;
    if (out_fd >= 0 && out_fd != 1) {
        WGET_CALL(SYS_CLOSE, out_fd, 0, 0, 0, 0, 0);
        out_fd = -1;
    }

    /* 9. Content-Length Verification (Post-download check) */
    if (s_response.has_content_length) {
        if (total_received != s_response.content_length) {
            message("wget: read error: expected ");
            print_dec(s_response.content_length);
            message(" bytes, got ");
            print_dec(total_received);
            message("\n");
            return 1;
        }
    }

    if (!quiet) {
        message("'");
        message(s_out_path);
        message("' saved [");
        print_dec(total_received);
        if (s_response.has_content_length) {
            message("/");
            print_dec(s_response.content_length);
        }
        message("]\n");
    }

    return 0;

failure:
    if (sock >= 0) WGET_CALL(SYS_CLOSE, sock, 0, 0, 0, 0, 0);
    if (out_fd >= 0 && out_fd != 1) WGET_CALL(SYS_CLOSE, out_fd, 0, 0, 0, 0, 0);
    return 1;
}
