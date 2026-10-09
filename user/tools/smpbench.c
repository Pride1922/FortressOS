#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "pipe_profile_abi.h"

#define MAX_WORKERS 32
#define DEFAULT_REPS 5
#define DEFAULT_ITERATIONS 10000000ULL /* 10 million loop iterations */

static char s_worker_path[64];
static char s_file_buf[4096];

enum profile_phase { P_SETUP, P_SPAWN, P_WAIT, P_SIGNAL, P_HEADER, P_READ, P_COMPUTE, P_CLOSE, P_WRITE, P_COUNT };
static const char *const phase_names[P_COUNT] = {"setup", "spawn", "wait", "signal", "header", "read", "compute", "close", "write"};
typedef struct {
    bool enabled, valid;
    uint64_t cycles[P_COUNT], maximum[P_COUNT], calls[P_COUNT];
} profile_t;

typedef struct {
    uint64_t magic, total, write, write_n, write_max, compute, compute_n, compute_max, other, valid;
    uint64_t queued_to_run, run_to_entry, startup_valid;
} pipe_profile_t;
#define PIPE_PROFILE_MAGIC 0x50524f46494c4532ULL
_Static_assert(sizeof(pipe_profile_t) == 104, "private pipe profile trailer layout");

static void profile_add(profile_t *profile, enum profile_phase phase, uint64_t begin, uint64_t end) {
    if (!profile->enabled) return;
    if (end < begin || UINT64_MAX - profile->cycles[phase] < end - begin) {
        profile->valid = false;
        return;
    }
    uint64_t delta = end - begin;
    profile->cycles[phase] += delta;
    if (delta > profile->maximum[phase]) profile->maximum[phase] = delta;
    profile->calls[phase]++;
}

static long profile_call(profile_t *profile, enum profile_phase phase, long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (!profile->enabled) return tool_syscall(nr, a, b, c);
    uint64_t begin = tool_rdtsc();
    long result = tool_syscall(nr, a, b, c);
    profile_add(profile, phase, begin, tool_rdtsc());
    return result;
}

static bool append_number(char *line, size_t *position, size_t capacity, const char *name, uint64_t value) {
    char number[32];
    size_t n = tool_format_u64(number, value), len = tool_length(name);
    /* Reserve both the eventual newline and terminating NUL. */
    if (*position >= capacity || len + n + 4 > capacity - *position) return false;
    line[(*position)++] = ' ';
    for (size_t i = 0; i < len; i++) line[(*position)++] = name[i];
    line[(*position)++] = '=';
    for (size_t i = 0; i < n; i++) line[(*position)++] = number[i];
    return true;
}

static void out_str(const char *s) {
    if (!s) return;
    tool_write("smpbench", s, tool_length(s));
}

static void out_u64(uint64_t val) {
    char buf[32];
    tool_format_u64(buf, val);
    out_str(buf);
}

static void copy_str(char *dest, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static uint32_t get_current_apic_id(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x0B) {
        cpuid(0x0B, 0, &eax, &ebx, &ecx, &edx);
        if (edx != 0 || ebx != 0) {
            return edx; /* 32-bit x2APIC ID */
        }
    }
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF; /* Local APIC ID fallback */
}

static void detect_env(char *out, size_t max) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (ecx & (1u << 31)) {
        /* Hypervisor present */
        uint32_t sig[4] = {0};
        cpuid(0x40000000, 0, &eax, &sig[0], &sig[1], &sig[2]);
        char *s = (char *)sig;
        if (s[0] == 'T' && s[1] == 'C' && s[2] == 'G') {
            copy_str(out, "hv:TCG", max);
        } else if (s[0] == 'K' && s[1] == 'V' && s[2] == 'M') {
            copy_str(out, "hv:KVM", max);
        } else if (s[0] == 'V' && s[1] == 'M' && s[2] == 'w') {
            copy_str(out, "hv:VMware", max);
        } else if (s[0] == 'M' && s[1] == 'i' && s[2] == 'c') {
            copy_str(out, "hv:Hyper-V", max);
        } else {
            copy_str(out, "hv:TCG", max);
        }
    } else {
        uint32_t vendor[4] = {0};
        cpuid(0, 0, &eax, &vendor[0], &vendor[2], &vendor[1]);
        char *v = (char *)vendor;
        if (v[0] == 'G' && v[1] == 'e' && v[2] == 'n') {
            copy_str(out, "bare:Intel", max);
        } else if (v[0] == 'A' && v[1] == 'u' && v[2] == 't') {
            copy_str(out, "bare:AMD", max);
        } else {
            copy_str(out, "bare:unknown", max);
        }
    }
}

static void detect_fs(char *out, size_t max) {
    mount_info_t mi;
    for (uint32_t i = 0; i < 16; i++) {
        long r = tool_syscall(SYS_MOUNTINFO, i, (uintptr_t)&mi, 0);
        if (r <= 0) break;
        if (tool_equal(mi.mount_path, "/mnt") || tool_equal(mi.mount_path, "/")) {
            if (mi.fs_type == VFS_FS_EXT4) {
                copy_str(out, "ext4", max);
                return;
            } else if (mi.fs_type == VFS_FS_EXT2) {
                copy_str(out, "ext2", max);
                return;
            }
        }
    }
    copy_str(out, "ext2", max);
}

/* Deterministic integer computation and checksum loop */
static uint64_t cpu_scale_loop(uint64_t iterations, uint32_t *out_cpus_seen, uint32_t *out_apic_mask) {
    uint64_t state = 0x123456789abcdef0ULL;
    uint32_t mask = 0;

    uint32_t apic = get_current_apic_id();
    if (apic < 32) mask |= (1u << apic);

    uint64_t step = iterations / 16;
    if (step == 0) step = 1;

    for (uint64_t i = 0; i < iterations; i++) {
        state = state * 6364136223846793005ULL + (i + 1);
        state ^= (state >> 27);
        if ((i % step) == 0) {
            apic = get_current_apic_id();
            if (apic < 32) mask |= (1u << apic);
        }
    }

    apic = get_current_apic_id();
    if (apic < 32) mask |= (1u << apic);

    uint32_t count = 0;
    for (uint32_t m = mask; m > 0; m >>= 1) {
        if (m & 1) count++;
    }
    if (count == 0) count = 1;

    if (out_cpus_seen) *out_cpus_seen = count;
    if (out_apic_mask) *out_apic_mask = mask;
    return state;
}

static volatile uint64_t s_signals_count = 0;

static void sig_bench_handler(int sig) {
    (void)sig;
    s_signals_count++;
}

static void format_units_per_sec(char *out, size_t out_cap, uint64_t units_per_sec) {
    if (units_per_sec == 0) {
        copy_str(out, "0.00e0", out_cap);
        return;
    }
    uint64_t temp = units_per_sec;
    int exp = 0;
    while (temp >= 10) {
        temp /= 10;
        exp++;
    }
    uint64_t p10 = 1;
    for (int i = 0; i < exp; i++) p10 *= 10;
    uint64_t scaled = (units_per_sec * 100ULL) / p10;
    uint64_t d0 = scaled / 100;
    uint64_t d1 = (scaled / 10) % 10;
    uint64_t d2 = scaled % 10;

    size_t pos = 0;
    if (pos + 1 < out_cap) out[pos++] = '0' + (char)d0;
    if (pos + 1 < out_cap) out[pos++] = '.';
    if (pos + 1 < out_cap) out[pos++] = '0' + (char)d1;
    if (pos + 1 < out_cap) out[pos++] = '0' + (char)d2;
    if (pos + 1 < out_cap) out[pos++] = 'e';
    char exp_buf[16];
    tool_format_u64(exp_buf, (uint64_t)exp);
    for (size_t i = 0; exp_buf[i] && pos + 1 < out_cap; i++) {
        out[pos++] = exp_buf[i];
    }
    out[pos] = '\0';
}

static void format_ms_str(char *out, size_t max, uint64_t us) {
    uint64_t ms_x10 = (us + 50ULL) / 100ULL; /* round to nearest 0.1ms */
    uint64_t whole = ms_x10 / 10;
    uint64_t frac = ms_x10 % 10;
    char wbuf[32];
    size_t wlen = tool_format_u64(wbuf, whole);
    size_t pos = 0;
    for (size_t i = 0; i < wlen && pos + 1 < max; i++) out[pos++] = wbuf[i];
    if (pos + 1 < max) out[pos++] = '.';
    if (pos + 1 < max) out[pos++] = '0' + (char)frac;
    if (pos + 1 < max) out[pos++] = 'm';
    if (pos + 1 < max) out[pos++] = 's';
    out[pos] = '\0';
}

static bool parse_u64(const char *s, size_t *pos, uint64_t *out) {
    uint64_t v = 0;
    size_t i = *pos;
    if (s[i] < '0' || s[i] > '9') return false;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (uint64_t)(s[i] - '0');
        i++;
    }
    *pos = i;
    *out = v;
    return true;
}

static bool parse_hex_u64(const char *s, size_t *pos, uint64_t *out) {
    size_t i = *pos;
    if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) i += 2;
    uint64_t v = 0;
    size_t start = i;
    while ((s[i] >= '0' && s[i] <= '9') ||
           (s[i] >= 'a' && s[i] <= 'f') ||
           (s[i] >= 'A' && s[i] <= 'F')) {
        uint64_t d = 0;
        if (s[i] >= '0' && s[i] <= '9') d = (uint64_t)(s[i] - '0');
        else if (s[i] >= 'a' && s[i] <= 'f') d = (uint64_t)(s[i] - 'a' + 10);
        else if (s[i] >= 'A' && s[i] <= 'F') d = (uint64_t)(s[i] - 'A' + 10);
        v = (v << 4) | d;
        i++;
    }
    if (i == start) return false;
    *pos = i;
    *out = v;
    return true;
}

static bool parse_field(const char *line, const char *key, char *out_val, size_t max_val) {
    size_t klen = tool_length(key);
    const char *p = line;
    while (*p) {
        bool match = true;
        for (size_t i = 0; i < klen; i++) {
            if (p[i] != key[i]) {
                match = false;
                break;
            }
        }
        if (match && p[klen] == '=') {
            p += klen + 1;
            size_t idx = 0;
            while (*p && *p != ' ' && *p != '\n' && *p != '\r' && idx + 1 < max_val) {
                out_val[idx++] = *p++;
            }
            out_val[idx] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

static void make_worker_filepath(uint32_t id, char *out, size_t max) {
    char id_buf[16];
    tool_format_u64(id_buf, (uint64_t)id);
    const char *prefix = "/tmp/smpbench-";
    const char *suffix = ".out";
    size_t pos = 0;
    for (size_t i = 0; prefix[i] && pos + 1 < max; i++) out[pos++] = prefix[i];
    for (size_t i = 0; id_buf[i] && pos + 1 < max; i++) out[pos++] = id_buf[i];
    for (size_t i = 0; suffix[i] && pos + 1 < max; i++) out[pos++] = suffix[i];
    out[pos] = '\0';
}

static uint8_t s_pipe_buf[1024];

static uint64_t pipe_expected_checksum(uint64_t iters) {
    uint64_t cs = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < iters; i++) {
        uint8_t base = (uint8_t)(i * 37 + 13);
        for (size_t j = 0; j < 1024; j++) {
            uint8_t b = (uint8_t)(base + j);
            cs ^= b;
            cs *= 0x100000001b3ULL;
        }
    }
    return cs;
}

static int run_pipe_writer_batch(uint64_t iters, uint64_t write_fd, bool profiling, size_t batch) {
    static uint8_t buffer[4096];
    if ((batch != 1 && batch != 4) || !iters || iters > UINT64_MAX / 1024) return 2;
    uint64_t entry = profiling ? tool_rdtsc() : 0;
    spawn_profile_t birth = {0};
    bool startup_valid = profiling && tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_READ,
        (uintptr_t)&birth, sizeof(birth)) == 0 && birth.birth_valid == 1 && entry >= birth.first_run_cycles;
    profile_t profile = {.enabled = profiling, .valid = true};
    uint32_t my_apic = get_current_apic_id();
    uint32_t hdr[2] = { my_apic, (uint32_t)iters };
    size_t written = 0;
    while (written < sizeof(hdr)) {
        long n = tool_syscall(SYS_WRITE, write_fd, (uintptr_t)((uint8_t *)hdr + written), sizeof(hdr) - written);
        if (n <= 0) return 1;
        written += (size_t)n;
    }

    uint64_t start = profiling ? tool_rdtsc() : 0;
    for (uint64_t i = 0; i < iters;) {
        size_t units = iters - i < batch ? (size_t)(iters - i) : batch;
        uint64_t compute_start = profiling ? tool_rdtsc() : 0;
        for (size_t unit = 0; unit < units; ++unit) {
            uint8_t base = (uint8_t)((i + unit) * 37 + 13);
            for (size_t j = 0; j < 1024; j++) buffer[unit * 1024 + j] = (uint8_t)(base + j);
        }
        if (profiling) profile_add(&profile, P_COMPUTE, compute_start, tool_rdtsc());
        written = 0;
        while (written < units * 1024) {
            long n = profile_call(&profile, P_WRITE, SYS_WRITE, write_fd, (uintptr_t)(buffer + written), units * 1024 - written);
            if (n <= 0) return 1;
            written += (size_t)n;
        }
        i += units;
    }
    if (profiling) {
        uint64_t end = tool_rdtsc();
        uint64_t accounted = profile.cycles[P_WRITE] + profile.cycles[P_COMPUTE];
        bool valid = profile.valid && end >= start && accounted <= end - start &&
                     accounted >= profile.cycles[P_WRITE];
        pipe_profile_t trailer = {PIPE_PROFILE_MAGIC, end - start,
            profile.cycles[P_WRITE], profile.calls[P_WRITE], profile.maximum[P_WRITE],
            profile.cycles[P_COMPUTE], profile.calls[P_COMPUTE], profile.maximum[P_COMPUTE],
            valid ? end - start - accounted : 0, valid ? 1 : 0,
            startup_valid ? birth.first_run_cycles - birth.queued_cycles : 0,
            startup_valid ? entry - birth.first_run_cycles : 0, startup_valid ? 1 : 0};
        written = 0;
        while (written < sizeof(trailer)) {
            long n = tool_syscall(SYS_WRITE, write_fd, (uintptr_t)((uint8_t *)&trailer + written), sizeof(trailer) - written);
            if (n <= 0) return 1;
            written += (size_t)n;
        }
    }
    tool_syscall(SYS_CLOSE, write_fd, 0, 0);
    return 0;
}

static int run_pipe_writer(uint64_t iters, uint64_t fd, bool profiling) {
    return run_pipe_writer_batch(iters, fd, profiling, 1);
}

static bool read_writer_profile(profile_t *profile, uint64_t fd, pipe_profile_t *writer) {
    size_t received = 0;
    while (received < sizeof(*writer)) {
        long n = profile_call(profile, P_CLOSE, SYS_READ, fd,
            (uintptr_t)((uint8_t *)writer + received), sizeof(*writer) - received);
        if (n <= 0) return false;
        received += (size_t)n;
    }
    uint8_t extra = 0;
    long eof = profile_call(profile, P_CLOSE, SYS_READ, fd, (uintptr_t)&extra, 1);
    return eof == 0 && writer->magic == PIPE_PROFILE_MAGIC && writer->valid == 1;
}

/* Worker mode */
static int run_worker(int argc, char **argv) {
    /* Usage: --worker <id> <work> <iters> <ready-r> <ready-w> <gate-r> <gate-w>.
     * The internal pipe_writer mode instead takes its output descriptor. */
    if (argc < 5) return 2;
    size_t p = 0;
    uint64_t wid = 0;
    if (!parse_u64(argv[2], &p, &wid)) return 2;
    const char *work = argv[3];
    p = 0;
    uint64_t iters = DEFAULT_ITERATIONS;
    if (!parse_u64(argv[4], &p, &iters) || iters == 0) iters = DEFAULT_ITERATIONS;

    if (tool_equal(work, "pipe_writer") || tool_equal(work, "pipe_writer4k")) {
        if (argc != 6 && argc != 7) return 2;
        p = 0;
        uint64_t write_fd = 0;
        if (!parse_u64(argv[5], &p, &write_fd)) return 2;
        if (argc == 7 && !tool_equal(argv[6], "--profile")) return 2;
        return tool_equal(work, "pipe_writer4k") ? run_pipe_writer_batch(iters, write_fd, argc == 7, 4) : run_pipe_writer(iters, write_fd, argc == 7);
    }

    /* Parent passes both pipe pairs. Close inherited unused ends before ready,
     * so failed children cannot keep either EOF condition alive. */
    if ((argc != 9 && argc != 10) || wid >= MAX_WORKERS) return 2;
    profile_t profile = {.enabled = argc == 10 && tool_equal(argv[9], "--profile"), .valid = true};
    bool wait_enabled = argc == 10;
    if (wait_enabled && !profile.enabled && !tool_equal(argv[9], "--wait-profile")) return 2;
    if (profile.enabled && iters > UINT64_MAX / 1024 &&
        (tool_equal(work, "pipes") || tool_equal(work, "pipe_bw") || tool_equal(work, "pipes4k"))) return 2;
    uint64_t barrier_fd[4];
    for (size_t i = 0; i < 4; i++) {
        p = 0;
        if (!parse_u64(argv[5 + i], &p, &barrier_fd[i]) ||
            argv[5 + i][p] != '\0' || barrier_fd[i] >= 32) return 2;
    }
    tool_syscall(SYS_CLOSE, barrier_fd[0], 0, 0);
    tool_syscall(SYS_CLOSE, barrier_fd[3], 0, 0);
    uint8_t token = (uint8_t)wid;
    long ready = tool_syscall(SYS_WRITE, barrier_fd[1], (uintptr_t)&token, 1);
    tool_syscall(SYS_CLOSE, barrier_fd[1], 0, 0);
    long released = ready == 1 ? tool_syscall(SYS_READ, barrier_fd[2], (uintptr_t)&token, 1) : -1;
    tool_syscall(SYS_CLOSE, barrier_fd[2], 0, 0);
    if (released != 1 || token != 1) return 1;

    sysinfo_t si_start;
    tool_syscall(SYS_SYSINFO, (uintptr_t)&si_start, 0, 0);
    uint64_t tsc_hz = si_start.tsc_hz ? si_start.tsc_hz : 1000000000ULL;
    uint64_t tick_hz = si_start.tick_hz ? si_start.tick_hz : 100;

    proc_info_t own_start, own_end;
    bool have_cpu_start = tool_syscall(SYS_PROCINFO, PROC_INFO_SELF, (uintptr_t)&own_start, 0) == 1;
    spawn_profile_t spawn_detail = {0};
    wait_profile_t wait_detail = {0};
    /* Exercise diagnostic ABI rejection before the timed workload. Failed
     * requests must not toggle profiling or touch the supplied address. */
    if (profile.enabled && (
        tool_syscall(SYS_SPAWN_PROFILE, 6, (uintptr_t)&spawn_detail, sizeof(spawn_detail)) != SYSCALL_EINVAL ||
        tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE, (uintptr_t)&spawn_detail, 0) != SYSCALL_EINVAL ||
        tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE, 0, sizeof(spawn_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE, UINT64_MAX - 32, sizeof(spawn_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE, 0xffffffff80000000ULL, sizeof(spawn_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE, (uintptr_t)run_worker, sizeof(spawn_detail)) != SYSCALL_EFAULT)) return 1;
    if (profile.enabled && tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_ENABLE,
        (uintptr_t)&spawn_detail, sizeof(spawn_detail)) != 0) return 1;
    if (wait_enabled && (
        tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_ENABLE, (uintptr_t)&wait_detail, 0) != SYSCALL_EINVAL ||
        tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_ENABLE, 0, sizeof(wait_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_ENABLE, UINT64_MAX - 32, sizeof(wait_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_ENABLE, (uintptr_t)run_worker, sizeof(wait_detail)) != SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_ENABLE, (uintptr_t)&wait_detail, sizeof(wait_detail)) != 0)) return 1;
    uint64_t start_cycles = tool_rdtsc();
    uint64_t start_ticks = si_start.uptime_ticks;
    uint64_t time_us_start = (start_cycles * 1000000ULL) / tsc_hz;

    uint32_t cpus_seen = 0;
    uint32_t apic_mask = 0;
    uint64_t checksum = 0;
    pipe_profile_t writer_profile = {0};
    pipe_io_profile_t pipe_detail = {0};

    if (tool_equal(work, "cpu_scale")) {
        uint64_t phase_begin = profile.enabled ? tool_rdtsc() : 0;
        checksum = cpu_scale_loop(iters, &cpus_seen, &apic_mask);
        if (profile.enabled) profile_add(&profile, P_COMPUTE, phase_begin, tool_rdtsc());
    } else if (tool_equal(work, "spawn_wait")) {
        uint64_t ok_spawns = 0;
        uint32_t mask = 0;
        uint32_t apic = get_current_apic_id();
        if (apic < 32) mask |= (1u << apic);
        const char *noop_argv[] = { "/bin/smpbench", "--noop", NULL };

        for (uint64_t i = 0; i < iters; i++) {
            long pid = profile_call(&profile, P_SPAWN, SYS_SPAWN, (uintptr_t)"/bin/smpbench", (uintptr_t)noop_argv, 0);
            if (pid > 0) {
                uint64_t status = 0;
                long r = profile_call(&profile, P_WAIT, SYS_WAITPID, (uint64_t)pid, (uintptr_t)&status, 0);
                if (r > 0 && WEXITSTATUS(status) == 0) {
                    ok_spawns++;
                }
            }
            apic = get_current_apic_id();
            if (apic < 32) mask |= (1u << apic);
        }
        checksum = ok_spawns;
        apic_mask = mask;
        uint32_t cnt = 0;
        for (uint32_t m = mask; m > 0; m >>= 1) {
            if (m & 1) cnt++;
        }
        cpus_seen = cnt ? cnt : 1;
    } else if (tool_equal(work, "signals")) {
        profile_call(&profile, P_SETUP, SYS_SETPGID, 0, 0, 0);
        signal_action_t sa = {.handler = (uintptr_t)sig_bench_handler, .mask = 0, .flags = 0, .reserved = 0};
        profile_call(&profile, P_SETUP, SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0);

        s_signals_count = 0;
        uint32_t mask = 0;
        uint32_t apic = get_current_apic_id();
        if (apic < 32) mask |= (1u << apic);

        for (uint64_t i = 0; i < iters; i++) {
            profile_call(&profile, P_SIGNAL, SYS_KILL, 0, SIGINT, 0);
            apic = get_current_apic_id();
            if (apic < 32) mask |= (1u << apic);
        }

        checksum = s_signals_count;
        apic_mask = mask;
        uint32_t cnt = 0;
        for (uint32_t m = mask; m > 0; m >>= 1) {
            if (m & 1) cnt++;
        }
        cpus_seen = cnt ? cnt : 1;
    } else if (tool_equal(work, "pipes") || tool_equal(work, "pipe_bw") || tool_equal(work, "pipes4k")) {
        int pfd[2];
        if (profile_call(&profile, P_SETUP, SYS_PIPE, (uintptr_t)pfd, 0, 0) != 0) return 1;
        pipe_detail.fd = (uint64_t)pfd[0];
        if (wait_enabled && (
            tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_ENABLE, (uintptr_t)&pipe_detail, 0) != SYSCALL_EINVAL ||
            tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_ENABLE, 0, sizeof(pipe_detail)) != SYSCALL_EFAULT ||
            tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_ENABLE, UINT64_MAX - 32, sizeof(pipe_detail)) != SYSCALL_EFAULT ||
            tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_ENABLE, (uintptr_t)run_worker, sizeof(pipe_detail)) != SYSCALL_EFAULT)) return 1;
        if (wait_enabled && tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_ENABLE,
            (uintptr_t)&pipe_detail, sizeof(pipe_detail)) != 0) return 1;

        sysinfo_t si_curr;
        profile_call(&profile, P_SETUP, SYS_SYSINFO, (uintptr_t)&si_curr, 0, 0);
        uint32_t cpus_online = si_curr.cpu_count ? si_curr.cpu_count : 1;
        uint64_t target_cpu = (cpus_online > 1) ? ((wid + 1) % cpus_online) : 0;

        char target_str[16]; tool_format_u64(target_str, target_cpu);
        char iters_str[32]; tool_format_u64(iters_str, iters);
        char fd_str[16]; tool_format_u64(fd_str, (uint64_t)pfd[1]);

        const char *writer_argv[] = {
            "/bin/smpbench",
            "--worker",
            target_str,
            tool_equal(work, "pipes4k") ? "pipe_writer4k" : "pipe_writer",
            iters_str,
            fd_str,
            profile.enabled ? "--profile" : NULL,
            NULL
        };

        long child_pid = profile_call(&profile, P_SPAWN, SYS_SPAWN, (uintptr_t)"/bin/smpbench", (uintptr_t)writer_argv, 0);
        if (child_pid <= 0) {
            tool_syscall(SYS_CLOSE, (uint64_t)pfd[0], 0, 0);
            tool_syscall(SYS_CLOSE, (uint64_t)pfd[1], 0, 0);
            return 1;
        }

        /* Close write end in reader so reader gets EOF when child closes write end */
        profile_call(&profile, P_CLOSE, SYS_CLOSE, (uint64_t)pfd[1], 0, 0);

        uint32_t hdr[2] = {0};
        size_t hdr_read = 0;
        while (hdr_read < sizeof(hdr)) {
            long n = profile_call(&profile, P_HEADER, SYS_READ, (uint64_t)pfd[0], (uintptr_t)((uint8_t *)hdr + hdr_read), sizeof(hdr) - hdr_read);
            if (n <= 0) break;
            hdr_read += (size_t)n;
        }

        uint32_t my_apic = get_current_apic_id();
        uint32_t mask = 0;
        if (my_apic < 32) mask |= (1u << my_apic);
        if (hdr_read == sizeof(hdr) && hdr[0] < 32) {
            mask |= (1u << hdr[0]);
        }

        uint64_t cs = 0xcbf29ce484222325ULL;
        size_t total_bytes = 0;
        for (;;) {
            size_t request = sizeof(s_pipe_buf);
            if (profile.enabled) {
                uint64_t remaining = iters * 1024 - total_bytes;
                if (remaining == 0) break;
                if (remaining < request) request = (size_t)remaining;
            }
            long n = profile_call(&profile, P_READ, SYS_READ, (uint64_t)pfd[0], (uintptr_t)s_pipe_buf, request);
            if (n <= 0) break;
            uint64_t phase_begin = profile.enabled ? tool_rdtsc() : 0;
            for (long k = 0; k < n; k++) {
                cs ^= s_pipe_buf[k];
                cs *= 0x100000001b3ULL;
            }
            total_bytes += (size_t)n;
            if (profile.enabled) profile_add(&profile, P_COMPUTE, phase_begin, tool_rdtsc());
        }
        bool writer_profile_ok = true;
        if (profile.enabled) {
            writer_profile_ok = read_writer_profile(&profile, (uint64_t)pfd[0], &writer_profile);
        }
        if (wait_enabled && tool_syscall(SYS_SPAWN_PROFILE, PIPE_PROFILE_DISABLE,
            (uintptr_t)&pipe_detail, sizeof(pipe_detail)) != 0) return 1;
        profile_call(&profile, P_CLOSE, SYS_CLOSE, (uint64_t)pfd[0], 0, 0);

        uint64_t status = 0;
        long wres = profile_call(&profile, P_WAIT, SYS_WAITPID, (uint64_t)child_pid, (uintptr_t)&status, 0);
        if (wres > 0 && WEXITSTATUS(status) == 0 && total_bytes == iters * 1024 && writer_profile_ok) {
            checksum = cs;
        } else {
            checksum = 0;
        }

        apic_mask = mask;
        uint32_t cnt = 0;
        for (uint32_t m = mask; m > 0; m >>= 1) {
            if (m & 1) cnt++;
        }
        cpus_seen = cnt ? cnt : 1;
    } else {
        return 2;
    }

    uint64_t end_cycles = tool_rdtsc();
    if (wait_enabled && tool_syscall(SYS_SPAWN_PROFILE, WAIT_PROFILE_DISABLE,
        (uintptr_t)&wait_detail, sizeof(wait_detail)) != 0) return 1;
    if (profile.enabled && tool_syscall(SYS_SPAWN_PROFILE, SPAWN_PROFILE_DISABLE,
        (uintptr_t)&spawn_detail, sizeof(spawn_detail)) != 0) return 1;
    bool have_cpu_end = tool_syscall(SYS_PROCINFO, PROC_INFO_SELF, (uintptr_t)&own_end, 0) == 1;
    uint64_t cpu_ticks = have_cpu_start && have_cpu_end && own_start.pid == own_end.pid &&
                         own_end.cpu_ticks >= own_start.cpu_ticks ? own_end.cpu_ticks - own_start.cpu_ticks : 0;
    sysinfo_t si_end;
    tool_syscall(SYS_SYSINFO, (uintptr_t)&si_end, 0, 0);
    uint64_t end_ticks = si_end.uptime_ticks;
    uint64_t time_us_end = (end_cycles * 1000000ULL) / tsc_hz;

    uint64_t delta_cycles = (end_cycles >= start_cycles) ? (end_cycles - start_cycles) : 1;
    uint64_t rdtsc_us = (delta_cycles * 1000000ULL) / tsc_hz;
    uint64_t delta_ticks = (end_ticks >= start_ticks) ? (end_ticks - start_ticks) : 0;
    uint64_t tick_us = (delta_ticks * 1000000ULL) / tick_hz;

    /* Flag clock disagreement > 5% */
    uint32_t clock_disagree = 0;
    if (rdtsc_us > 0 && tick_us > 0) {
        uint64_t diff = (rdtsc_us > tick_us) ? (rdtsc_us - tick_us) : (tick_us - rdtsc_us);
        uint64_t max_t = (rdtsc_us > tick_us) ? rdtsc_us : tick_us;
        if ((diff * 100ULL) / max_t > 5) clock_disagree = 1;
    }

    make_worker_filepath((uint32_t)wid, s_worker_path, sizeof(s_worker_path));
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)s_worker_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (fd < 0) return 1;

    /* Write structured result line */
    static char line[4096];
    size_t pos = 0;
    const char *s = "worker_id=";
    while (*s) line[pos++] = *s++;
    char tmp[32];
    size_t n = tool_format_u64(tmp, wid);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " iterations=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, iters);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " checksum=0x";
    while (*s) line[pos++] = *s++;
    /* format checksum in hex */
    for (int b = 60; b >= 0; b -= 4) {
        uint32_t nib = (uint32_t)((checksum >> b) & 0xFULL);
        line[pos++] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }

    s = " cpus_seen=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, (uint64_t)cpus_seen);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " apic_mask=0x";
    while (*s) line[pos++] = *s++;
    for (int b = 28; b >= 0; b -= 4) {
        uint32_t nib = (uint32_t)((apic_mask >> b) & 0xFULL);
        line[pos++] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }

    s = " time_us_start=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, time_us_start);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " time_us_end=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, time_us_end);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " time_us=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, rdtsc_us);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " cpu_ticks=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, cpu_ticks);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];
    s = " tick_hz=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, tick_hz);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];
    s = " cpu_valid=";
    while (*s) line[pos++] = *s++;
    line[pos++] = have_cpu_start && have_cpu_end && own_start.pid == own_end.pid &&
                  own_end.cpu_ticks >= own_start.cpu_ticks ? '1' : '0';

    s = " tick_us=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, tick_us);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    s = " clock_disagree=";
    while (*s) line[pos++] = *s++;
    n = tool_format_u64(tmp, (uint64_t)clock_disagree);
    for (size_t i = 0; i < n; i++) line[pos++] = tmp[i];

    if (profile.enabled) {
        uint64_t accounted = 0;
        for (size_t i = 0; i < P_COUNT; i++) {
            if (UINT64_MAX - accounted < profile.cycles[i]) { profile.valid = false; break; }
            accounted += profile.cycles[i];
        }
        if (end_cycles < start_cycles || accounted > delta_cycles) profile.valid = false;
        if (!append_number(line, &pos, sizeof(line), "profile", 1) ||
            !append_number(line, &pos, sizeof(line), "profile_valid", profile.valid ? 1 : 0) ||
            !append_number(line, &pos, sizeof(line), "profile_hz", tsc_hz) ||
            !append_number(line, &pos, sizeof(line), "profile_cycles", delta_cycles) ||
            !append_number(line, &pos, sizeof(line), "p_other", accounted <= delta_cycles ? delta_cycles - accounted : 0)) return 1;
        for (size_t i = 0; i < P_COUNT; i++) {
            char name[32]; size_t k = 0;
            name[k++] = 'p'; name[k++] = '_';
            for (const char *s = phase_names[i]; *s; s++) name[k++] = *s;
            name[k] = '\0';
            if (!append_number(line, &pos, sizeof(line), name, profile.cycles[i])) return 1;
            name[k] = '_'; name[k + 1] = 'n'; name[k + 2] = '\0';
            if (!append_number(line, &pos, sizeof(line), name, profile.calls[i])) return 1;
            name[k + 1] = 'm'; name[k + 2] = 'a'; name[k + 3] = 'x'; name[k + 4] = '\0';
            if (!append_number(line, &pos, sizeof(line), name, profile.maximum[i])) return 1;
        }
        uint64_t spawn_accounted = 0;
        for (size_t i = 0; i < SP_COUNT; i++) {
            if (UINT64_MAX - spawn_accounted < spawn_detail.phase[i]) spawn_detail.valid = 0;
            else spawn_accounted += spawn_detail.phase[i];
        }
        if (spawn_accounted > spawn_detail.total) spawn_detail.valid = 0;
        if (!spawn_detail.valid) return 1;
        const char *const spawn_names[] = {"sp_file", "sp_reap", "sp_elf", "sp_ustack", "sp_kstack",
            "sp_tcb", "sp_fds", "sp_publish", "sp_cleanup"};
        const char *const elf_names[] = {"se_space", "se_alloc", "se_map", "se_copy"};
        if (!append_number(line, &pos, sizeof(line), "spawn_diag", 1) ||
            !append_number(line, &pos, sizeof(line), "sp_valid", spawn_detail.valid) ||
            !append_number(line, &pos, sizeof(line), "sp_total", spawn_detail.total) ||
            !append_number(line, &pos, sizeof(line), "sp_calls", spawn_detail.calls) ||
            !append_number(line, &pos, sizeof(line), "sp_failures", spawn_detail.failures) ||
            !append_number(line, &pos, sizeof(line), "sp_other", spawn_detail.total - spawn_accounted)) return 1;
        for (size_t i = 0; i < SP_COUNT; i++)
            if (!append_number(line, &pos, sizeof(line), spawn_names[i], spawn_detail.phase[i])) return 1;
        for (size_t i = 0; i < SE_COUNT; i++)
            if (!append_number(line, &pos, sizeof(line), elf_names[i], spawn_detail.elf[i])) return 1;
        if (tool_equal(work, "pipes") || tool_equal(work, "pipe_bw") || tool_equal(work, "pipes4k")) {
            const char *const names[] = {"writer_cycles", "writer_write", "writer_write_n", "writer_write_max",
                                        "writer_compute", "writer_compute_n", "writer_compute_max", "writer_other", "writer_valid",
                "writer_queued_to_run", "writer_run_to_entry", "writer_startup_valid"};
            const uint64_t values[] = {writer_profile.total, writer_profile.write, writer_profile.write_n,
                writer_profile.write_max, writer_profile.compute, writer_profile.compute_n,
                writer_profile.compute_max, writer_profile.other, writer_profile.valid,
                writer_profile.queued_to_run, writer_profile.run_to_entry, writer_profile.startup_valid};
            for (size_t i = 0; i < 12; i++) {
                if (!append_number(line, &pos, sizeof(line), names[i], values[i])) return 1;
            }
        }
    }
    if (wait_enabled) {
        if (!wait_detail.valid || wait_detail.blocks != wait_detail.wakes ||
            wait_detail.blocks != wait_detail.selections || wait_detail.blocks != wait_detail.resumes) return 1;
        const char *const names[] = {"wait_diag", "wait_valid", "wait_blocks", "wait_wakes", "wait_selections",
            "wait_resumes", "wait_blocked", "wait_ready", "wait_resume", "wait_ready_max"};
        const uint64_t values[] = {1, wait_detail.valid, wait_detail.blocks, wait_detail.wakes,
            wait_detail.selections, wait_detail.resumes, wait_detail.blocked_cycles,
            wait_detail.ready_cycles, wait_detail.resume_cycles, wait_detail.ready_max};
        for (size_t i=0; i<10; i++)
            if (!append_number(line, &pos, sizeof(line), names[i], values[i])) return 1;
        if (!append_number(line, &pos, sizeof(line), "wait_hz", tsc_hz) ||
            !append_number(line, &pos, sizeof(line), "wait_total", delta_cycles)) return 1;
    }
    if (wait_enabled && (tool_equal(work, "pipes") || tool_equal(work, "pipe_bw") || tool_equal(work, "pipes4k"))) {
        if (!pipe_detail.valid || pipe_detail.read_bytes != pipe_detail.write_bytes ||
            pipe_detail.read_bytes != iters * 1024 + 8 + (profile.enabled ? sizeof(writer_profile) : 0)) return 1;
        const char *const names[] = {"pi_diag", "pi_valid", "pi_reads", "pi_writes", "pi_read_bytes", "pi_write_bytes",
            "pi_read_max", "pi_write_max", "pi_read_small", "pi_read_1k", "pi_read_large", "pi_write_small", "pi_write_1k",
            "pi_write_large", "pi_read_waits", "pi_write_waits", "pi_empty_drains", "pi_empty_fills", "pi_full_fills",
            "pi_reader_cpus", "pi_writer_cpus", "pi_direction_changes", "pi_cpu_changes"};
        const uint64_t values[] = {1, pipe_detail.valid, pipe_detail.reads, pipe_detail.writes, pipe_detail.read_bytes,
            pipe_detail.write_bytes, pipe_detail.read_max, pipe_detail.write_max, pipe_detail.read_small, pipe_detail.read_1k,
            pipe_detail.read_large, pipe_detail.write_small, pipe_detail.write_1k, pipe_detail.write_large, pipe_detail.read_waits,
            pipe_detail.write_waits, pipe_detail.empty_drains, pipe_detail.empty_fills, pipe_detail.full_fills,
            pipe_detail.reader_cpus, pipe_detail.writer_cpus, pipe_detail.direction_changes, pipe_detail.cpu_changes};
        for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i)
            if (!append_number(line, &pos, sizeof(line), names[i], values[i])) return 1;
    }
    if (!append_number(line, &pos, sizeof(line), "pipe_batch", tool_equal(work, "pipes4k") ? 4 : 1)) return 1;
    line[pos++] = '\n';
    line[pos] = '\0';

    bool saved = tool_syscall(SYS_WRITE, fd, (uintptr_t)line, pos) == (long)pos;
    tool_syscall(SYS_CLOSE, fd, 0, 0);
    return saved ? 0 : 1;
}

/* Parent execution */
int smpbench_main(int argc, char **argv) {
    if (argc >= 2 && tool_equal(argv[1], "--noop")) {
        return 0;
    }
    if (argc >= 2 && tool_equal(argv[1], "--worker")) {
        return run_worker(argc, argv);
    }

    bool compare_mode = false;
    bool profile_mode = false;
    bool wait_mode = false;
    uint32_t workers = 0;
    uint32_t reps = DEFAULT_REPS;
    uint64_t iterations = DEFAULT_ITERATIONS;
    const char *workload = "cpu_scale";

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "-c") || tool_equal(argv[i], "--compare")) {
            compare_mode = true;
        } else if (tool_equal(argv[i], "-p") || tool_equal(argv[i], "--profile")) {
            profile_mode = true;
        } else if (tool_equal(argv[i], "--wait-profile")) {
            wait_mode = true;
        } else if (tool_equal(argv[i], "-n") && i + 1 < argc) {
            size_t p = 0; uint64_t val = 0;
            if (!parse_u64(argv[++i], &p, &val) || val == 0) return 2;
            workers = (uint32_t)val;
        } else if (tool_equal(argv[i], "-r") && i + 1 < argc) {
            size_t p = 0; uint64_t val = 0;
            if (!parse_u64(argv[++i], &p, &val) || val == 0) return 2;
            reps = (uint32_t)val;
        } else if (tool_equal(argv[i], "-i") && i + 1 < argc) {
            size_t p = 0; uint64_t val = 0;
            if (!parse_u64(argv[++i], &p, &val) || val == 0) return 2;
            iterations = val;
        } else if (tool_equal(argv[i], "-w") && i + 1 < argc) {
            workload = argv[++i];
            if (!tool_equal(workload, "cpu_scale") && !tool_equal(workload, "spawn_wait") &&
                !tool_equal(workload, "signals") && !tool_equal(workload, "pipes") &&
                !tool_equal(workload, "pipe_bw") && !tool_equal(workload, "pipes4k")) {
                tool_error("smpbench", "unsupported workload", workload);
                return 2;
            }
        } else if (tool_equal(argv[i], "-h") || tool_equal(argv[i], "--help")) {
            out_str("Usage: smpbench [-c] [-n workers] [-r reps] [-i iters] [-w workload]\n");
            out_str("  Workers report ready before release; setup and start spread are reported per repetition.\n");
            out_str("  -p, --profile  Per-phase elapsed cycles, call counts and longest calls (adds timing overhead)\n");
            out_str("  --wait-profile  Scheduler wait clocks only; no per-call phase clocks\n");
            out_str("  -c             Machine-parseable output\n");
            out_str("  -n <workers>   Worker count (1, 2, 4, 8, capped at online CPUs)\n");
            out_str("  -r <reps>      Timed repetitions (default: 5)\n");
            out_str("  -i <iters>     Loop iterations per worker (default: 10000000 for cpu_scale, 25 for spawn_wait, 500 for signals, 500 for pipes)\n");
            out_str("  -w <workload>  Workload (cpu_scale, spawn_wait, signals, pipes, pipes4k)\n");
            return 0;
        } else {
            tool_error("smpbench", "unrecognized option", argv[i]);
            return 2;
        }
    }

    if (tool_equal(workload, "spawn_wait") && iterations == DEFAULT_ITERATIONS) {
        iterations = 25;
    } else if (tool_equal(workload, "signals") && iterations == DEFAULT_ITERATIONS) {
        iterations = 500;
    } else if ((tool_equal(workload, "pipes") || tool_equal(workload, "pipe_bw") || tool_equal(workload, "pipes4k")) && iterations == DEFAULT_ITERATIONS) {
        iterations = 500;
    }

    sysinfo_t si;
    tool_syscall(SYS_SYSINFO, (uintptr_t)&si, 0, 0);
    uint32_t cpus_online = si.cpu_count ? si.cpu_count : 1;
    if (workers == 0) workers = cpus_online;
    if (workers > cpus_online) workers = cpus_online;
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;

    char env_tag[32];
    detect_env(env_tag, sizeof(env_tag));
    char fs_tag[32];
    detect_fs(fs_tag, sizeof(fs_tag));

    uint64_t expected_checksum = 0;
    if (tool_equal(workload, "cpu_scale")) {
        expected_checksum = cpu_scale_loop(iterations, NULL, NULL);
    } else if (tool_equal(workload, "spawn_wait") || tool_equal(workload, "signals")) {
        expected_checksum = iterations;
    } else if (tool_equal(workload, "pipes") || tool_equal(workload, "pipe_bw") || tool_equal(workload, "pipes4k")) {
        expected_checksum = pipe_expected_checksum(iterations);
    }

    if (tool_equal(workload, "signals")) {
        signal_action_t sa_ign = {.handler = SIG_IGN, .mask = 0, .flags = 0, .reserved = 0};
        tool_syscall(SYS_SIGACTION, SIGINT, (uintptr_t)&sa_ign, 0);
    }
    /* A broken barrier is an ordinary failed repetition, including when all
     * peers have exited before release. Handle EPIPE rather than terminating. */
    signal_action_t pipe_ign = {.handler = SIG_IGN, .mask = 0, .flags = 0, .reserved = 0};
    if (tool_syscall(SYS_SIGACTION, SIGPIPE, (uintptr_t)&pipe_ign, 0) != 0) return 1;

    /* Total reps: 1 warm-up + reps timed */
    uint64_t rep_times[32];
    uint32_t rep_cpus_seen[32];
    uint32_t timed_count = 0;
    bool all_ok = true;
    bool short_run = false;

    char iters_str[32];
    tool_format_u64(iters_str, iterations);

    for (uint32_t rep = 0; rep < reps + 1; rep++) {
        bool is_warmup = (rep == 0);

        uint64_t pids[MAX_WORKERS] = {0};
        uint64_t setup_t = tool_rdtsc();
        int ready_fd[2], gate_fd[2];
        if (tool_syscall(SYS_PIPE, (uintptr_t)ready_fd, 0, 0) != 0) return 1;
        if (tool_syscall(SYS_PIPE, (uintptr_t)gate_fd, 0, 0) != 0) {
            tool_syscall(SYS_CLOSE, ready_fd[0], 0, 0);
            tool_syscall(SYS_CLOSE, ready_fd[1], 0, 0);
            return 1;
        }
        char barrier_str[4][16];
        tool_format_u64(barrier_str[0], (uint64_t)ready_fd[0]);
        tool_format_u64(barrier_str[1], (uint64_t)ready_fd[1]);
        tool_format_u64(barrier_str[2], (uint64_t)gate_fd[0]);
        tool_format_u64(barrier_str[3], (uint64_t)gate_fd[1]);
        bool barrier_ok = true;

        for (uint32_t w = 0; w < workers; w++) {
            make_worker_filepath(w, s_worker_path, sizeof(s_worker_path));
            tool_syscall(SYS_UNLINK, (uintptr_t)s_worker_path, 0, 0);
            char id_str[16];
            tool_format_u64(id_str, (uint64_t)w);
            const char *spawn_argv[] = {
                "/bin/smpbench",
                "--worker",
                id_str,
                workload,
                iters_str,
                barrier_str[0], barrier_str[1], barrier_str[2], barrier_str[3],
                profile_mode ? "--profile" : wait_mode ? "--wait-profile" : NULL,
                NULL
            };
            long pid = tool_syscall(SYS_SPAWN, (uintptr_t)"/bin/smpbench", (uintptr_t)spawn_argv, 0);
            if (pid <= 0) {
                all_ok = false;
                short_run = true;
                pids[w] = 0;
                barrier_ok = false;
            } else {
                pids[w] = (uint64_t)pid;
            }
        }

        uint64_t launch_t = profile_mode ? tool_rdtsc() : 0;
        tool_syscall(SYS_CLOSE, ready_fd[1], 0, 0);
        tool_syscall(SYS_CLOSE, gate_fd[0], 0, 0);
        uint32_t ready_mask = 0;
        if (barrier_ok) {
            for (uint32_t w = 0; w < workers; w++) {
                uint8_t id = 0;
                long n = tool_syscall(SYS_READ, ready_fd[0], (uintptr_t)&id, 1);
                if (n != 1 || id >= workers || (ready_mask & (1u << id))) {
                    barrier_ok = false;
                    break;
                }
                ready_mask |= 1u << id;
            }
        }
        tool_syscall(SYS_CLOSE, ready_fd[0], 0, 0);
        uint64_t start_t = tool_rdtsc();
        if (barrier_ok) {
            uint8_t tokens[MAX_WORKERS];
            for (uint32_t w = 0; w < workers; w++) tokens[w] = 1;
            /* One atomic small write releases the complete cohort. Each worker
             * consumes exactly one byte; scheduling still determines start skew. */
            barrier_ok = tool_syscall(SYS_WRITE, gate_fd[1], (uintptr_t)tokens, workers) == (long)workers;
        }
        tool_syscall(SYS_CLOSE, gate_fd[1], 0, 0);
        if (!barrier_ok) {
            all_ok = false;
            short_run = true;
            for (uint32_t w = 0; w < workers; w++) {
                if (pids[w]) tool_syscall(SYS_KILL, pids[w], SIGKILL, 0);
            }
        }

        uint64_t release_t = profile_mode ? tool_rdtsc() : 0;
        /* Wait for workers */
        for (uint32_t w = 0; w < workers; w++) {
            if (pids[w] > 0) {
                uint64_t status = 0;
                long r = tool_syscall(SYS_WAITPID, pids[w], (uintptr_t)&status, 0);
                if (r <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    all_ok = false;
                }
            }
        }

        uint64_t end_t = tool_rdtsc();
        uint64_t tsc_hz = si.tsc_hz ? si.tsc_hz : 1000000000ULL;
        uint64_t elapsed_us = ((end_t >= start_t ? end_t - start_t : 1) * 1000000ULL) / tsc_hz;
        if (elapsed_us == 0) elapsed_us = 1;

        out_str("smpbench_rep rep=");
        out_u64(rep);
        out_str(" warmup=");
        out_u64(is_warmup ? 1 : 0);
        out_str(" elapsed_us=");
        out_u64(elapsed_us);
        out_str(" setup_us=");
        out_u64(((start_t - setup_t) * 1000000ULL) / tsc_hz);
        out_str(" barrier_ok=");
        out_u64(barrier_ok ? 1 : 0);
        out_str("\n");
        /* Aggregate worker results */
        uint32_t rep_mask = 0;
        uint64_t max_worker_us = 0;
        uint64_t first_start_us = UINT64_MAX, last_start_us = 0;
        for (uint32_t w = 0; w < workers; w++) {
            make_worker_filepath(w, s_worker_path, sizeof(s_worker_path));
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)s_worker_path, VFS_O_RDONLY, 0);
            if (fd >= 0) {
                long nr = tool_syscall(SYS_READ, fd, (uintptr_t)s_file_buf, sizeof(s_file_buf) - 1);
                tool_syscall(SYS_CLOSE, fd, 0, 0);
                tool_syscall(SYS_UNLINK, (uintptr_t)s_worker_path, 0, 0);
                if (nr > 0) {
                    s_file_buf[nr] = '\0';
                    char val_str[64];
                    out_str("smpbench_worker rep=");
                    out_u64(rep);
                    out_str(" warmup=");
                    out_u64(is_warmup ? 1 : 0);
                    out_str(" ");
                    out_str(s_file_buf);
                    if (parse_field(s_file_buf, "time_us_start", val_str, sizeof(val_str))) {
                        size_t p = 0; uint64_t start_us = 0;
                        if (parse_u64(val_str, &p, &start_us)) {
                            if (start_us < first_start_us) first_start_us = start_us;
                            if (start_us > last_start_us) last_start_us = start_us;
                        }
                    }
                    if (parse_field(s_file_buf, "checksum", val_str, sizeof(val_str))) {
                        size_t p = 0; uint64_t cs = 0;
                        if (parse_hex_u64(val_str, &p, &cs)) {
                            if (cs != expected_checksum) all_ok = false;
                        }
                    }
                    if (parse_field(s_file_buf, "apic_mask", val_str, sizeof(val_str))) {
                        size_t p = 0; uint64_t mask = 0;
                        if (parse_hex_u64(val_str, &p, &mask)) {
                            rep_mask |= (uint32_t)mask;
                        }
                    }
                    if (parse_field(s_file_buf, "time_us", val_str, sizeof(val_str))) {
                        size_t p = 0; uint64_t w_us = 0;
                        if (parse_u64(val_str, &p, &w_us)) {
                            if (w_us > max_worker_us) max_worker_us = w_us;
                        }
                    }
                } else {
                    all_ok = false;
                }
            } else {
                all_ok = false;
            }
        }

        if (profile_mode) {
            uint64_t collect_t = tool_rdtsc();
            bool valid = setup_t <= launch_t && launch_t <= start_t &&
                         start_t <= release_t && release_t <= end_t && end_t <= collect_t;
            out_str("smpbench_parent rep="); out_u64(rep);
            out_str(" profile_valid="); out_u64(valid ? 1 : 0);
            out_str(" hz="); out_u64(tsc_hz);
            const char *const names[] = {"launch", "ready", "release", "join", "collect"};
            const uint64_t boundaries[] = {setup_t, launch_t, start_t, release_t, end_t, collect_t};
            for (size_t i = 0; i < 5; i++) {
                out_str(" "); out_str(names[i]); out_str("=");
                out_u64(valid ? boundaries[i + 1] - boundaries[i] : 0);
            }
            out_str("\n");
        }
        out_str("smpbench_barrier rep=");
        out_u64(rep);
        out_str(" start_spread_us=");
        out_u64(first_start_us == UINT64_MAX ? 0 : last_start_us - first_start_us);
        out_str("\n");

        uint32_t cpus_count = 0;
        for (uint32_t m = rep_mask; m > 0; m >>= 1) {
            if (m & 1) cpus_count++;
        }
        if (cpus_count == 0) cpus_count = 1;

        uint64_t rep_time = (max_worker_us > 0) ? max_worker_us : elapsed_us;
        if (!is_warmup && timed_count < 32) {
            rep_times[timed_count] = rep_time;
            rep_cpus_seen[timed_count] = cpus_count;
            timed_count++;
        }
    }

    if (timed_count == 0) return 1;

    /* Sort rep_times to find min, med, max */
    for (uint32_t i = 0; i < timed_count - 1; i++) {
        for (uint32_t j = i + 1; j < timed_count; j++) {
            if (rep_times[j] < rep_times[i]) {
                uint64_t t = rep_times[i];
                rep_times[i] = rep_times[j];
                rep_times[j] = t;
            }
        }
    }

    uint64_t min_us = rep_times[0];
    uint64_t med_us = rep_times[timed_count / 2];
    uint64_t max_us = rep_times[timed_count - 1];

    uint64_t total_work_units = (uint64_t)workers * iterations;
    uint64_t units_per_sec = (total_work_units * 1000000ULL) / (med_us ? med_us : 1);

    uint32_t final_cpus_seen = rep_cpus_seen[timed_count / 2];
    char units_s_buf[32];
    format_units_per_sec(units_s_buf, sizeof(units_s_buf), units_per_sec);

    if (compare_mode) {
        /* Line 1 */
        out_str(profile_mode ? "smpbench rev=3 profile=1 barrier=pipe metric=max_worker_us cpus_online=" : "smpbench rev=2 barrier=pipe metric=max_worker_us cpus_online=");
        out_u64((uint64_t)cpus_online);
        out_str(" env=");
        out_str(env_tag);
        out_str(" fs=");
        out_str(fs_tag);
        out_str("\n");

        /* Line 2 */
        out_str("w=");
        out_str(workload);
        out_str(" n=");
        out_u64((uint64_t)workers);
        out_str(" reps=");
        out_u64((uint64_t)timed_count);
        out_str(" min_us=");
        out_u64(min_us);
        out_str(" med_us=");
        out_u64(med_us);
        out_str(" max_us=");
        out_u64(max_us);
        out_str(" units_per_sec=");
        out_str(units_s_buf);
        out_str(" cpus_seen=");
        out_u64((uint64_t)final_cpus_seen);
        out_str(" ok=");
        out_u64(all_ok ? 1 : 0);
        out_str(" short=");
        out_u64(short_run ? 1 : 0);
        out_str("\n");
    } else {
        /* Line 1 */
        out_str(profile_mode ? "smpbench rev=3 profile=1 barrier=pipe metric=max_worker_us cpus_online=" : "smpbench rev=2 barrier=pipe metric=max_worker_us cpus_online=");
        out_u64((uint64_t)cpus_online);
        out_str(" env=");
        out_str(env_tag);
        out_str(" fs=");
        out_str(fs_tag);
        out_str("\n");

        /* Line 2 */
        char min_ms[32], med_ms[32], max_ms[32];
        format_ms_str(min_ms, sizeof(min_ms), min_us);
        format_ms_str(med_ms, sizeof(med_ms), med_us);
        format_ms_str(max_ms, sizeof(max_ms), max_us);

        out_str("w=");
        out_str(workload);
        out_str(" n=");
        out_u64((uint64_t)workers);
        out_str(" reps=");
        out_u64((uint64_t)timed_count);
        out_str(" min=");
        out_str(min_ms);
        out_str(" med=");
        out_str(med_ms);
        out_str(" max=");
        out_str(max_ms);
        out_str(" units/s=");
        out_str(units_s_buf);
        out_str(" cpus_seen=");
        out_u64((uint64_t)final_cpus_seen);
        out_str(" ok=");
        out_u64(all_ok ? 1 : 0);
        out_str(" short=");
        out_u64(short_run ? 1 : 0);
        out_str("\n");
    }

    return all_ok ? 0 : 1;
}
