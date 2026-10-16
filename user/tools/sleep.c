#include "common.h"

static bool parse_duration(const char *arg, uint64_t *out_sec, uint64_t *out_nsec) {
    if (!arg || !*arg) return false;

    uint64_t int_part = 0;
    uint64_t frac_part = 0;
    uint64_t frac_div = 1;
    bool has_digits = false;
    bool in_frac = false;

    size_t i = 0;
    while (arg[i]) {
        char c = arg[i];
        if (c >= '0' && c <= '9') {
            has_digits = true;
            if (!in_frac) {
                if (int_part > UINT64_MAX / 10) return false;
                int_part = int_part * 10 + (uint64_t)(c - '0');
            } else {
                if (frac_div < 1000000000ULL) {
                    frac_part = frac_part * 10 + (uint64_t)(c - '0');
                    frac_div *= 10;
                }
            }
            i++;
        } else if (c == '.') {
            if (in_frac) return false; /* multiple decimal points */
            in_frac = true;
            i++;
        } else {
            break; /* suffix reached */
        }
    }

    if (!has_digits) return false;

    /* Normalize fraction to nanoseconds */
    uint64_t nsec = 0;
    if (frac_div > 1) {
        nsec = (frac_part * 1000000000ULL) / frac_div;
    }

    /* Check suffix */
    uint64_t multiplier = 1;
    if (arg[i] != '\0') {
        char suffix = arg[i];
        if (arg[i + 1] != '\0') return false; /* trailing junk */
        if (suffix == 's' || suffix == 'S') {
            multiplier = 1;
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 60;
        } else if (suffix == 'h' || suffix == 'H') {
            multiplier = 3600;
        } else if (suffix == 'd' || suffix == 'D') {
            multiplier = 86400;
        } else {
            return false;
        }
    }

    /* Apply multiplier */
    if (int_part > UINT64_MAX / multiplier) return false;
    uint64_t total_sec = int_part * multiplier;

    /* Multiply fractional nanoseconds */
    if (multiplier > 1 && nsec > 0) {
        uint64_t total_ns = nsec * multiplier;
        total_sec += total_ns / 1000000000ULL;
        nsec = total_ns % 1000000000ULL;
    }

    *out_sec = total_sec;
    *out_nsec = nsec;
    return true;
}

int sleep_main(int argc, char **argv) {
    if (argc < 2) {
        return tool_error("sleep", "missing operand", NULL);
    }

    uint64_t total_sec = 0;
    uint64_t total_nsec = 0;

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--help") || tool_equal(argv[i], "-h")) {
            const char *help =
                "usage: sleep NUMBER[SUFFIX]...\n"
                "Pause for NUMBER seconds. SUFFIX may be 's' for seconds (default),\n"
                "'m' for minutes, 'h' for hours or 'd' for days.\n";
            return tool_write("sleep", help, tool_length(help));
        }

        uint64_t s = 0, ns = 0;
        if (!parse_duration(argv[i], &s, &ns)) {
            return tool_error("sleep", "invalid time interval", argv[i]);
        }

        total_sec += s;
        total_nsec += ns;
        if (total_nsec >= 1000000000ULL) {
            total_sec += total_nsec / 1000000000ULL;
            total_nsec %= 1000000000ULL;
        }
    }

    timespec_t req;
    req.tv_sec = (int64_t)total_sec;
    req.tv_nsec = (int64_t)total_nsec;

    timespec_t rem;
    long ret = tool_syscall(SYS_NANOSLEEP, (uintptr_t)&req, (uintptr_t)&rem, 0);
    if (ret < 0) {
        if (ret == SYSCALL_EINTR) {
            return 130; /* Interrupted by signal */
        }
        return tool_sys_error("sleep", "nanosleep failed", NULL, ret);
    }

    return 0;
}
