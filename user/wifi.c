#include "types.h"
#include "syscall_abi.h"
#include "wifi_abi.h"

typedef long (*gzip_read_fn)(void *, void *, size_t);
typedef long (*gzip_write_fn)(void *, const void *, size_t);
int gzip_decode_callbacks(gzip_read_fn read, gzip_write_fn write, void *context);

#ifndef WIFI_SYSCALL
static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}
#define WIFI_SYSCALL(nr, a, b, c) sys_call((nr), (uintptr_t)(a), (uintptr_t)(b), (uintptr_t)(c), 0, 0, 0)
#endif

static char s_out[1024];
static wifi_scan_result_t s_scan_res;

static bool write_str(int fd, const char *str);


static uint8_t s_fw_decomp_buf[2000000];
static size_t  s_fw_decomp_pos = 0;
static bool    s_fw_loaded = false;

static long fw_read_cb(void *ctx, void *buf, size_t count) {
    int fd = (int)(intptr_t)ctx;
    long r = WIFI_SYSCALL(SYS_READ, fd, buf, count);
    return r;
}

static long fw_write_cb(void *ctx, const void *buf, size_t count) {
    (void)ctx;
    if (s_fw_decomp_pos + count > sizeof(s_fw_decomp_buf)) {
        return -1;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        s_fw_decomp_buf[s_fw_decomp_pos + i] = src[i];
    }
    s_fw_decomp_pos += count;
    return (long)count;
}

static bool ensure_firmware_loaded(void) {
    if (s_fw_loaded) return true;

    wifi_status_t st;
    if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &st, sizeof(st)) == 0) {
        /* FAILED/SCANNING do not prove firmware readiness. LOAD_FW is already
         * idempotent for a live runtime image; retry preparation after failure. */
        if (st.link_online && st.state == WIFI_STATUS_CONNECTED) {
            s_fw_loaded = true;
            return true;
        }
    }

    /* Try compressed firmware */
    int fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/lib/firmware/iwlwifi-8265-36.ucode.gz", 0, 0);
    if (fd >= 0) {
        s_fw_decomp_pos = 0;
        int gz_rc = gzip_decode_callbacks(fw_read_cb, fw_write_cb, (void *)(intptr_t)fd);
        WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
        if (gz_rc == 0 && s_fw_decomp_pos > 0) {
            long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_LOAD_FW, s_fw_decomp_buf, s_fw_decomp_pos);
            if (rc == 0) {
                s_fw_loaded = true;
                return true;
            } else {
                write_str(2, "[wifi] Hardware bootstrap returned error.\n");
            }
        } else {
            write_str(2, "[wifi] Failed to decompress firmware.\n");
        }
    }

    /* Fallback to uncompressed firmware */
    fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/lib/firmware/iwlwifi-8265-36.ucode", 0, 0);
    if (fd >= 0) {
        long rd = WIFI_SYSCALL(SYS_READ, fd, s_fw_decomp_buf, sizeof(s_fw_decomp_buf));
        WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
        if (rd > 0) {
            long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_LOAD_FW, s_fw_decomp_buf, (size_t)rd);
            if (rc == 0) {
                s_fw_loaded = true;
                return true;
            }
        }
    }

    write_str(2, "[wifi] Error: could not load /lib/firmware/iwlwifi-8265-36.ucode.gz\n");
    return false;
}

static size_t mystrlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static bool write_str(int fd, const char *str) {
    size_t len = mystrlen(str);
    while (len > 0) {
        long w = WIFI_SYSCALL(SYS_WRITE, fd, str, len);
        if (w <= 0) return false;
        str += w;
        len -= (size_t)w;
    }
    return true;
}

static void append_str(char *buf, size_t *pos, const char *s) {
    while (*s && *pos < sizeof(s_out) - 1) {
        buf[(*pos)++] = *s++;
    }
}

static void append_int(char *buf, size_t *pos, int val) {
    char tmp[16];
    size_t i = 0;
    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    if (neg && *pos < sizeof(s_out) - 1) buf[(*pos)++] = '-';
    while (i > 0 && *pos < sizeof(s_out) - 1) {
        buf[(*pos)++] = tmp[--i];
    }
}

static void append_hex_byte(char *buf, size_t *pos, uint8_t byte) {
    const char hex[] = "0123456789abcdef";
    if (*pos < sizeof(s_out) - 2) {
        buf[(*pos)++] = hex[(byte >> 4) & 0x0f];
        buf[(*pos)++] = hex[byte & 0x0f];
    }
}

static void append_mac(char *buf, size_t *pos, const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (i > 0 && *pos < sizeof(s_out) - 1) buf[(*pos)++] = ':';
        append_hex_byte(buf, pos, mac[i]);
    }
}

static void append_pad(char *buf, size_t *pos, size_t target_col) {
    while (*pos < target_col && *pos < sizeof(s_out) - 1) {
        buf[(*pos)++] = ' ';
    }
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const char *state_to_str(uint32_t st) {
    switch (st) {
        case WIFI_STATUS_IDLE: return "IDLE";
        case WIFI_STATUS_SCANNING: return "SCANNING";
        case WIFI_STATUS_AUTHENTICATING: return "AUTHENTICATING";
        case WIFI_STATUS_ASSOCIATING: return "ASSOCIATING";
        case WIFI_STATUS_HANDSHAKE: return "4-WAY HANDSHAKE";
        case WIFI_STATUS_CONNECTED: return "CONNECTED";
        case WIFI_STATUS_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

static int do_scan(void) {
    ensure_firmware_loaded();

    /* 1. Asynchronously kick hardware multi-channel scan */
    s_scan_res.count = 0;
    s_scan_res.max_entries = 0;
    (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &s_scan_res, sizeof(s_scan_res));

    /* 2. Show animated progress bar while hardware dwells across channels (1.2s) */
    const char *bars[] = {
        "Scanning [==        ]",
        "Scanning [====      ]",
        "Scanning [======    ]",
        "Scanning [========  ]",
        "Scanning [==========]",
    };
    for (int i = 0; i < 5; i++) {
        write_str(1, "\r");
        write_str(1, bars[i]);
        timespec_t ts = { .tv_sec = 0, .tv_nsec = 240000000 }; /* 240ms * 5 = 1.2s */
        WIFI_SYSCALL(SYS_NANOSLEEP, &ts, 0, 0);
    }
    write_str(1, "\r                     \r"); /* Clear progress bar */

    s_scan_res.count = 0;
    s_scan_res.max_entries = WIFI_MAX_SCAN_ENTRIES;

    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &s_scan_res, sizeof(s_scan_res));
    if (rc != 0) {
        write_str(2, "wifi: scan failed\n");
        return 1;
    }

    if (s_scan_res.count == 0) {
        write_str(1, "No wireless networks found in range.\n");
        return 0;
    }

    write_str(1, "BSSID              CHAN  RSSI   SEC       SSID\n");
    write_str(1, "-----------------  ----  -----  --------  --------------------------------\n");

    for (uint32_t i = 0; i < s_scan_res.count; i++) {
        const wifi_scan_entry_t *e = &s_scan_res.entries[i];
        size_t pos = 0;

        /* BSSID */
        append_mac(s_out, &pos, e->bssid);
        append_pad(s_out, &pos, 19);

        /* Channel */
        append_int(s_out, &pos, e->channel);
        append_pad(s_out, &pos, 25);

        /* RSSI */
        append_int(s_out, &pos, e->rssi);
        append_pad(s_out, &pos, 32);

        /* Security */
        append_str(s_out, &pos, e->security == WIFI_SEC_WPA2_PSK ? "WPA2-PSK" : "OPEN");
        append_pad(s_out, &pos, 42);

        /* SSID */
        append_str(s_out, &pos, e->ssid);
        s_out[pos++] = '\n';
        s_out[pos] = '\0';

        write_str(1, s_out);
    }
    return 0;
}

static int do_status(void) {
    wifi_status_t st;
    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &st, sizeof(st));
    if (rc != 0) {
        write_str(2, "wifi: failed to get status\n");
        return 1;
    }

    size_t pos = 0;
    append_str(s_out, &pos, "Interface: wlan0 (");
    append_mac(s_out, &pos, st.mac);
    append_str(s_out, &pos, ")\nStatus: ");
    append_str(s_out, &pos, state_to_str(st.state));
    append_str(s_out, &pos, "\n");

    if (st.state != WIFI_STATUS_IDLE) {
        append_str(s_out, &pos, "SSID: \"");
        append_str(s_out, &pos, st.ssid);
        append_str(s_out, &pos, "\"\nBSSID: ");
        append_mac(s_out, &pos, st.bssid);
        append_str(s_out, &pos, "\nChannel: ");
        append_int(s_out, &pos, st.channel);
        append_str(s_out, &pos, "\nRSSI: ");
        append_int(s_out, &pos, st.rssi);
        append_str(s_out, &pos, " dBm\nLink: ");
        append_str(s_out, &pos, st.link_online ? "ONLINE\n" : "CONNECTING / DOWN\n");
    }
    s_out[pos] = '\0';
    write_str(1, s_out);
    return 0;
}

static int do_disconnect(void) {
    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
    if (rc != 0) {
        write_str(2, "wifi: disconnect failed\n");
        return 1;
    }
    (void)WIFI_SYSCALL(SYS_UNLINK, (uintptr_t)"/run/wifi-static.configured", 0, 0);
    write_str(1, "Disconnected from wireless network.\n");
    return 0;
}

#define MAX_SAVED_NETWORKS 16

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char psk[WIFI_PASSPHRASE_MAX_LEN + 1];
    char address[32];   /* e.g. "192.168.1.150/24" (empty = DHCP) */
    char gateway[16];   /* e.g. "192.168.1.1" */
    char dns[16];       /* e.g. "1.1.1.1" */
} saved_net_t;

static saved_net_t s_saved_nets[MAX_SAVED_NETWORKS];
static int         s_saved_count = 0;

static void commit_current_network(char *cur_ssid, char *cur_psk,
                                   char *cur_addr, char *cur_gw, char *cur_dns) {
    if (!cur_ssid[0] || !cur_psk[0] || s_saved_count >= MAX_SAVED_NETWORKS) return;
    saved_net_t *n = &s_saved_nets[s_saved_count++];
    for (size_t i = 0; i < sizeof(n->ssid); i++) n->ssid[i] = 0;
    for (size_t i = 0; i < sizeof(n->psk); i++) n->psk[i] = 0;
    for (size_t i = 0; i < sizeof(n->address); i++) n->address[i] = 0;
    for (size_t i = 0; i < sizeof(n->gateway); i++) n->gateway[i] = 0;
    for (size_t i = 0; i < sizeof(n->dns); i++) n->dns[i] = 0;

    for (size_t i = 0; cur_ssid[i] && i < WIFI_SSID_MAX_LEN; i++) n->ssid[i] = cur_ssid[i];
    for (size_t i = 0; cur_psk[i] && i < WIFI_PASSPHRASE_MAX_LEN; i++) n->psk[i] = cur_psk[i];
    for (size_t i = 0; cur_addr[i] && i < sizeof(n->address) - 1; i++) n->address[i] = cur_addr[i];
    for (size_t i = 0; cur_gw[i] && i < sizeof(n->gateway) - 1; i++) n->gateway[i] = cur_gw[i];
    for (size_t i = 0; cur_dns[i] && i < sizeof(n->dns) - 1; i++) n->dns[i] = cur_dns[i];

    cur_ssid[0] = '\0';
    cur_psk[0] = '\0';
    cur_addr[0] = '\0';
    cur_gw[0] = '\0';
    cur_dns[0] = '\0';
}

static int load_saved_networks(void) {
    s_saved_count = 0;
    int fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wlan.conf", 0, 0);
    if (fd < 0) {
        fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wifi.conf", 0, 0);
    }
    if (fd < 0) return 0;

    static char buf[2048];
    long rd = WIFI_SYSCALL(SYS_READ, fd, buf, sizeof(buf) - 1);
    WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
    if (rd <= 0) return 0;
    buf[rd] = '\0';

    char *line = buf;
    char cur_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char cur_psk[WIFI_PASSPHRASE_MAX_LEN + 1] = {0};
    char cur_addr[32] = {0};
    char cur_gw[16] = {0};
    char cur_dns[16] = {0};

    while (*line && s_saved_count < MAX_SAVED_NETWORKS) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
        if (!*line) break;
        char *line_end = line;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
        char saved_ch = *line_end;
        *line_end = '\0';

        if (*line != '#') {
            if (line[0] == '[' && line[1] == 'n' && line[2] == 'e' && line[3] == 't') {
                commit_current_network(cur_ssid, cur_psk, cur_addr, cur_gw, cur_dns);
            } else if (line[0] == 's' && line[1] == 's' && line[2] == 'i' && line[3] == 'd' && line[4] == '=') {
                commit_current_network(cur_ssid, cur_psk, cur_addr, cur_gw, cur_dns);
                const char *val = line + 5;
                if (*val == '"') {
                    val++;
                    size_t k = 0;
                    while (*val && *val != '"' && k < WIFI_SSID_MAX_LEN) cur_ssid[k++] = *val++;
                    cur_ssid[k] = '\0';
                } else {
                    size_t k = 0;
                    while (*val && *val != ' ' && *val != '\t' && k < WIFI_SSID_MAX_LEN) cur_ssid[k++] = *val++;
                    cur_ssid[k] = '\0';
                }
            } else if (line[0] == 'p' && line[1] == 's' && line[2] == 'k' && line[3] == '=') {
                const char *val = line + 4;
                if (*val == '"') {
                    val++;
                    size_t k = 0;
                    while (*val && *val != '"' && k < WIFI_PASSPHRASE_MAX_LEN) cur_psk[k++] = *val++;
                    cur_psk[k] = '\0';
                } else {
                    size_t k = 0;
                    while (*val && *val != ' ' && *val != '\t' && k < WIFI_PASSPHRASE_MAX_LEN) cur_psk[k++] = *val++;
                    cur_psk[k] = '\0';
                }
            } else if ((line[0] == 'a' && line[1] == 'd' && line[2] == 'd' && line[3] == 'r') ||
                       (line[0] == 'i' && line[1] == 'p' && line[2] == '=')) {
                const char *val = (line[0] == 'i') ? line + 3 : line + 8;
                if (*val == '"') {
                    val++;
                    size_t k = 0;
                    while (*val && *val != '"' && k < sizeof(cur_addr) - 1) cur_addr[k++] = *val++;
                    cur_addr[k] = '\0';
                } else {
                    size_t k = 0;
                    while (*val && *val != ' ' && *val != '\t' && k < sizeof(cur_addr) - 1) cur_addr[k++] = *val++;
                    cur_addr[k] = '\0';
                }
            } else if ((line[0] == 'g' && line[1] == 'a' && line[2] == 't' && line[3] == 'e') ||
                       (line[0] == 'g' && line[1] == 'w' && line[2] == '=')) {
                const char *val = (line[1] == 'w') ? line + 3 : line + 8;
                if (*val == '"') {
                    val++;
                    size_t k = 0;
                    while (*val && *val != '"' && k < sizeof(cur_gw) - 1) cur_gw[k++] = *val++;
                    cur_gw[k] = '\0';
                } else {
                    size_t k = 0;
                    while (*val && *val != ' ' && *val != '\t' && k < sizeof(cur_gw) - 1) cur_gw[k++] = *val++;
                    cur_gw[k] = '\0';
                }
            } else if (line[0] == 'd' && line[1] == 'n' && line[2] == 's' && line[3] == '=') {
                const char *val = line + 4;
                if (*val == '"') {
                    val++;
                    size_t k = 0;
                    while (*val && *val != '"' && k < sizeof(cur_dns) - 1) cur_dns[k++] = *val++;
                    cur_dns[k] = '\0';
                } else {
                    size_t k = 0;
                    while (*val && *val != ' ' && *val != '\t' && k < sizeof(cur_dns) - 1) cur_dns[k++] = *val++;
                    cur_dns[k] = '\0';
                }
            } else if (line[0] == 'n' && line[1] == 'e' && line[2] == 't' && line[3] == 'w' &&
                       line[4] == 'o' && line[5] == 'r' && line[6] == 'k' && line[7] == '=') {
                /* Format: network=SSID:PSK[:ADDR[:GW[:DNS]]] */
                commit_current_network(cur_ssid, cur_psk, cur_addr, cur_gw, cur_dns);
                const char *p = line + 8;
                size_t k = 0;
                while (*p && *p != ':' && k < WIFI_SSID_MAX_LEN) cur_ssid[k++] = *p++;
                cur_ssid[k] = '\0';
                if (*p == ':') p++;
                size_t m = 0;
                while (*p && *p != ':' && m < WIFI_PASSPHRASE_MAX_LEN) cur_psk[m++] = *p++;
                cur_psk[m] = '\0';
                if (*p == ':') {
                    p++;
                    size_t a = 0;
                    while (*p && *p != ':' && a < sizeof(cur_addr) - 1) cur_addr[a++] = *p++;
                    cur_addr[a] = '\0';
                    if (*p == ':') {
                        p++;
                        size_t g = 0;
                        while (*p && *p != ':' && g < sizeof(cur_gw) - 1) cur_gw[g++] = *p++;
                        cur_gw[g] = '\0';
                        if (*p == ':') {
                            p++;
                            size_t d = 0;
                            while (*p && *p != ':' && *p != ' ' && d < sizeof(cur_dns) - 1) cur_dns[d++] = *p++;
                            cur_dns[d] = '\0';
                        }
                    }
                }
                commit_current_network(cur_ssid, cur_psk, cur_addr, cur_gw, cur_dns);
            }
        }

        if (saved_ch == '\0') break;
        line = line_end + 1;
    }

    commit_current_network(cur_ssid, cur_psk, cur_addr, cur_gw, cur_dns);
    return s_saved_count;
}

static bool save_all_networks(void) {
    (void)WIFI_SYSCALL(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0);

    static char content[2048];
    size_t pos = 0;
    const char *hdr = "# /etc/network/wlan.conf - FortressOS Wireless Configuration\n\n";
    while (*hdr) content[pos++] = *hdr++;

    for (int i = 0; i < s_saved_count; i++) {
        saved_net_t *n = &s_saved_nets[i];
        const char *sec = "[network]\nssid=\"";
        while (*sec && pos < sizeof(content) - 1) content[pos++] = *sec++;
        for (size_t k = 0; n->ssid[k] && pos < sizeof(content) - 1; k++) content[pos++] = n->ssid[k];
        const char *p1 = "\"\npsk=\"";
        while (*p1 && pos < sizeof(content) - 1) content[pos++] = *p1++;
        for (size_t k = 0; n->psk[k] && pos < sizeof(content) - 1; k++) content[pos++] = n->psk[k];
        content[pos++] = '"';
        content[pos++] = '\n';

        if (n->address[0]) {
            const char *a = "address=\"";
            while (*a && pos < sizeof(content) - 1) content[pos++] = *a++;
            for (size_t k = 0; n->address[k] && pos < sizeof(content) - 1; k++) content[pos++] = n->address[k];
            content[pos++] = '"';
            content[pos++] = '\n';

            if (n->gateway[0]) {
                const char *g = "gateway=\"";
                while (*g && pos < sizeof(content) - 1) content[pos++] = *g++;
                for (size_t k = 0; n->gateway[k] && pos < sizeof(content) - 1; k++) content[pos++] = n->gateway[k];
                content[pos++] = '"';
                content[pos++] = '\n';
            }
            if (n->dns[0]) {
                const char *d = "dns=\"";
                while (*d && pos < sizeof(content) - 1) content[pos++] = *d++;
                for (size_t k = 0; n->dns[k] && pos < sizeof(content) - 1; k++) content[pos++] = n->dns[k];
                content[pos++] = '"';
                content[pos++] = '\n';
            }
        }
        content[pos++] = '\n';
    }
    content[pos] = '\0';

    int fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wlan.conf", 0x241, 0600);
    if (fd < 0) return false;
    (void)WIFI_SYSCALL(SYS_FCHMOD, fd, 0600, 0);
    long wr = WIFI_SYSCALL(SYS_WRITE, fd, content, pos);
    WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
    return wr > 0;
}

static int do_list(void) {
    int count = load_saved_networks();
    if (count == 0) {
        write_str(1, "No saved wireless networks found in /etc/network/wlan.conf.\n");
        return 0;
    }

    write_str(1, "SSID                             MODE    IP ADDRESS       GATEWAY         DNS\n");
    write_str(1, "-------------------------------- ------  ---------------  --------------  --------------\n");

    for (int i = 0; i < count; i++) {
        saved_net_t *n = &s_saved_nets[i];
        size_t pos = 0;
        append_str(s_out, &pos, n->ssid);
        append_pad(s_out, &pos, 33);

        if (n->address[0]) {
            append_str(s_out, &pos, "STATIC  ");
            append_str(s_out, &pos, n->address);
            append_pad(s_out, &pos, 58);
            append_str(s_out, &pos, n->gateway[0] ? n->gateway : "-");
            append_pad(s_out, &pos, 74);
            append_str(s_out, &pos, n->dns[0] ? n->dns : "-");
        } else {
            append_str(s_out, &pos, "DHCP    -                -               -");
        }
        s_out[pos++] = '\n';
        s_out[pos] = '\0';
        write_str(1, s_out);
    }
    return 0;
}

static int do_save(const char *ssid, const char *passphrase,
                   const char *address, const char *gateway, const char *dns) {
    if (!ssid || !passphrase) {
        write_str(2, "Usage: wifi save <SSID> <passphrase> [--static <IP/prefix> [gateway] [dns]]\n");
        return 1;
    }
    size_t slen = mystrlen(ssid);
    size_t plen = mystrlen(passphrase);
    if (slen == 0 || slen > WIFI_SSID_MAX_LEN || plen < 8 || plen > WIFI_PASSPHRASE_MAX_LEN) {
        write_str(2, "wifi save: invalid SSID or passphrase length (8-63 chars)\n");
        return 1;
    }

    load_saved_networks();

    int existing_idx = -1;
    for (int i = 0; i < s_saved_count; i++) {
        if (streq(s_saved_nets[i].ssid, ssid)) {
            existing_idx = i;
            break;
        }
    }

    int idx = (existing_idx >= 0) ? existing_idx : s_saved_count;
    if (existing_idx < 0) {
        if (s_saved_count >= MAX_SAVED_NETWORKS) {
            write_str(2, "wifi save: maximum number of saved networks reached\n");
            return 1;
        }
        s_saved_count++;
    }

    saved_net_t *n = &s_saved_nets[idx];
    for (size_t i = 0; i < sizeof(n->ssid); i++) n->ssid[i] = 0;
    for (size_t i = 0; i < sizeof(n->psk); i++) n->psk[i] = 0;
    for (size_t i = 0; i < sizeof(n->address); i++) n->address[i] = 0;
    for (size_t i = 0; i < sizeof(n->gateway); i++) n->gateway[i] = 0;
    for (size_t i = 0; i < sizeof(n->dns); i++) n->dns[i] = 0;

    for (size_t i = 0; i < slen; i++) n->ssid[i] = ssid[i];
    for (size_t i = 0; i < plen; i++) n->psk[i] = passphrase[i];
    if (address) {
        for (size_t i = 0; address[i] && i < sizeof(n->address) - 1; i++) n->address[i] = address[i];
    }
    if (gateway) {
        for (size_t i = 0; gateway[i] && i < sizeof(n->gateway) - 1; i++) n->gateway[i] = gateway[i];
    }
    if (dns) {
        for (size_t i = 0; dns[i] && i < sizeof(n->dns) - 1; i++) n->dns[i] = dns[i];
    }

    if (save_all_networks()) {
        write_str(1, "Saved wireless network \"");
        write_str(1, ssid);
        write_str(1, "\"");
        if (n->address[0]) {
            write_str(1, " (static IP: ");
            write_str(1, n->address);
            write_str(1, ")");
        } else {
            write_str(1, " (mode: DHCP)");
        }
        write_str(1, " to /etc/network/wlan.conf\n");
        return 0;
    } else {
        write_str(2, "wifi save: failed to write /etc/network/wlan.conf (permission denied or write error)\n");
        return 1;
    }
}

static int do_forget(const char *ssid) {
    if (!ssid || !ssid[0]) {
        write_str(2, "Usage: wifi forget <SSID>\n");
        return 1;
    }

    load_saved_networks();

    int existing_idx = -1;
    for (int i = 0; i < s_saved_count; i++) {
        if (streq(s_saved_nets[i].ssid, ssid)) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx < 0) {
        write_str(2, "wifi forget: network \"");
        write_str(2, ssid);
        write_str(2, "\" not found in /etc/network/wlan.conf\n");
        return 1;
    }

    /* Shift remaining networks down */
    for (int i = existing_idx; i < s_saved_count - 1; i++) {
        s_saved_nets[i] = s_saved_nets[i + 1];
    }
    s_saved_count--;

    if (!save_all_networks()) {
        write_str(2, "wifi forget: write error\n");
        return 1;
    }

    write_str(1, "Forgot wireless network \"");
    write_str(1, ssid);
    write_str(1, "\" from /etc/network/wlan.conf.\n");

    /* If currently connected to this network, disconnect and clean up */
    wifi_status_t cur_st;
    if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &cur_st, sizeof(cur_st)) == 0 &&
        cur_st.state == WIFI_STATUS_CONNECTED && streq(cur_st.ssid, ssid)) {
        write_str(1, "Disconnecting from forgotten network...\n");
        (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
        (void)WIFI_SYSCALL(SYS_UNLINK, (uintptr_t)"/run/wifi-static.configured", 0, 0);
    }

    return 0;
}

static int do_static(const char *ssid, const char *address, const char *gateway, const char *dns) {
    if (!ssid) {
        write_str(2, "Usage: wifi static <SSID> <IP/prefix> [gateway] [dns]\n"
                     "       wifi static <SSID> dhcp\n");
        return 1;
    }

    load_saved_networks();

    int existing_idx = -1;
    for (int i = 0; i < s_saved_count; i++) {
        if (streq(s_saved_nets[i].ssid, ssid)) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx < 0) {
        /* If SSID not saved yet, create new entry */
        if (s_saved_count < MAX_SAVED_NETWORKS) {
            existing_idx = s_saved_count++;
            for (size_t i = 0; i < sizeof(s_saved_nets[existing_idx].ssid); i++) s_saved_nets[existing_idx].ssid[i] = 0;
            for (size_t i = 0; ssid[i] && i < WIFI_SSID_MAX_LEN; i++) s_saved_nets[existing_idx].ssid[i] = ssid[i];
        } else {
            write_str(2, "wifi static: network \"");
            write_str(2, ssid);
            write_str(2, "\" not found and saved network capacity reached.\n");
            return 1;
        }
    }

    saved_net_t *n = &s_saved_nets[existing_idx];
    if (!address || streq(address, "dhcp") || streq(address, "auto") || streq(address, "none")) {
        for (size_t i = 0; i < sizeof(n->address); i++) n->address[i] = 0;
        for (size_t i = 0; i < sizeof(n->gateway); i++) n->gateway[i] = 0;
        for (size_t i = 0; i < sizeof(n->dns); i++) n->dns[i] = 0;

        if (save_all_networks()) {
            write_str(1, "Network \"");
            write_str(1, ssid);
            write_str(1, "\" reverted to DHCP.\n");
            return 0;
        } else {
            write_str(2, "wifi static: write error\n");
            return 1;
        }
    }

    for (size_t i = 0; i < sizeof(n->address); i++) n->address[i] = 0;
    for (size_t i = 0; i < sizeof(n->gateway); i++) n->gateway[i] = 0;
    for (size_t i = 0; i < sizeof(n->dns); i++) n->dns[i] = 0;

    for (size_t i = 0; address[i] && i < sizeof(n->address) - 1; i++) n->address[i] = address[i];
    if (gateway) {
        for (size_t i = 0; gateway[i] && i < sizeof(n->gateway) - 1; i++) n->gateway[i] = gateway[i];
    }
    if (dns) {
        for (size_t i = 0; dns[i] && i < sizeof(n->dns) - 1; i++) n->dns[i] = dns[i];
    }

    if (save_all_networks()) {
        write_str(1, "Network \"");
        write_str(1, ssid);
        if (n->address[0]) {
            write_str(1, "\" saved with static IP ");
            write_str(1, n->address);
            write_str(1, ".\n");
        } else {
            write_str(1, "\" reverted to DHCP.\n");
        }

        /* If currently connected to this network, apply changes immediately on the live link */
        wifi_status_t cur_st;
        if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &cur_st, sizeof(cur_st)) == 0 &&
            cur_st.state == WIFI_STATUS_CONNECTED && streq(cur_st.ssid, ssid)) {
            if (n->address[0]) {
                write_str(1, "Applying static IP address to wlan0...\n");
                const char *argv_ifup[7];
                int argc_ifup = 0;
                argv_ifup[argc_ifup++] = "/sbin/ifup";
                argv_ifup[argc_ifup++] = "--no-save";
                argv_ifup[argc_ifup++] = n->address;
                if (n->gateway[0]) argv_ifup[argc_ifup++] = n->gateway;
                if (n->dns[0]) argv_ifup[argc_ifup++] = n->dns;
                argv_ifup[argc_ifup] = NULL;

                spawn_opts_t opts;
                for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
                opts.size = sizeof(opts);
                opts.version = 1;
                opts.argv = (uint64_t)argv_ifup;
                long pid = WIFI_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/ifup", (uintptr_t)&opts, sizeof(opts));
                if (pid > 0) {
                    uint64_t status = 0;
                    long waited;
                    do {
                        waited = WIFI_SYSCALL(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
                    } while (waited == -4 /* EINTR */);
                }

                static char st_buf[256];
                size_t st_pos = 0;
                append_str(st_buf, &st_pos, "IP=");
                append_str(st_buf, &st_pos, n->address);
                append_str(st_buf, &st_pos, "\nGATEWAY=");
                append_str(st_buf, &st_pos, n->gateway);
                append_str(st_buf, &st_pos, "\nDNS=");
                append_str(st_buf, &st_pos, n->dns);
                append_str(st_buf, &st_pos, "\n");
                int sfd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/run/wifi-static.configured", 0x241, 0644);
                if (sfd >= 0) {
                    WIFI_SYSCALL(SYS_WRITE, sfd, st_buf, st_pos);
                    WIFI_SYSCALL(SYS_CLOSE, sfd, 0, 0);
                }
            } else {
                (void)WIFI_SYSCALL(SYS_UNLINK, (uintptr_t)"/run/wifi-static.configured", 0, 0);
                write_str(1, "Requesting DHCP lease on wlan0...\n");
                const char *const dhcp_argv[] = { "/sbin/dhcpc", NULL };
                spawn_opts_t opts;
                for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
                opts.size = sizeof(opts);
                opts.version = 1;
                opts.argv = (uint64_t)dhcp_argv;
                long pid = WIFI_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/dhcpc", (uintptr_t)&opts, sizeof(opts));
                if (pid > 0) {
                    uint64_t status = 0;
                    long waited;
                    do {
                        waited = WIFI_SYSCALL(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
                    } while (waited == -4 /* EINTR */);
                }
            }
        }
        return 0;
    } else {
        write_str(2, "wifi static: write error\n");
        return 1;
    }
}

static int do_connect(const char *ssid, const char *passphrase,
                      const char *static_addr, const char *static_gw, const char *static_dns) {
    if (!ssid) {
        write_str(2, "wifi connect: requires <SSID> [passphrase]\n");
        return 1;
    }

    load_saved_networks();

    const char *actual_psk = passphrase;
    const char *actual_addr = static_addr;
    const char *actual_gw = static_gw;
    const char *actual_dns = static_dns;

    int saved_idx = -1;
    for (int i = 0; i < s_saved_count; i++) {
        if (streq(s_saved_nets[i].ssid, ssid)) {
            saved_idx = i;
            break;
        }
    }

    if (!actual_psk && saved_idx >= 0) {
        actual_psk = s_saved_nets[saved_idx].psk;
    }

    if (!actual_psk) {
        write_str(2, "wifi connect: network not saved and no passphrase provided\n");
        return 1;
    }

    if (!actual_addr && saved_idx >= 0 && s_saved_nets[saved_idx].address[0]) {
        actual_addr = s_saved_nets[saved_idx].address;
        if (!actual_gw && s_saved_nets[saved_idx].gateway[0]) actual_gw = s_saved_nets[saved_idx].gateway;
        if (!actual_dns && s_saved_nets[saved_idx].dns[0]) actual_dns = s_saved_nets[saved_idx].dns;
    }

    ensure_firmware_loaded();

    wifi_connect_req_t req;
    for (size_t i = 0; i < sizeof(req.ssid); i++) req.ssid[i] = 0;
    for (size_t i = 0; i < sizeof(req.passphrase); i++) req.passphrase[i] = 0;
    for (size_t i = 0; i < 6; i++) req.bssid[i] = 0;

    size_t s_len = mystrlen(ssid);
    if (s_len > WIFI_SSID_MAX_LEN) s_len = WIFI_SSID_MAX_LEN;
    for (size_t i = 0; i < s_len; i++) req.ssid[i] = ssid[i];

    size_t p_len = mystrlen(actual_psk);
    if (p_len > WIFI_PASSPHRASE_MAX_LEN) p_len = WIFI_PASSPHRASE_MAX_LEN;
    for (size_t i = 0; i < p_len; i++) req.passphrase[i] = actual_psk[i];

    write_str(1, "Connecting to \"");
    write_str(1, ssid);
    write_str(1, "\"...\n");

    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_CONNECT, &req, sizeof(req));
    if (rc != 0) {
        write_str(2, "wifi: connection failed (authentication rejected or invalid passphrase)\n");
        return 1;
    }

    /* Wait up to 6 seconds and pump RX via WIFI_CMD_STATUS until 4-way handshake completes */
    wifi_status_t st;
    for (int wait = 0; wait < 60; wait++) {
        if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &st, sizeof(st)) == 0) {
            if (st.state == WIFI_STATUS_FAILED) {
                write_str(2, "wifi: connection failed (authentication rejected or invalid passphrase)\n");
                return 1;
            }
            if (st.link_online && st.state == WIFI_STATUS_CONNECTED) {
                write_str(1, "Connected to \"");
                write_str(1, ssid);
                write_str(1, "\" (WPA2-PSK). Link is ONLINE.\n");

                timespec_t ts_settle = { .tv_sec = 0, .tv_nsec = 250000000 }; /* 250ms AP bridge port settling delay */
                (void)WIFI_SYSCALL(SYS_NANOSLEEP, &ts_settle, 0, 0);

                if (actual_addr && actual_addr[0]) {
                    write_str(1, "Configuring static IP address ");
                    write_str(1, actual_addr);
                    write_str(1, "...\n");

                    const char *argv_ifup[7];
                    int argc_ifup = 0;
                    argv_ifup[argc_ifup++] = "/sbin/ifup";
                    argv_ifup[argc_ifup++] = "--no-save";
                    argv_ifup[argc_ifup++] = actual_addr;
                    if (actual_gw && actual_gw[0]) argv_ifup[argc_ifup++] = actual_gw;
                    if (actual_dns && actual_dns[0]) argv_ifup[argc_ifup++] = actual_dns;
                    argv_ifup[argc_ifup] = NULL;

                    spawn_opts_t opts;
                    for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
                    opts.size = sizeof(opts);
                    opts.version = 1;
                    opts.argv = (uint64_t)argv_ifup;
                    long pid = WIFI_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/ifup", (uintptr_t)&opts, sizeof(opts));
                    if (pid > 0) {
                        uint64_t status = 0;
                        long waited;
                        do {
                            waited = WIFI_SYSCALL(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
                        } while (waited == -4 /* EINTR */);
                        if (status != 0) {
                            write_str(2, "wifi: ifup failed to configure static IP\n");
                            return 1;
                        }
                    } else {
                        write_str(2, "wifi: failed to spawn /sbin/ifup\n");
                        return 1;
                    }

                    /* Write /run/wifi-static.configured */
                    static char st_buf[256];
                    size_t st_pos = 0;
                    append_str(st_buf, &st_pos, "IP=");
                    append_str(st_buf, &st_pos, actual_addr);
                    append_str(st_buf, &st_pos, "\nGATEWAY=");
                    append_str(st_buf, &st_pos, actual_gw ? actual_gw : "");
                    append_str(st_buf, &st_pos, "\nDNS=");
                    append_str(st_buf, &st_pos, actual_dns ? actual_dns : "");
                    append_str(st_buf, &st_pos, "\n");
                    int sfd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/run/wifi-static.configured", 0x241, 0644);
                    if (sfd >= 0) {
                        WIFI_SYSCALL(SYS_WRITE, sfd, st_buf, st_pos);
                        WIFI_SYSCALL(SYS_CLOSE, sfd, 0, 0);
                    }
                } else {
                    (void)WIFI_SYSCALL(SYS_UNLINK, (uintptr_t)"/run/wifi-static.configured", 0, 0);
                    write_str(1, "Configuring IP address via DHCP...\n");
                    const char *const dhcp_argv[] = { "/sbin/dhcpc", NULL };
                    spawn_opts_t opts;
                    for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
                    opts.size = sizeof(opts);
                    opts.version = 1;
                    opts.argv = (uint64_t)dhcp_argv;
                    long pid = WIFI_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/dhcpc", (uintptr_t)&opts, sizeof(opts));
                    if (pid > 0) {
                        uint64_t status = 0;
                        long waited;
                        do {
                            waited = WIFI_SYSCALL(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
                        } while (waited == -4 /* EINTR */);
                        if (status != 0) {
                            write_str(2, "wifi: failed to configure IP address via DHCP\n");
                            return 1;
                        }
                    } else {
                        write_str(2, "wifi: failed to spawn /sbin/dhcpc\n");
                        return 1;
                    }
                }
                /* Auto-save network profile permanently so on reboot it connects automatically */
                if (do_save(ssid, actual_psk, actual_addr, actual_gw, actual_dns) != 0) {
                    write_str(2, "wifi: warning: connected, but failed to save profile to /etc/network/wlan.conf\n");
                }
                return 0;
            }
        }
        timespec_t ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        WIFI_SYSCALL(SYS_NANOSLEEP, &ts, 0, 0);
    }

    if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &st, sizeof(st)) == 0) {
        if (st.state != WIFI_STATUS_CONNECTED) {
            write_str(2, "wifi: connection failed (authentication rejected or timeout)\n");
            (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
            return 1;
        }
    } else {
        write_str(2, "wifi: connection timed out\n");
        (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
        return 1;
    }
    return 0;
}

static int do_auto(void) {
    if (!ensure_firmware_loaded()) {
        return 0;
    }

    int count = load_saved_networks();
    if (count <= 0) {
        return 0; /* No saved networks configured */
    }

    /* 1. Asynchronously kick hardware multi-channel scan */
    s_scan_res.count = 0;
    s_scan_res.max_entries = 0;
    (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &s_scan_res, sizeof(s_scan_res));

    /* 2. Dwell while hardware sweeps across channels (1.2s) */
    timespec_t ts = { .tv_sec = 1, .tv_nsec = 200000000 }; /* 1.2s */
    WIFI_SYSCALL(SYS_NANOSLEEP, &ts, 0, 0);

    /* 3. Query scan results from kernel cache */
    s_scan_res.count = 0;
    s_scan_res.max_entries = WIFI_MAX_SCAN_ENTRIES;
    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &s_scan_res, sizeof(s_scan_res));

    if (rc != 0 || s_scan_res.count == 0) {
        /* Extra dwell in case cold hardware was slow to populate */
        timespec_t ts_retry = { .tv_sec = 0, .tv_nsec = 600000000 }; /* 600ms */
        WIFI_SYSCALL(SYS_NANOSLEEP, &ts_retry, 0, 0);
        rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &s_scan_res, sizeof(s_scan_res));
    }

    /* 4. Find visible saved network with highest RSSI */
    int best_saved_idx = -1;
    int8_t best_rssi = -128;

    if (rc == 0 && s_scan_res.count > 0) {
        for (int s = 0; s < count; s++) {
            for (uint32_t v = 0; v < s_scan_res.count; v++) {
                if (streq(s_saved_nets[s].ssid, s_scan_res.entries[v].ssid)) {
                    if (s_scan_res.entries[v].rssi > best_rssi) {
                        best_rssi = s_scan_res.entries[v].rssi;
                        best_saved_idx = s;
                    }
                }
            }
        }
    }

    /* 5. Connect to best saved network, or fallback to saved networks in order */
    if (best_saved_idx >= 0) {
        write_str(1, "[wifi] Auto-connecting to saved network \"");
        write_str(1, s_saved_nets[best_saved_idx].ssid);
        write_str(1, "\" (signal: ");
        size_t p = 0;
        append_int(s_out, &p, (int)best_rssi);
        s_out[p] = '\0';
        write_str(1, s_out);
        write_str(1, " dBm)...\n");

        return do_connect(s_saved_nets[best_saved_idx].ssid,
                          s_saved_nets[best_saved_idx].psk,
                          s_saved_nets[best_saved_idx].address,
                          s_saved_nets[best_saved_idx].gateway,
                          s_saved_nets[best_saved_idx].dns);
    } else {
        /* Fallback: try saved networks in order with directed connection (hidden APs / quiet beacons) */
        for (int s = 0; s < count; s++) {
            write_str(1, "[wifi] Trying saved network \"");
            write_str(1, s_saved_nets[s].ssid);
            write_str(1, "\"...\n");
            int conn_rc = do_connect(s_saved_nets[s].ssid,
                                     s_saved_nets[s].psk,
                                     s_saved_nets[s].address,
                                     s_saved_nets[s].gateway,
                                     s_saved_nets[s].dns);
            if (conn_rc == 0) return 0;
        }
        write_str(1, "[wifi] Auto-connect: could not connect to any saved network.\n");
        return 1;
    }
}

static bool looks_like_ip(const char *s) {
    if (!s) return false;
    bool has_dot = false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '.') has_dot = true;
        if (s[i] == '/') return true;
    }
    return has_dot && (s[0] >= '0' && s[0] <= '9');
}

int wifi_main(int argc, char **argv) {
    if (argc < 2 || streq(argv[1], "--help") || streq(argv[1], "-h") || streq(argv[1], "help")) {
        write_str(1, "Usage: wifi <command> [arguments]\n");
        write_str(1, "Commands:\n");
        write_str(1, "  scan                               Scan for nearby wireless networks\n");
        write_str(1, "  list                               List saved wireless networks\n");
        write_str(1, "  status                             Display wireless connection status\n");
        write_str(1, "  connect <SSID> [PSK] [--static ..] Connect to network (auto-saves profile)\n");
        write_str(1, "  static <IP/pfx> [GW] [DNS]         Set static IP on current Wi-Fi connection\n");
        write_str(1, "  static <SSID> <IP/pfx> [GW] [DNS]  Set static IP for a specific saved network\n");
        write_str(1, "  static dhcp                        Revert current Wi-Fi connection to DHCP\n");
        write_str(1, "  static <SSID> dhcp                 Revert specific saved network to DHCP\n");
        write_str(1, "  save <SSID> <PSK> [--static ...]   Save network to /etc/network/wlan.conf\n");
        write_str(1, "  forget [SSID]                      Remove saved network from /etc/network/wlan.conf\n");
        write_str(1, "  auto                               Auto-connect to best saved wireless network\n");
        write_str(1, "  disconnect                         Disconnect from wireless network\n");
        return (argc < 2) ? 1 : 0;
    }

    if (streq(argv[1], "scan")) {
        return do_scan();
    } else if (streq(argv[1], "status")) {
        return do_status();
    } else if (streq(argv[1], "list")) {
        return do_list();
    } else if (streq(argv[1], "load-fw") || streq(argv[1], "init")) {
        return ensure_firmware_loaded() ? 0 : 1;
    } else if (streq(argv[1], "disconnect")) {
        return do_disconnect();
    } else if (streq(argv[1], "auto")) {
        return do_auto();
    } else if (streq(argv[1], "save")) {
        if (argc < 4) {
            write_str(2, "Usage: wifi save <SSID> <passphrase> [--static <IP/prefix> [gateway] [dns]]\n");
            return 1;
        }
        const char *ssid = argv[2];
        const char *psk = argv[3];
        const char *addr = NULL;
        const char *gw = NULL;
        const char *dns = NULL;
        for (int i = 4; i < argc; i++) {
            if (streq(argv[i], "--static") && i + 1 < argc) {
                addr = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-') gw = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-') dns = argv[++i];
            }
        }
        return do_save(ssid, psk, addr, gw, dns);
    } else if (streq(argv[1], "forget") || streq(argv[1], "remove") || streq(argv[1], "delete")) {
        const char *ssid = NULL;
        static char s_cur_connected_ssid[WIFI_SSID_MAX_LEN + 1];
        if (argc >= 3) {
            ssid = argv[2];
        } else {
            /* If no SSID provided, target current connected network */
            wifi_status_t cur_st;
            if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &cur_st, sizeof(cur_st)) == 0 &&
                cur_st.state == WIFI_STATUS_CONNECTED && cur_st.ssid[0]) {
                size_t k = 0;
                while (cur_st.ssid[k] && k < WIFI_SSID_MAX_LEN) {
                    s_cur_connected_ssid[k] = cur_st.ssid[k];
                    k++;
                }
                s_cur_connected_ssid[k] = '\0';
                ssid = s_cur_connected_ssid;
            } else {
                write_str(2, "Usage: wifi forget <SSID>          (or 'wifi forget' while connected)\n");
                return 1;
            }
        }
        return do_forget(ssid);
    } else if (streq(argv[1], "static")) {
        if (argc < 3) {
            write_str(2, "Usage: wifi static <IP/prefix> [gateway] [dns]       (applies to current network)\n"
                         "       wifi static <SSID> <IP/prefix> [gateway] [dns]\n"
                         "       wifi static dhcp                             (reverts current network to DHCP)\n"
                         "       wifi static <SSID> dhcp\n");
            return 1;
        }

        const char *ssid = NULL;
        const char *addr = NULL;
        const char *gw = NULL;
        const char *dns = NULL;

        static char s_cur_connected_ssid[WIFI_SSID_MAX_LEN + 1];
        if (looks_like_ip(argv[2]) || streq(argv[2], "dhcp")) {
            /* Target current connected network */
            wifi_status_t cur_st;
            if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &cur_st, sizeof(cur_st)) == 0 &&
                cur_st.state == WIFI_STATUS_CONNECTED && cur_st.ssid[0]) {
                size_t k = 0;
                while (cur_st.ssid[k] && k < WIFI_SSID_MAX_LEN) {
                    s_cur_connected_ssid[k] = cur_st.ssid[k];
                    k++;
                }
                s_cur_connected_ssid[k] = '\0';
                ssid = s_cur_connected_ssid;
                addr = argv[2];
                gw = (argc >= 4) ? argv[3] : NULL;
                dns = (argc >= 5) ? argv[4] : NULL;
            } else {
                write_str(2, "wifi static: not currently connected to Wi-Fi. Please specify SSID:\n"
                             "Usage: wifi static <SSID> <IP/prefix> [gateway] [dns]\n");
                return 1;
            }
        } else {
            ssid = argv[2];
            addr = (argc >= 4) ? argv[3] : "dhcp";
            gw = (argc >= 5) ? argv[4] : NULL;
            dns = (argc >= 6) ? argv[5] : NULL;
        }

        return do_static(ssid, addr, gw, dns);
    } else if (streq(argv[1], "connect")) {
        if (argc < 3) {
            write_str(2, "Usage: wifi connect <SSID> [passphrase] [--static <IP/prefix> [gateway] [dns]]\n");
            return 1;
        }
        const char *ssid = argv[2];
        const char *psk = NULL;
        const char *addr = NULL;
        const char *gw = NULL;
        const char *dns = NULL;
        int arg_idx = 3;
        if (arg_idx < argc && !streq(argv[arg_idx], "--static")) {
            psk = argv[arg_idx++];
        }
        while (arg_idx < argc) {
            if (streq(argv[arg_idx], "--static") && arg_idx + 1 < argc) {
                addr = argv[++arg_idx];
                if (arg_idx + 1 < argc && argv[arg_idx + 1][0] != '-') gw = argv[++arg_idx];
                if (arg_idx + 1 < argc && argv[arg_idx + 1][0] != '-') dns = argv[++arg_idx];
            }
            arg_idx++;
        }
        return do_connect(ssid, psk, addr, gw, dns);
    } else {
        write_str(2, "wifi: unknown command '");
        write_str(2, argv[1]);
        write_str(2, "'. Run 'wifi' without arguments for help.\n");
        return 1;
    }
}
