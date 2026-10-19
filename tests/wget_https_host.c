#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_HOST 1

#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "tls.h"
#include "user/dns.h"
#include "user/dns_codec.h"
#include "user/wget_codec.h"

/* Forward declare mock syscall dispatcher */
static long host_wget_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                            uintptr_t d, uintptr_t e, uintptr_t f);

#define WGET_CALL host_wget_call

/* Mock DNS resolver mapping test domains to 127.0.0.1 */
int dns_resolve_ipv4(dns_context_t *ctx, const dns_options_t *opts,
                     const char *name, size_t name_len,
                     dns_result_t *out)
{
    (void)ctx; (void)opts; (void)name; (void)name_len;
    if (!out) return -1;
    out->address_count = 1;
    out->addresses[0] = inet_addr("127.0.0.1");
    return 0;
}

void dns_context_init(dns_context_t *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
}

const char *dns_status_name(int status) {
    return status == 0 ? "success" : "dns resolution error";
}

uint32_t dns_servers_configured(const dns_context_t *ctx) {
    (void)ctx;
    return 1;
}

/* Include user/wget_codec.c and user/wget.c directly */
#include "user/wget_codec.c"
#include "user/wget.c"

static uint64_t host_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static long host_wget_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                            uintptr_t d, uintptr_t e, uintptr_t f)
{
    (void)d; (void)f;
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const void *buf = (const void *)b;
        size_t count = (size_t)c;
        ssize_t n = write(fd, buf, count);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_SYSINFO) {
        sysinfo_t *info = (sysinfo_t *)a;
        if (info) {
            info->uptime_ticks = host_now_ms();
            info->tick_hz = 1000;
        }
        return 0;
    } else if (nr == SYS_GETRANDOM) {
        void *buf = (void *)a;
        size_t len = (size_t)b;
        FILE *rf = fopen("/dev/urandom", "rb");
        if (!rf) return -1;
        size_t r = fread(buf, 1, len, rf);
        fclose(rf);
        return (long)r;
    } else if (nr == SYS_SOCKET) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return -errno;
        return s;
    } else if (nr == SYS_CONNECT_UNTIL || nr == SYS_CONNECT) {
        int s = (int)a;
        const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)b;
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = in->port;
        sin.sin_addr.s_addr = in->address;
        if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            return -errno;
        }
        return 0;
    } else if (nr == SYS_SEND_UNTIL || nr == SYS_SEND) {
        int s = (int)a;
        const void *buf = (const void *)b;
        size_t len = (size_t)c;
        ssize_t n = send(s, buf, len, 0);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_RECV_UNTIL || nr == SYS_RECV || nr == SYS_READ) {
        int s = (int)a;
        void *buf = (void *)b;
        size_t cap = (size_t)c;
        uint64_t deadline = (uint64_t)e;
        if (deadline > 0) {
            uint64_t now = host_now_ms();
            if (now >= deadline) return -110; /* ETIMEDOUT */
            int timeout_ms = (int)(deadline - now);
            struct pollfd pfd = { .fd = s, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr == 0) return -110;
            if (pr < 0) return -errno;
        }
        ssize_t n = recv(s, buf, cap, 0);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_SHUTDOWN) {
        int s = (int)a;
        int how = ((int)b == NET_SHUT_WR) ? SHUT_WR : SHUT_RDWR;
        shutdown(s, how);
        return 0;
    } else if (nr == SYS_CLOSE) {
        close((int)a);
        return 0;
    } else if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        struct stat hst;
        if (stat(path, &hst) < 0) {
            return -errno;
        }
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (st) {
            memset(st, 0, sizeof(*st));
            st->size = (uint64_t)hst.st_size;
            st->type = S_ISDIR(hst.st_mode) ? VFS_DIRECTORY : VFS_FILE;
        }
        return 0;
    } else if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        int vflags = (int)b;
        mode_t mode = (mode_t)c;
        int pflags = 0;
        int acc = vflags & VFS_O_ACCMODE;
        if (acc == VFS_O_RDONLY) pflags |= O_RDONLY;
        else if (acc == VFS_O_WRONLY) pflags |= O_WRONLY;
        else if (acc == VFS_O_RDWR) pflags |= O_RDWR;

        if (vflags & VFS_O_CREAT) pflags |= O_CREAT;
        if (vflags & VFS_O_EXCL) pflags |= O_EXCL;
        if (vflags & VFS_O_TRUNC) pflags |= O_TRUNC;
#ifdef O_NOFOLLOW
        if (vflags & VFS_O_NOFOLLOW) pflags |= O_NOFOLLOW;
#endif
        int fd = open(path, pflags, mode);
        if (fd < 0) return -errno;
        return fd;
    } else if (nr == SYS_RENAME) {
        const char *oldp = (const char *)a;
        const char *newp = (const char *)b;
        if (rename(oldp, newp) < 0) {
            return -errno;
        }
        return 0;
    } else if (nr == SYS_UNLINK) {
        const char *p = (const char *)a;
        if (unlink(p) < 0) {
            return -errno;
        }
        return 0;
    }
    return -38; /* ENOSYS */
}

int main(int argc, char **argv) {
    return wget_main(argc, argv);
}
