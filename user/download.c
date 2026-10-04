#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "udp_common.h"
#include "wget.h"

#ifndef DOWNLOAD_CALL
#define DOWNLOAD_CALL udp_call
#endif

static void message(const char *text) {
    size_t left = udp_length(text);
    while (left) {
        long n = DOWNLOAD_CALL(SYS_WRITE, 2, (uintptr_t)text, left, 0, 0, 0);
        if (n <= 0 || (size_t)n > left) break;
        text += n;
        left -= (size_t)n;
    }
}

static void print_usage(void) {
    message("usage: download [destination] <URL>\n"
            "       download <URL> [destination]\n\n"
            "Simple file downloader for FortressOS.\n"
            "options:\n"
            "  -q, --quiet      quiet mode\n"
            "  -s <server-ip>   override DNS server IPv4 address\n"
            "  -h, --help       display this help and exit\n\n"
            "If destination is a directory (such as /mnt), the remote filename is appended.\n"
            "If destination is omitted, /mnt is used if mounted, otherwise current directory.\n");
}

static bool is_url(const char *s) {
    if (!s) return false;
    if (s[0] == '/' || (s[0] == '.' && (s[1] == '/' || s[1] == '\0'))) return false;
    if (s[0] == '-' && s[1] == '\0') return false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == ':' && s[i+1] == '/' && s[i+2] == '/') return true;
    }
    return true;
}

int download_main(int argc, char **argv) {
    const char *url = NULL;
    const char *dest = NULL;
    bool quiet = false;
    uint32_t dns_override = 0;
    bool has_dns_override = false;
    const char *pos_args[4];
    int pos_count = 0;

    for (int i = 1; i < argc; i++) {
        if (udp_equal(argv[i], "-h") || udp_equal(argv[i], "--help")) {
            print_usage();
            return 0;
        } else if (udp_equal(argv[i], "-q") || udp_equal(argv[i], "--quiet")) {
            quiet = true;
        } else if (udp_equal(argv[i], "-s")) {
            if (i + 1 >= argc || !udp_ip(argv[i + 1], &dns_override)) {
                message("download: option -s requires a valid IPv4 address\n");
                print_usage();
                return 1;
            }
            has_dns_override = true;
            i++;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            message("download: unrecognized option '");
            message(argv[i]);
            message("'\n");
            print_usage();
            return 1;
        } else {
            if (pos_count < 4) {
                pos_args[pos_count++] = argv[i];
            } else {
                message("download: too many arguments\n");
                print_usage();
                return 1;
            }
        }
    }

    if (pos_count == 0) {
        print_usage();
        return 1;
    } else if (pos_count == 1) {
        if (is_url(pos_args[0])) {
            url = pos_args[0];
        } else {
            message("download: missing URL\n");
            print_usage();
            return 1;
        }
    } else if (pos_count == 2) {
        if (is_url(pos_args[0]) && !is_url(pos_args[1])) {
            url = pos_args[0];
            dest = pos_args[1];
        } else if (!is_url(pos_args[0]) && is_url(pos_args[1])) {
            dest = pos_args[0];
            url = pos_args[1];
        } else {
            url = pos_args[0];
            dest = pos_args[1];
        }
    } else {
        message("download: too many arguments\n");
        print_usage();
        return 1;
    }

    if (!dest) {
        /* Check if /mnt exists and is a directory */
        vfs_stat_t st;
        if (DOWNLOAD_CALL(SYS_STAT, (uintptr_t)"/mnt", (uintptr_t)&st, 0, 0, 0, 0) == 0 &&
            st.type == VFS_DIRECTORY) {
            dest = "/mnt";
        }
    }

    return wget_run(url, dest, quiet, dns_override, has_dns_override);
}
