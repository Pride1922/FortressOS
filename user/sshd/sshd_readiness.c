#include "sshd_readiness.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "hostkey_abi.h"
#include "ssh_hostkey.h"
#include "crypto.h"
#include "string.h"

#ifndef SSHD_SYSCALL
static inline long sshd_readiness_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                          uintptr_t d, uintptr_t e, uintptr_t f) {
#ifndef CRYPTO_HOST_TEST
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
#else
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -1;
#endif
}
#define SSHD_SYSCALL sshd_readiness_syscall
#endif

static void readiness_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static size_t uint_to_str(char *buf, uint32_t val) {
    char tmp[16];
    size_t i = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    buf[i] = '\0';
    return i;
}

void sshd_format_bind_error(char *dst, size_t cap, uint32_t ip, uint16_t port) {
    if (!dst || cap == 0) return;
    const char prefix[] = "bind failed on ";
    size_t pos = 0;
    for (size_t i = 0; prefix[i] && pos + 1 < cap; i++) dst[pos++] = prefix[i];

    uint32_t octets[4] = { ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff };
    for (int i = 0; i < 4; i++) {
        char num[16];
        size_t nlen = uint_to_str(num, octets[i]);
        for (size_t j = 0; j < nlen && pos + 1 < cap; j++) dst[pos++] = num[j];
        if (i < 3 && pos + 1 < cap) dst[pos++] = '.';
    }
    if (pos + 1 < cap) dst[pos++] = ':';
    char port_buf[16];
    size_t plen = uint_to_str(port_buf, port);
    for (size_t j = 0; j < plen && pos + 1 < cap; j++) dst[pos++] = port_buf[j];
    dst[pos] = '\0';
}

static int default_load_host_key(uint8_t out_secret[64], uint8_t out_public[32]) {
#ifndef CRYPTO_HOST_TEST
    static char key_buf[HOSTKEY_FILE_MAX];
    long len = SSHD_SYSCALL(SYS_HOSTKEY, HOSTKEY_READ, (uintptr_t)key_buf, sizeof(key_buf), 0, 0, 0);
    if (len <= 0) {
        /* Fallback: attempt to read /etc/ssh/ssh_host_ed25519_key or /mnt/.fortress/ssh_host_ed25519_key */
        long fd = SSHD_SYSCALL(SYS_OPEN, (uintptr_t)"/etc/ssh/ssh_host_ed25519_key", VFS_O_RDONLY | VFS_O_CLOEXEC, 0, 0, 0, 0);
        if (fd < 0) {
            fd = SSHD_SYSCALL(SYS_OPEN, (uintptr_t)"/mnt/.fortress/ssh_host_ed25519_key", VFS_O_RDONLY | VFS_O_CLOEXEC, 0, 0, 0, 0);
        }
        if (fd >= 0) {
            len = SSHD_SYSCALL(SYS_READ, (uintptr_t)fd, (uintptr_t)key_buf, sizeof(key_buf) - 1, 0, 0, 0);
            (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        }
    }

    if (len <= 0) return -1;
    if ((size_t)len >= sizeof(key_buf)) len = sizeof(key_buf) - 1;
    key_buf[len] = '\0';

    static ssh_hostkey_workspace ws;
    int r = ssh_hostkey_decode(key_buf, (size_t)len, out_secret, &ws);
    if (r != 0) return -1;

    memcpy(out_public, out_secret + 32, 32);
    crypto_wipe(&ws, sizeof(ws));
    return 0;
#else
    (void)out_secret; (void)out_public;
    return -1;
#endif
}

int sshd_read_host_public_key(uint8_t out_public[32]) {
#ifndef CRYPTO_HOST_TEST
    static uint8_t s_sec[64];
    int r = default_load_host_key(s_sec, out_public);
    crypto_wipe(s_sec, sizeof(s_sec));
    return r;
#else
    (void)out_public;
    return -1;
#endif
}

static long default_bind_socket(uint32_t listen_addr, uint16_t port, char *err_msg, size_t err_cap) {
#ifndef CRYPTO_HOST_TEST
    long sock_fd = SSHD_SYSCALL(SYS_SOCKET, NET_AF_INET, NET_SOCK_STREAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (sock_fd < 0) {
        if (err_msg && err_cap > 0) {
            sshd_format_bind_error(err_msg, err_cap, listen_addr, port);
        }
        return -1;
    }

    net_sockaddr_in_t sin;
    memset(&sin, 0, sizeof(sin));
    sin.family = NET_AF_INET;
    sin.port = ((port & 0xff) << 8) | ((port >> 8) & 0xff); /* htons */
    sin.address = listen_addr;

    long err = SSHD_SYSCALL(SYS_BIND, (uintptr_t)sock_fd, (uintptr_t)&sin, sizeof(sin), 0, 0, 0);
    if (err != 0) {
        if (err_msg && err_cap > 0) {
            sshd_format_bind_error(err_msg, err_cap, listen_addr, port);
        }
        (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)sock_fd, 0, 0, 0, 0, 0);
        return -1;
    }

    return sock_fd;
#else
    if (err_msg && err_cap > 0) {
        sshd_format_bind_error(err_msg, err_cap, listen_addr, port);
    }
    return -1;
#endif
}

static int default_listen_socket(long sock_fd) {
#ifndef CRYPTO_HOST_TEST
    return (int)SSHD_SYSCALL(SYS_LISTEN, (uintptr_t)sock_fd, 2, 0, 0, 0, 0);
#else
    (void)sock_fd;
    return -1;
#endif
}

int sshd_check_readiness(const char *cfg_path, const sshd_readiness_ops_t *ops,
                         sshd_config_t *out_cfg, uint8_t out_host_secret[64],
                         userdb_t *out_db, long *out_sock_fd,
                         char *out_diag, size_t diag_cap, bool *out_disabled) {
    if (out_disabled) *out_disabled = false;
    if (out_diag && diag_cap > 0) out_diag[0] = '\0';
    if (out_sock_fd) *out_sock_fd = -1;

    /* Step 1: Verify installed root vs. live media & Read config */
    static sshd_config_t cfg;
    static char cfg_err[128];
    memset(cfg_err, 0, sizeof(cfg_err));
    int cfg_rc;
    if (ops && ops->load_config) {
        cfg_rc = ops->load_config(cfg_path, &cfg, cfg_err, sizeof(cfg_err));
    } else {
        cfg_rc = sshd_config_load(cfg_path, &cfg, cfg_err, sizeof(cfg_err));
    }

    if (cfg_rc != 0) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, cfg_err[0] ? cfg_err : "Configuration error", diag_cap);
        }
        return 1;
    }
    if (out_cfg) *out_cfg = cfg;

    /* Read config -> if Enabled no, exit cleanly */
    if (!cfg.enabled) {
        if (out_disabled) *out_disabled = true;
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, "Service disabled in configuration.", diag_cap);
        }
        return 0;
    }

    /* Step 2: Load host key via SYS_HOSTKEY -> fail = "host key unavailable/corrupt" */
    static uint8_t host_secret[64];
    static uint8_t host_public[32];
    int hk_rc;
    if (ops && ops->load_host_key) {
        hk_rc = ops->load_host_key(host_secret, host_public);
    } else {
        hk_rc = default_load_host_key(host_secret, host_public);
    }

    if (hk_rc != 0) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, "host key unavailable/corrupt", diag_cap);
        }
        return 1;
    }
    if (out_host_secret) {
        memcpy(out_host_secret, host_secret, 64);
    }

    /* Step 3: Load account database (userdb_t) -> fail = "account database unavailable" */
    static userdb_t s_check_db;
    userdb_t *db_ptr = out_db ? out_db : &s_check_db;
    bool db_ok;
    if (ops && ops->load_userdb) {
        db_ok = ops->load_userdb(db_ptr, true);
    } else {
#ifndef CRYPTO_HOST_TEST
        db_ok = db_load(db_ptr, true);
#else
        db_ok = false;
#endif
    }

    if (!db_ok) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, "account database unavailable", diag_cap);
        }
        if (out_host_secret) crypto_wipe(out_host_secret, 64);
        crypto_wipe(host_secret, sizeof(host_secret));
        return 1;
    }
    if (out_db && out_db != db_ptr) *out_db = *db_ptr;

    /* Step 4: Verify SYS_GETRANDOM succeeds -> fail = "strong randomness unavailable" (fails closed) */
    static uint8_t probe_rnd[16];
    long rnd_rc;
    if (ops && ops->getrandom) {
        rnd_rc = ops->getrandom(probe_rnd, sizeof(probe_rnd));
    } else {
#ifndef CRYPTO_HOST_TEST
        rnd_rc = SSHD_SYSCALL(SYS_GETRANDOM, (uintptr_t)probe_rnd, sizeof(probe_rnd), 0, 0, 0, 0);
#else
        rnd_rc = -1;
#endif
    }

    if (rnd_rc != (long)sizeof(probe_rnd)) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, "strong randomness unavailable", diag_cap);
        }
        crypto_wipe(probe_rnd, sizeof(probe_rnd));
        if (out_host_secret) crypto_wipe(out_host_secret, 64);
        crypto_wipe(host_secret, sizeof(host_secret));
        return 1;
    }
    crypto_wipe(probe_rnd, sizeof(probe_rnd));

    /* Step 5: Bind port 22 -> fail = "bind failed on <addr>:<port>" */
    long sock_fd = -1;
    static char bind_err[128];
    memset(bind_err, 0, sizeof(bind_err));
    if (ops && ops->bind_socket) {
        sock_fd = ops->bind_socket(cfg.listen_address, cfg.port, bind_err, sizeof(bind_err));
    } else {
        sock_fd = default_bind_socket(cfg.listen_address, cfg.port, bind_err, sizeof(bind_err));
    }

    if (sock_fd < 0) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, bind_err[0] ? bind_err : "bind failed", diag_cap);
        }
        if (out_host_secret) crypto_wipe(out_host_secret, 64);
        crypto_wipe(host_secret, sizeof(host_secret));
        return 1;
    }

    /* Step 6: Listen on socket */
    int listen_rc;
    if (ops && ops->listen_socket) {
        listen_rc = ops->listen_socket(sock_fd);
    } else {
        listen_rc = default_listen_socket(sock_fd);
    }

    if (listen_rc != 0) {
        if (out_diag && diag_cap > 0) {
            readiness_copy(out_diag, "Failed to listen on TCP socket.", diag_cap);
        }
#ifndef CRYPTO_HOST_TEST
        (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)sock_fd, 0, 0, 0, 0, 0);
#endif
        if (out_host_secret) crypto_wipe(out_host_secret, 64);
        crypto_wipe(host_secret, sizeof(host_secret));
        return 1;
    }

    if (out_sock_fd) *out_sock_fd = sock_fd;
    crypto_wipe(host_secret, sizeof(host_secret));
    return 0;
}
