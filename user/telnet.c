#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "poll.h"
#include "udp_common.h"
#include "terminal.h"
#include "dns.h"
#include "netconf.h"

#ifdef TEST_HOST
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#ifndef TELNET_CALL
#define TELNET_CALL udp_call
#endif

#ifndef NETCONF_SYSCALL
#define NETCONF_SYSCALL(nr, a, b, c) TELNET_CALL(nr, a, b, c, 0, 0, 0)
#endif

/* Telnet Protocol Constants (RFC 854) */
#define TELNET_SE    240  /* Subnegotiation End */
#define TELNET_NOP   241  /* No Operation */
#define TELNET_DM    242  /* Data Mark */
#define TELNET_BRK   243  /* Break */
#define TELNET_IP    244  /* Interrupt Process */
#define TELNET_AO    245  /* Abort Output */
#define TELNET_AYT   246  /* Are You There */
#define TELNET_EC    247  /* Erase Character */
#define TELNET_EL    248  /* Erase Line */
#define TELNET_GA    249  /* Go Ahead */
#define TELNET_SB    250  /* Subnegotiation Begin */
#define TELNET_WILL  251  /* Desire to begin performing option */
#define TELNET_WONT  252  /* Refusal to perform option */
#define TELNET_DO    253  /* Request that other party perform option */
#define TELNET_DONT  254  /* Demand that other party stop performing option */
#define TELNET_IAC   255  /* Interpret As Command */

/* Common Telnet Options */
#define TELOPT_BINARY   0
#define TELOPT_ECHO     1
#define TELOPT_RCP      2
#define TELOPT_SGA      3   /* Suppress Go Ahead */
#define TELOPT_NAMS     4
#define TELOPT_STATUS   5
#define TELOPT_TM       6   /* Timing Mark */
#define TELOPT_TTYPE    24  /* Terminal Type */
#define TELOPT_NAWS     31  /* Window Size */
#define TELOPT_LINEMODE 34

/* Default Escape Character: ^] (ASCII 29 / 0x1D) */
#define DEFAULT_ESCAPE_CHAR 0x1D

typedef enum {
    IAC_STATE_NORMAL = 0,
    IAC_STATE_IAC,
    IAC_STATE_WILL,
    IAC_STATE_WONT,
    IAC_STATE_DO,
    IAC_STATE_DONT,
    IAC_STATE_SB,
    IAC_STATE_SB_IAC
} iac_state_t;

/* Static BSS storage adhering strictly to Gate G1 (zero dynamic heap) */
static uint8_t      s_sock_read_buf[2048];
static uint8_t      s_term_write_buf[2048];
static uint8_t      s_stdin_read_buf[2048];
static uint8_t      s_sock_write_buf[2048];
static char         s_prompt_buf[128];
static char         s_ip_str[32];
static dns_context_t s_dns_context;
static dns_result_t  s_dns_result;

static iac_state_t  s_iac_state = IAC_STATE_NORMAL;
static bool         s_seen_cr = false;
static uint8_t      s_escape_char = DEFAULT_ESCAPE_CHAR;

/* I/O message helpers */
static void telnet_write_fd(int fd, const char *msg) {
    size_t len = udp_length(msg);
    size_t at = 0;
    while (at < len) {
        long w = TELNET_CALL(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(msg + at), len - at, 0, 0, 0);
        if (w <= 0) break;
        at += (size_t)w;
    }
}

static void telnet_write_bytes(int fd, const uint8_t *data, size_t len) {
    size_t at = 0;
    while (at < len) {
        long w = TELNET_CALL(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(data + at), len - at, 0, 0, 0);
        if (w <= 0) break;
        at += (size_t)w;
    }
}

static void print_usage(void) {
    telnet_write_fd(2,
        "usage: telnet [options] <host> [port]\n"
        "\n"
        "Options:\n"
        "  -e <char>    Set escape character (default '^]')\n"
        "  -s <dns-ip>  Override DNS server IPv4 address for host resolution\n"
        "  -h, --help   Display this help message\n"
        "\n"
        "Default port is 23.\n"
        "Escape character '^]' enters the telnet> command prompt.\n");
}

/* Format dotted IPv4 in network byte order */
static void format_ipv4(char *buf, uint32_t net_ip) {
    const uint8_t *b = (const uint8_t *)&net_ip;
    size_t pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t val = b[i];
        if (val >= 100) {
            buf[pos++] = (char)('0' + (val / 100));
            buf[pos++] = (char)('0' + ((val / 10) % 10));
            buf[pos++] = (char)('0' + (val % 10));
        } else if (val >= 10) {
            buf[pos++] = (char)('0' + (val / 10));
            buf[pos++] = (char)('0' + (val % 10));
        } else {
            buf[pos++] = (char)('0' + val);
        }
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

/* Format unsigned integer */
static void format_u32(char *buf, uint32_t val) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[16];
    int t = 0;
    while (val > 0) {
        tmp[t++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int pos = 0;
    while (t > 0) buf[pos++] = tmp[--t];
    buf[pos] = '\0';
}

/* Send a 3-byte Telnet option negotiation reply */
static void send_iac_reply(int sock, uint8_t cmd, uint8_t opt) {
    uint8_t reply[3];
    reply[0] = TELNET_IAC;
    reply[1] = cmd;
    reply[2] = opt;
    telnet_write_bytes(sock, reply, 3);
}

/*
 * Process bytes received from the remote Telnet server:
 * Filters RFC 854 IAC commands and negotiates options,
 * translates NVT line endings (\r\n and \r\0 to \n),
 * and writes clean printable bytes to stdout.
 */
static void process_server_bytes(int sock, const uint8_t *src, size_t count) {
    size_t out_len = 0;

    for (size_t i = 0; i < count; i++) {
        uint8_t b = src[i];

        switch (s_iac_state) {
        case IAC_STATE_NORMAL:
            if (b == TELNET_IAC) {
                s_iac_state = IAC_STATE_IAC;
            } else {
                /* NVT newline translation:
                 * \r\n -> \n
                 * \r\0 -> \n
                 * lone \r -> \r
                 */
                if (s_seen_cr) {
                    s_seen_cr = false;
                    if (b == '\n' || b == '\0') {
                        s_term_write_buf[out_len++] = '\n';
                    } else {
                        s_term_write_buf[out_len++] = '\r';
                        s_term_write_buf[out_len++] = b;
                    }
                } else if (b == '\r') {
                    s_seen_cr = true;
                } else {
                    s_term_write_buf[out_len++] = b;
                }

                if (out_len >= sizeof(s_term_write_buf) - 2) {
                    telnet_write_bytes(1, s_term_write_buf, out_len);
                    out_len = 0;
                }
            }
            break;

        case IAC_STATE_IAC:
            switch (b) {
            case TELNET_IAC:
                /* Escaped 0xFF: literal data byte */
                s_term_write_buf[out_len++] = 0xFF;
                s_iac_state = IAC_STATE_NORMAL;
                break;
            case TELNET_WILL:
                s_iac_state = IAC_STATE_WILL;
                break;
            case TELNET_WONT:
                s_iac_state = IAC_STATE_WONT;
                break;
            case TELNET_DO:
                s_iac_state = IAC_STATE_DO;
                break;
            case TELNET_DONT:
                s_iac_state = IAC_STATE_DONT;
                break;
            case TELNET_SB:
                s_iac_state = IAC_STATE_SB;
                break;
            case TELNET_AYT:
                /* Are You There response */
                telnet_write_fd(sock, "\r\n[FortressOS Telnet]\r\n");
                s_iac_state = IAC_STATE_NORMAL;
                break;
            default:
                /* Other 2-byte commands: NOP, DM, BRK, IP, AO, EC, EL, GA */
                s_iac_state = IAC_STATE_NORMAL;
                break;
            }
            break;

        case IAC_STATE_WILL:
            /* Server offers to enable option 'b' */
            if (b == TELOPT_SGA) {
                /* Accept Suppress Go Ahead */
                send_iac_reply(sock, TELNET_DO, TELOPT_SGA);
            } else {
                /* Refuse all other options */
                send_iac_reply(sock, TELNET_DONT, b);
            }
            s_iac_state = IAC_STATE_NORMAL;
            break;

        case IAC_STATE_WONT:
            /* Server refuses option 'b': acknowledge */
            send_iac_reply(sock, TELNET_DONT, b);
            s_iac_state = IAC_STATE_NORMAL;
            break;

        case IAC_STATE_DO:
            /* Server requests client to enable option 'b' */
            if (b == TELOPT_SGA) {
                send_iac_reply(sock, TELNET_WILL, TELOPT_SGA);
            } else {
                /* Refuse all other options */
                send_iac_reply(sock, TELNET_WONT, b);
            }
            s_iac_state = IAC_STATE_NORMAL;
            break;

        case IAC_STATE_DONT:
            /* Server demands client not perform option 'b': acknowledge */
            send_iac_reply(sock, TELNET_WONT, b);
            s_iac_state = IAC_STATE_NORMAL;
            break;

        case IAC_STATE_SB:
            /* Subnegotiation payload: swallow until IAC */
            if (b == TELNET_IAC) {
                s_iac_state = IAC_STATE_SB_IAC;
            }
            break;

        case IAC_STATE_SB_IAC:
            if (b == TELNET_SE) {
                /* Subnegotiation finished */
                s_iac_state = IAC_STATE_NORMAL;
            } else if (b == TELNET_IAC) {
                /* Escaped IAC within subnegotiation */
                s_iac_state = IAC_STATE_SB;
            } else {
                s_iac_state = IAC_STATE_SB;
            }
            break;
        }
    }

    if (out_len > 0) {
        telnet_write_bytes(1, s_term_write_buf, out_len);
    }
}

/*
 * Interactive Telnet Command Prompt (^])
 * Returns true if session should continue, false if user requested quit/close.
 */
static bool handle_escape_prompt(int sock, const char *host_str, uint16_t port) {
    (void)sock;
    telnet_write_fd(1, "\ntelnet> ");

    size_t pos = 0;
    while (pos < sizeof(s_prompt_buf) - 1) {
        char ch = 0;
        long r = TELNET_CALL(SYS_READ, 0, (uintptr_t)&ch, 1, 0, 0, 0);
        if (r <= 0) {
            /* EOF on stdin */
            return false;
        }
        if (ch == '\r') continue;
        if (ch == '\n') break;
        s_prompt_buf[pos++] = ch;
    }
    s_prompt_buf[pos] = '\0';

    /* Empty input resumes session */
    if (pos == 0) {
        return true;
    }

    /* Trim leading spaces */
    const char *cmd = s_prompt_buf;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    if (udp_equal(cmd, "close") || udp_equal(cmd, "quit") || udp_equal(cmd, "q")) {
        telnet_write_fd(1, "Connection closed.\n");
        return false;
    }

    if (udp_equal(cmd, "status")) {
        telnet_write_fd(1, "Connected to ");
        telnet_write_fd(1, host_str);
        telnet_write_fd(1, " (port ");
        char pbuf[16];
        format_u32(pbuf, (uint32_t)port);
        telnet_write_fd(1, pbuf);
        telnet_write_fd(1, ").\n");

        telnet_write_fd(1, "Escape character is '");
        if (s_escape_char == DEFAULT_ESCAPE_CHAR) {
            telnet_write_fd(1, "^]");
        } else {
            char esc_s[2] = {(char)s_escape_char, '\0'};
            telnet_write_fd(1, esc_s);
        }
        telnet_write_fd(1, "'.\n");
        return true;
    }

    if (udp_equal(cmd, "help") || udp_equal(cmd, "?")) {
        telnet_write_fd(1,
            "Commands:\n"
            "  close, quit  - close current connection\n"
            "  status       - display connection status\n"
            "  help, ?      - print help information\n"
            "  <enter>      - resume connection\n");
        return true;
    }

    telnet_write_fd(1, "?Invalid command. Type 'help' for available commands.\n");
    return true;
}

/*
 * Process bytes from standard input:
 * Checks for escape character, translates newline \n to \r\n,
 * and escapes literal 0xFF bytes.
 * Returns true if session should continue, false on escape quit.
 */
static bool process_stdin_bytes(int sock, const uint8_t *src, size_t count,
                                const char *host_str, uint16_t port) {
    size_t out_len = 0;

    for (size_t i = 0; i < count; i++) {
        uint8_t b = src[i];

        if (b == s_escape_char) {
            /* Flush any buffered output to socket before entering prompt */
            if (out_len > 0) {
                telnet_write_bytes(sock, s_sock_write_buf, out_len);
                out_len = 0;
            }
            if (!handle_escape_prompt(sock, host_str, port)) {
                return false;
            }
            continue;
        }

        if (b == '\n') {
            /* Local newline -> NVT CRLF */
            s_sock_write_buf[out_len++] = '\r';
            s_sock_write_buf[out_len++] = '\n';
        } else if (b == TELNET_IAC) {
            /* Escape literal 0xFF byte */
            s_sock_write_buf[out_len++] = TELNET_IAC;
            s_sock_write_buf[out_len++] = TELNET_IAC;
        } else {
            s_sock_write_buf[out_len++] = b;
        }

        if (out_len >= sizeof(s_sock_write_buf) - 2) {
            telnet_write_bytes(sock, s_sock_write_buf, out_len);
            out_len = 0;
        }
    }

    if (out_len > 0) {
        telnet_write_bytes(sock, s_sock_write_buf, out_len);
    }
    return true;
}

/*
 * Main Bidirectional Multiplexing Loop (SYS_POLL)
 */
static int telnet_run_loop(int sock, const char *host_str, uint16_t port) {
    struct pollfd pfds[2];
    pfds[0].fd = 0;       /* stdin */
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    pfds[1].fd = sock;    /* tcp socket */
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    bool stdin_eof = false;

    for (;;) {
        pfds[0].revents = 0;
        pfds[1].revents = 0;

        long pr = TELNET_CALL(SYS_POLL, (uintptr_t)pfds, 2, -1, 0, 0, 0);
        if (pr < 0) {
            /* Interrupted or poll error */
            break;
        }

        /* 1. Remote socket has incoming data */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            long r = TELNET_CALL(SYS_READ, (uintptr_t)sock, (uintptr_t)s_sock_read_buf,
                                sizeof(s_sock_read_buf), 0, 0, 0);
            if (r <= 0) {
                /* Connection closed by foreign host */
                telnet_write_fd(1, "\nConnection closed by foreign host.\n");
                return 0;
            }
            process_server_bytes(sock, s_sock_read_buf, (size_t)r);
        }

        /* 2. Stdin has incoming user input */
        if (!stdin_eof && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            long r = TELNET_CALL(SYS_READ, 0, (uintptr_t)s_stdin_read_buf,
                                sizeof(s_stdin_read_buf), 0, 0, 0);
            if (r <= 0) {
                /* Stdin reached EOF (e.g. piped input completed) */
                stdin_eof = true;
                pfds[0].fd = -1;
                (void)TELNET_CALL(SYS_SHUTDOWN, (uintptr_t)sock, NET_SHUT_WR, 0, 0, 0, 0);
            } else {
                if (!process_stdin_bytes(sock, s_stdin_read_buf, (size_t)r, host_str, port)) {
                    /* User exited via escape command (quit/close) */
                    return 0;
                }
            }
        }
    }

    return 0;
}

int telnet_main(int argc, char **argv) {
    const char *host = NULL;
    const char *port_str = NULL;
    uint32_t dns_server = 0;
    bool has_dns_server = false;
    uint16_t port = 23;

    /* Parse command-line options */
    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (udp_equal(arg, "-h") || udp_equal(arg, "--help")) {
            print_usage();
            return 0;
        } else if (udp_equal(arg, "-s")) {
            if (i + 1 >= argc) {
                telnet_write_fd(2, "telnet: option -s requires an IPv4 address\n");
                return 1;
            }
            if (!udp_ip(argv[++i], &dns_server)) {
                telnet_write_fd(2, "telnet: invalid DNS server IPv4 address\n");
                return 1;
            }
            has_dns_server = true;
        } else if (udp_equal(arg, "-e")) {
            if (i + 1 >= argc || argv[i+1][0] == '\0') {
                telnet_write_fd(2, "telnet: option -e requires an escape character\n");
                return 1;
            }
            s_escape_char = (uint8_t)argv[++i][0];
        } else if (arg[0] == '-') {
            telnet_write_fd(2, "telnet: unknown option '");
            telnet_write_fd(2, arg);
            telnet_write_fd(2, "'\n");
            print_usage();
            return 1;
        } else {
            if (!host) {
                host = arg;
            } else if (!port_str) {
                port_str = arg;
            } else {
                telnet_write_fd(2, "telnet: unexpected argument '");
                telnet_write_fd(2, arg);
                telnet_write_fd(2, "'\n");
                print_usage();
                return 1;
            }
        }
        i++;
    }

    if (!host) {
        print_usage();
        return 1;
    }

    /* Parse port if provided, otherwise default to 23 */
    if (port_str) {
        unsigned pval = 0;
        const char *p = port_str;
        if (!udp_number(&p, 65535, &pval) || *p != '\0' || pval == 0) {
            telnet_write_fd(2, "telnet: invalid port number '");
            telnet_write_fd(2, port_str);
            telnet_write_fd(2, "'\n");
            return 1;
        }
        port = (uint16_t)pval;
    }

    /* Resolve host */
    uint32_t target_ip = 0;
    if (!udp_ip(host, &target_ip)) {
        size_t hlen = udp_length(host);
        if (hlen > 254) {
            telnet_write_fd(2, "telnet: hostname too long\n");
            return 1;
        }

        dns_options_t dopt = {
            .server_ipv4 = has_dns_server ? dns_server : 0
        };
        dns_context_init(&s_dns_context);

        int status = dns_resolve_ipv4(&s_dns_context, &dopt, host, hlen, &s_dns_result);
        if (status != 0) {
            if (!has_dns_server && dns_servers_configured(&s_dns_context) == 0) {
                telnet_write_fd(2, "telnet: no DNS servers configured in /etc/network/resolv.conf\n");
                return 1;
            }
            telnet_write_fd(2, "telnet: cannot resolve '");
            telnet_write_fd(2, host);
            telnet_write_fd(2, "': ");
            telnet_write_fd(2, dns_status_name(status));
            telnet_write_fd(2, "\n");
            return 1;
        }
        target_ip = s_dns_result.addresses[0];
    }

    format_ipv4(s_ip_str, target_ip);

    telnet_write_fd(1, "Trying ");
    telnet_write_fd(1, s_ip_str);
    telnet_write_fd(1, "...\n");

    /* Create TCP socket */
    long sock = TELNET_CALL(SYS_SOCKET, NET_AF_INET, NET_SOCK_STREAM | NET_SOCK_CLOEXEC, 6, 0, 0, 0);
    if (sock < 0) {
        telnet_write_fd(2, "telnet: failed to create TCP socket\n");
        return 1;
    }

    net_sockaddr_in_t addr = {
        .family = NET_AF_INET,
        .port = __builtin_bswap16(port),
        .address = target_ip
    };

    long conn = TELNET_CALL(SYS_CONNECT, (uintptr_t)sock, (uintptr_t)&addr, sizeof(addr), 0, 0, 0);
    if (conn < 0) {
        telnet_write_fd(2, "telnet: unable to connect to remote host: Connection refused\n");
        TELNET_CALL(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
        return 1;
    }

    telnet_write_fd(1, "Connected to ");
    telnet_write_fd(1, host);
    telnet_write_fd(1, ".\n");
    telnet_write_fd(1, "Escape character is '");
    if (s_escape_char == DEFAULT_ESCAPE_CHAR) {
        telnet_write_fd(1, "^]");
    } else {
        char esc_s[2] = {(char)s_escape_char, '\0'};
        telnet_write_fd(1, esc_s);
    }
    telnet_write_fd(1, "'.\n");

    /* Reset state for new session */
    s_iac_state = IAC_STATE_NORMAL;
    s_seen_cr = false;

    /* Run interactive multiplexing loop */
    int ret = telnet_run_loop((int)sock, host, port);

    TELNET_CALL(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
    return ret;
}
