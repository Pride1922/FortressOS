/* Actual benchmark orchestration with syscall adapters; no scheduler claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/tools/smpbench.c"
#include "../src/kernel/spawn_profile.h"

static bool open_fd[32], worker_mode, cohort_released;
static unsigned spawned, ready_reads, released, killed, waited, pipe_calls;
static int fault, result_id;
static char output[32000];
static char worker_report[4096];
static unsigned char wire[8192];
static size_t wire_len, wire_pos, write_chunk;
static size_t output_len;
enum { SUCCESS, SPAWN_FAIL, READY_EOF, READY_DUP, RELEASE_FAIL, PIPE_FAIL, WORKER_EOF };

size_t tool_length(const char *s) { return strlen(s); }
bool tool_equal(const char *a, const char *b) { return strcmp(a, b) == 0; }
int tool_write(const char *name, const void *p, size_t n) {
    (void)name;
    assert(output_len + n < sizeof(output));
    memcpy(output + output_len, p, n); output_len += n; output[output_len] = 0;
    return 0;
}
int tool_error(const char *a, const char *b, const char *c) {
    (void)a; (void)b; (void)c; return 1;
}
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_SPAWN_PROFILE) {
        if (a >= WAIT_PROFILE_READ && a <= WAIT_PROFILE_DISABLE) {
            if (c != sizeof(wait_profile_t)) return SYSCALL_EINVAL;
            if (!b || b >= 0x0000800000000000ULL || b == (uintptr_t)run_worker) return SYSCALL_EFAULT;
            wait_profile_t *info = (void *)b;
            memset(info, 0, sizeof(*info)); info->valid = 1;
            return 0;
        }
        if (a > SPAWN_PROFILE_DISABLE || c != sizeof(spawn_profile_t)) return SYSCALL_EINVAL;
        if (!b || b >= 0x0000800000000000ULL || b == (uintptr_t)run_worker) return SYSCALL_EFAULT;
        spawn_profile_t *info = (void *)b;
        memset(info, 0, sizeof(*info)); info->valid = 1;
        info->queued_cycles = 1; info->first_run_cycles = 2; info->birth_valid = 1;
        return 0;
    }
    if (nr == SYS_SYSINFO) {
        sysinfo_t *info = (void *)a; memset(info, 0, sizeof(*info));
        info->cpu_count = 8; info->tsc_hz = 1000000000; info->tick_hz = 100; return 0;
    }
    if (nr == SYS_PROCINFO) return 0;
    if (nr == SYS_MOUNTINFO) return 0;
    if (nr == SYS_SIGACTION || nr == SYS_UNLINK) return 0;
    if (nr == SYS_PIPE) {
        pipe_calls++;
        if (fault == PIPE_FAIL && pipe_calls == 2) return SYSCALL_EMFILE;
        if (pipe_calls % 2 == 1) { spawned = ready_reads = 0; cohort_released = false; }
        int *fds = (void *)a;
        fds[0] = pipe_calls % 2 == 1 ? 3 : 5; fds[1] = fds[0] + 1;
        assert(!open_fd[fds[0]] && !open_fd[fds[1]]);
        open_fd[fds[0]] = open_fd[fds[1]] = true; return 0;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && open_fd[a]); open_fd[a] = false; return 0;
    }
    if (nr == SYS_SPAWN) {
        const char *const *args = (void *)b;
        assert(!worker_mode && !cohort_released && ready_reads == 0);
        assert(!strcmp(args[5], "3") && !strcmp(args[6], "4"));
        assert(!strcmp(args[7], "5") && !strcmp(args[8], "6") && args[9] == NULL);
        unsigned id = spawned++;
        return fault == SPAWN_FAIL && id == 3 ? SYSCALL_ENOMEM : (long)(100 + id);
    }
    if (nr == SYS_READ && a == 3) {
        assert(!worker_mode && spawned == 8 && !cohort_released && c == 1 && !open_fd[4]);
        if (fault == READY_EOF) return 0;
        *(uint8_t *)b = fault == READY_DUP ? 0 : (uint8_t)ready_reads;
        ready_reads++; return 1;
    }
    if (nr == SYS_READ && a == 5) {
        assert(worker_mode && !open_fd[3] && !open_fd[4] && !open_fd[6]);
        if (fault == WORKER_EOF) return 0;
        *(uint8_t *)b = 1; return 1;
    }
    if (nr == SYS_WRITE && a == 6) {
        assert(!worker_mode && spawned == 8 && ready_reads == 8 && c == 8 && !open_fd[3]);
        for (size_t i = 0; i < c; i++) assert(((uint8_t *)b)[i] == 1);
        released++; cohort_released = true; return fault == RELEASE_FAIL ? SYSCALL_EPIPE : (long)c;
    }
    if (nr == SYS_WRITE && a == 4) {
        assert(worker_mode && !open_fd[3] && !open_fd[6] && c == 1);
        assert(*(uint8_t *)b == 0); ready_reads++; return 1;
    }
    if (nr == SYS_KILL) { assert(!released || fault == RELEASE_FAIL); killed++; return 0; }
    if (nr == SYS_WAITPID) {
        assert(!worker_mode && !open_fd[3] && !open_fd[4] && !open_fd[5] && !open_fd[6]);
        waited++; *(uint64_t *)b = killed ? SIGKILL : 0; return (long)a;
    }
    if (nr == SYS_OPEN) {
        if (fault != SUCCESS) return SYSCALL_ENOENT;
        assert(!open_fd[7]); open_fd[7] = true;
        if (!worker_mode) result_id = (int)(((const char *)a)[14] - '0');
        return 7;
    }
    if (nr == SYS_READ && a == 7) {
        char line[512];
        int n = snprintf(line, sizeof(line),
            "worker_id=%d checksum=0x%016llx apic_mask=0xff time_us_start=%d time_us_end=%d time_us=100\n",
            result_id, (unsigned long long)cpu_scale_loop(1, NULL, NULL), 1000 + result_id, 1100 + result_id);
        assert(n > 0 && (size_t)n < c); memcpy((void *)b, line, (size_t)n); return n;
    }
    if (nr == SYS_WRITE && a == 7) {
        assert(worker_mode && ready_reads == 1 && c < sizeof(worker_report));
        memcpy(worker_report, (void *)b, c); worker_report[c] = 0; return (long)c;
    }
    if (nr == SYS_WRITE && a == 8) {
        size_t n = c < write_chunk ? c : write_chunk;
        assert(n && wire_len + n <= sizeof(wire));
        memcpy(wire + wire_len, (void *)b, n); wire_len += n; return (long)n;
    }
    if (nr == SYS_READ && a == 8) {
        size_t n = wire_len - wire_pos;
        if (n > c) n = c;
        if (n > write_chunk) n = write_chunk;
        memcpy((void *)b, wire + wire_pos, n); wire_pos += n; return (long)n;
    }
    assert(!"Unexpected syscall"); return -1;
}

static void reset(int mode, bool worker) {
    memset(open_fd, 0, sizeof(open_fd)); worker_mode = worker; fault = mode;
    spawned = ready_reads = released = killed = waited = pipe_calls = 0;
    cohort_released = false;
    output_len = 0; output[0] = 0;
    worker_report[0] = 0;
}
int main(void) {
    char *args[] = {"smpbench", "-w", "cpu_scale", "-n", "8", "-r", "1", "-i", "1", "-c", NULL};
    for (int mode = SUCCESS; mode <= PIPE_FAIL; mode++) {
        reset(mode, false);
        int result = smpbench_main(10, args);
        assert(result == (mode == SUCCESS ? 0 : 1));
        for (int fd = 3; fd < 32; fd++) assert(!open_fd[fd]);
        if (mode == SUCCESS) {
            assert(released == 2 && waited == 16 && killed == 0);
            assert(strstr(output, "barrier_ok=1") && strstr(output, "start_spread_us=7"));
        } else if (mode != PIPE_FAIL) {
            assert(killed == (mode == SPAWN_FAIL ? 14u : 16u));
            assert(waited == killed && strstr(output, "ok=0 short=1"));
        }
    }
    for (int mode = 0; mode < 6; mode++) {
        bool use_profile = mode == 2 || mode == 3;
        bool use_wait = mode >= 2;
        reset(mode % 2 ? WORKER_EOF : SUCCESS, true);
        for (int fd = 3; fd <= 6; fd++) open_fd[fd] = true;
        char *worker[] = {"smpbench", "--worker", "0", "cpu_scale", "1", "3", "4", "5", "6", "--profile", NULL};
        if (mode >= 4) worker[9] = "--wait-profile";
        assert(run_worker(use_wait ? 10 : 9, worker) == (mode % 2 ? 1 : 0));
        assert(ready_reads == 1);
        if (!(mode % 2)) {
            assert((strstr(worker_report, "profile=1") != NULL) == use_profile);
            if (use_profile) assert(strstr(worker_report, "profile_valid=1") && strstr(worker_report, "p_compute_n=1"));
            assert((strstr(worker_report, "wait_diag=1") != NULL) == use_wait);
        }
        for (int fd = 3; fd < 32; fd++) assert(!open_fd[fd]);
    }
    profile_t p = {.enabled = true, .valid = true};
    spawn_profile_t kernel_profile = {.valid = 1};
    spawn_profile_t *kernel_pointer = &kernel_profile;
    uint64_t begin = spawn_profile_clock(kernel_pointer);
    SPAWN_ADD(kernel_pointer, total, begin);
    assert(kernel_profile.valid && kernel_profile.total > 0);
    kernel_profile.total = UINT64_MAX;
    SPAWN_ADD(kernel_pointer, total, spawn_profile_clock(&kernel_profile));
    assert(!kernel_profile.valid && kernel_profile.total == UINT64_MAX);
    kernel_profile.valid = 1;
    SPAWN_ADD(kernel_pointer, total, UINT64_MAX);
    assert(!kernel_profile.valid);
    spawn_profile_t *disabled = NULL;
    SPAWN_ADD(disabled, total, 0);
    assert(spawn_profile_clock(disabled) == 0);
    profile_add(&p, P_READ, 10, 30); profile_add(&p, P_READ, 100, 150);
    assert(p.valid && p.cycles[P_READ] == 70 && p.maximum[P_READ] == 50 && p.calls[P_READ] == 2);
    profile_add(&p, P_READ, 50, 40); assert(!p.valid && p.cycles[P_READ] == 70);
    p.valid = true; p.cycles[P_READ] = UINT64_MAX;
    profile_add(&p, P_READ, 0, 1); assert(!p.valid && p.cycles[P_READ] == UINT64_MAX);
    char tiny[8] = {0}; size_t position = 0;
    assert(!append_number(tiny, &position, sizeof(tiny), "too_long", UINT64_MAX) && position == 0);
    assert(!append_number(tiny, &position, 5, "a", 9) && position == 0);
    assert(append_number(tiny, &position, 6, "a", 9) && position == 4);
    tiny[position++] = '\n'; tiny[position] = '\0';
    assert(!strcmp(tiny, " a=9\n"));
    for (int enabled = 0; enabled < 2; enabled++) {
        reset(SUCCESS, false); open_fd[8] = true; wire_len = 0; write_chunk = 7;
        assert(run_pipe_writer(2, 8, enabled != 0) == 0 && !open_fd[8]);
        assert(wire_len == 8 + 2048 + (enabled ? sizeof(pipe_profile_t) : 0));
        for (size_t i = 0; i < 2048; i++) {
            assert(wire[8 + i] == (uint8_t)((i / 1024) * 37 + 13 + i % 1024));
        }
        if (enabled) {
            pipe_profile_t trailer; memcpy(&trailer, wire + 8 + 2048, sizeof(trailer));
            assert(trailer.magic == PIPE_PROFILE_MAGIC && trailer.valid == 1);
            assert(trailer.startup_valid == 1 && trailer.queued_to_run == 1);
            assert(trailer.total == trailer.write + trailer.compute + trailer.other);
            assert(trailer.write_n == 294 && trailer.compute_n == 2 && trailer.write_max <= trailer.write);
            for (int failure = 0; failure < 4; failure++) {
                memcpy(wire, &trailer, sizeof(trailer)); wire_pos = 0; wire_len = sizeof(trailer);
                if (failure == 1) wire_len--;
                if (failure == 2) wire[0] ^= 1;
                if (failure == 3) wire[wire_len++] = 42;
                profile_t reader = {.enabled = true, .valid = true}; pipe_profile_t received = {0};
                assert(read_writer_profile(&reader, 8, &received) == (failure == 0));
            }
        }
    }
    for (unsigned enabled=0; enabled<=1; ++enabled) for (unsigned units=1; units<=5; ++units) {
        reset(SUCCESS, false); open_fd[8]=true; wire_len=0; write_chunk=7;
        assert(run_pipe_writer_batch(units, 8, enabled!=0, 4)==0);
        assert(wire_len==8+units*1024+(enabled?sizeof(pipe_profile_t):0));
        for (size_t i=0;i<units*1024;i++) assert(wire[8+i]==(uint8_t)((i/1024)*37+13+i%1024));
        if (enabled) {
            pipe_profile_t detail; memcpy(&detail,wire+8+units*1024,sizeof(detail));
            assert(detail.valid && detail.compute_n==(units+3)/4);
            assert(detail.total==detail.write+detail.compute+detail.other);
        }
    }
    puts("phase adapters: elapsed sums/maxima, clock reversal/overflow, bounded formatting, opt-in worker, partial writer/trailer and malformed/EOF rejection PASS");
    puts("smpbench barrier adapters: success, EOF, duplicate ready, spawn/release/pipe failures, worker gate PASS");
    return 0;
}
