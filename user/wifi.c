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
        if (st.link_online || st.state != WIFI_STATUS_IDLE) {
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
    wifi_scan_result_t trigger_res = { .count = 0, .max_entries = 0 };
    (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &trigger_res, sizeof(trigger_res));

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

    wifi_scan_result_t res;
    res.count = 0;
    res.max_entries = WIFI_MAX_SCAN_ENTRIES;

    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &res, sizeof(res));
    if (rc != 0) {
        write_str(2, "wifi: scan failed\n");
        return 1;
    }

    if (res.count == 0) {
        write_str(1, "No wireless networks found in range.\n");
        return 0;
    }

    write_str(1, "BSSID              CHAN  RSSI   SEC       SSID\n");
    write_str(1, "-----------------  ----  -----  --------  --------------------------------\n");

    for (uint32_t i = 0; i < res.count; i++) {
        const wifi_scan_entry_t *e = &res.entries[i];
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
    write_str(1, "Disconnected from wireless network.\n");
    return 0;
}

static int do_connect(const char *ssid, const char *passphrase) {
    if (!ssid || !passphrase) {
        write_str(2, "wifi connect: requires <SSID> and <passphrase>\n");
        return 1;
    }

    ensure_firmware_loaded();

    wifi_connect_req_t req;
    for (size_t i = 0; i < sizeof(req.ssid); i++) req.ssid[i] = 0;
    for (size_t i = 0; i < sizeof(req.passphrase); i++) req.passphrase[i] = 0;
    for (size_t i = 0; i < 6; i++) req.bssid[i] = 0;

    size_t s_len = mystrlen(ssid);
    if (s_len > WIFI_SSID_MAX_LEN) s_len = WIFI_SSID_MAX_LEN;
    for (size_t i = 0; i < s_len; i++) req.ssid[i] = ssid[i];

    size_t p_len = mystrlen(passphrase);
    if (p_len > WIFI_PASSPHRASE_MAX_LEN) p_len = WIFI_PASSPHRASE_MAX_LEN;
    for (size_t i = 0; i < p_len; i++) req.passphrase[i] = passphrase[i];

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
                } else {
                    write_str(2, "wifi: failed to spawn /sbin/dhcpc\n");
                }
                return 0;
            }
        }
        timespec_t ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        WIFI_SYSCALL(SYS_NANOSLEEP, &ts, 0, 0);
    }

    if (WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_STATUS, &st, sizeof(st)) == 0) {
        if (st.state == WIFI_STATUS_FAILED || st.state == WIFI_STATUS_IDLE) {
            write_str(2, "wifi: connection failed (authentication rejected or timeout)\n");
            (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
            return 1;
        }
        write_str(1, "Connection in progress (current stage: ");
        write_str(1, state_to_str(st.state));
        write_str(1, "). Check 'wifi status'.\n");
    } else {
        write_str(2, "wifi: connection timed out\n");
        (void)WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_DISCONNECT, 0, 0);
        return 1;
    }
    return 0;
}

#define MAX_SAVED_NETWORKS 8

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char psk[WIFI_PASSPHRASE_MAX_LEN + 1];
} saved_net_t;

static saved_net_t s_saved_nets[MAX_SAVED_NETWORKS];
static int         s_saved_count = 0;

static int load_saved_networks(void) {
    s_saved_count = 0;
    int fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wlan.conf", 0, 0);
    if (fd < 0) {
        fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wifi.conf", 0, 0);
    }
    if (fd < 0) return 0;

    char buf[1024];
    long rd = WIFI_SYSCALL(SYS_READ, fd, buf, sizeof(buf) - 1);
    WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
    if (rd <= 0) return 0;
    buf[rd] = '\0';

    char *line = buf;
    char cur_ssid[WIFI_SSID_MAX_LEN + 1] = {0};

    while (*line && s_saved_count < MAX_SAVED_NETWORKS) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
        if (!*line) break;
        char *line_end = line;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
        char saved_ch = *line_end;
        *line_end = '\0';

        if (*line != '#') {
            if (line[0] == 's' && line[1] == 's' && line[2] == 'i' && line[3] == 'd' && line[4] == '=') {
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
                char cur_psk[WIFI_PASSPHRASE_MAX_LEN + 1] = {0};
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
                if (cur_ssid[0] && cur_psk[0] && s_saved_count < MAX_SAVED_NETWORKS) {
                    size_t i = 0;
                    while (cur_ssid[i] && i < WIFI_SSID_MAX_LEN) {
                        s_saved_nets[s_saved_count].ssid[i] = cur_ssid[i];
                        i++;
                    }
                    s_saved_nets[s_saved_count].ssid[i] = '\0';

                    size_t j = 0;
                    while (cur_psk[j] && j < WIFI_PASSPHRASE_MAX_LEN) {
                        s_saved_nets[s_saved_count].psk[j] = cur_psk[j];
                        j++;
                    }
                    s_saved_nets[s_saved_count].psk[j] = '\0';
                    s_saved_count++;
                    cur_ssid[0] = '\0';
                }
            } else if (line[0] == 'n' && line[1] == 'e' && line[2] == 't' && line[3] == 'w' &&
                       line[4] == 'o' && line[5] == 'r' && line[6] == 'k' && line[7] == '=') {
                /* Format: network=SSID:PSK */
                const char *p = line + 8;
                char s_buf[WIFI_SSID_MAX_LEN + 1] = {0};
                char p_buf[WIFI_PASSPHRASE_MAX_LEN + 1] = {0};
                size_t k = 0;
                while (*p && *p != ':' && k < WIFI_SSID_MAX_LEN) s_buf[k++] = *p++;
                s_buf[k] = '\0';
                if (*p == ':') p++;
                size_t m = 0;
                while (*p && m < WIFI_PASSPHRASE_MAX_LEN) p_buf[m++] = *p++;
                p_buf[m] = '\0';
                if (s_buf[0] && p_buf[0] && s_saved_count < MAX_SAVED_NETWORKS) {
                    for (size_t x = 0; x <= k; x++) s_saved_nets[s_saved_count].ssid[x] = s_buf[x];
                    for (size_t x = 0; x <= m; x++) s_saved_nets[s_saved_count].psk[x] = p_buf[x];
                    s_saved_count++;
                }
            }
        }

        if (saved_ch == '\0') break;
        line = line_end + 1;
    }

    return s_saved_count;
}

static int do_save(const char *ssid, const char *passphrase) {
    if (!ssid || !passphrase) {
        write_str(2, "Usage: wifi save <SSID> <passphrase>\n");
        return 1;
    }
    size_t slen = mystrlen(ssid);
    size_t plen = mystrlen(passphrase);
    if (slen == 0 || slen > WIFI_SSID_MAX_LEN || plen < 8 || plen > WIFI_PASSPHRASE_MAX_LEN) {
        write_str(2, "wifi save: invalid SSID or passphrase length (8-63 chars)\n");
        return 1;
    }

    (void)WIFI_SYSCALL(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0);

    /* Read existing file or create new */
    char content[1024];
    size_t pos = 0;

    int fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wlan.conf", 0, 0);
    if (fd >= 0) {
        long rd = WIFI_SYSCALL(SYS_READ, fd, content, sizeof(content) - 256);
        WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);
        if (rd > 0) pos = (size_t)rd;
    } else {
        const char *hdr = "# /etc/network/wlan.conf - FortressOS Wireless Configuration\n";
        while (*hdr) content[pos++] = *hdr++;
    }

    if (pos > 0 && content[pos - 1] != '\n') content[pos++] = '\n';

    /* Append network=SSID:PSK */
    const char *pfx = "network=";
    while (*pfx) content[pos++] = *pfx++;
    for (size_t i = 0; i < slen; i++) content[pos++] = ssid[i];
    content[pos++] = ':';
    for (size_t i = 0; i < plen; i++) content[pos++] = passphrase[i];
    content[pos++] = '\n';
    content[pos] = '\0';

    /* Write to /etc/network/wlan.conf using O_WRONLY | O_CREAT | O_TRUNC (0x241) */
    fd = (int)WIFI_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/network/wlan.conf", 0x241, 0644);
    if (fd < 0) {
        write_str(2, "wifi save: failed to open /etc/network/wlan.conf for writing\n");
        return 1;
    }
    long wr = WIFI_SYSCALL(SYS_WRITE, fd, content, pos);
    WIFI_SYSCALL(SYS_CLOSE, fd, 0, 0);

    if (wr > 0) {
        write_str(1, "Saved wireless network \"");
        write_str(1, ssid);
        write_str(1, "\" to /etc/network/wlan.conf\n");
        return 0;
    } else {
        write_str(2, "wifi save: write error\n");
        return 1;
    }
}

static int do_auto(void) {
    if (!ensure_firmware_loaded()) {
        return 0;
    }

    int count = load_saved_networks();
    if (count <= 0) {
        return 0; /* No saved networks configured */
    }

    /* Perform scan to see which saved networks are in range */
    wifi_scan_result_t res;
    res.count = 0;
    res.max_entries = WIFI_MAX_SCAN_ENTRIES;

    long rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &res, sizeof(res));
    if (rc != 0 || res.count == 0) {
        /* Probe sent; sleep briefly and re-query */
        timespec_t ts = { .tv_sec = 0, .tv_nsec = 500000000 }; /* 500ms */
        WIFI_SYSCALL(SYS_NANOSLEEP, &ts, 0, 0);
        rc = WIFI_SYSCALL(SYS_WIFI, WIFI_CMD_SCAN, &res, sizeof(res));
    }

    if (rc != 0 || res.count == 0) {
        write_str(1, "[wifi] Auto-connect: no networks detected in range.\n");
        return 0;
    }

    /* Find visible saved network with highest RSSI */
    int best_saved_idx = -1;
    int8_t best_rssi = -128;

    for (int s = 0; s < count; s++) {
        for (uint32_t v = 0; v < res.count; v++) {
            if (streq(s_saved_nets[s].ssid, res.entries[v].ssid)) {
                if (res.entries[v].rssi > best_rssi) {
                    best_rssi = res.entries[v].rssi;
                    best_saved_idx = s;
                }
            }
        }
    }

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
                          s_saved_nets[best_saved_idx].psk);
    } else {
        write_str(1, "[wifi] Auto-connect: none of the saved networks are in range.\n");
        return 0;
    }
}

int wifi_main(int argc, char **argv) {
    if (argc < 2) {
        write_str(1, "Usage: wifi <command> [arguments]\n");
        write_str(1, "Commands:\n");
        write_str(1, "  scan                  Scan for nearby wireless networks\n");
        write_str(1, "  connect <SSID> <PSK>  Connect to WPA2-PSK network\n");
        write_str(1, "  save <SSID> <PSK>     Save network credentials to /etc/network/wlan.conf\n");
        write_str(1, "  auto                  Auto-connect to best saved wireless network\n");
        write_str(1, "  disconnect            Disconnect from wireless network\n");
        write_str(1, "  status                Display wireless connection status\n");
        return 1;
    }

    if (streq(argv[1], "scan")) {
        return do_scan();
    } else if (streq(argv[1], "status")) {
        return do_status();
    } else if (streq(argv[1], "load-fw") || streq(argv[1], "init")) {
        return ensure_firmware_loaded() ? 0 : 1;
    } else if (streq(argv[1], "disconnect")) {
        return do_disconnect();
    } else if (streq(argv[1], "auto")) {
        return do_auto();
    } else if (streq(argv[1], "save")) {
        if (argc < 4) {
            write_str(2, "Usage: wifi save <SSID> <passphrase>\n");
            return 1;
        }
        return do_save(argv[2], argv[3]);
    } else if (streq(argv[1], "connect")) {
        if (argc < 4) {
            write_str(2, "Usage: wifi connect <SSID> <passphrase>\n");
            return 1;
        }
        return do_connect(argv[2], argv[3]);
    } else {
        write_str(2, "wifi: unknown command '");
        write_str(2, argv[1]);
        write_str(2, "'. Run 'wifi' without arguments for help.\n");
        return 1;
    }
}
