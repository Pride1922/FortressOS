/* shm-test: end-to-end check of SYS_SHM_CREATE/MAP/UNMAP.
 *   shm-test             run all checks, print [SHMTEST] lines, exit 0 on success
 *   shm-test child       (spawned) fd 3 = object: verify, answer, exit 0
 *   shm-test child-ro    (spawned) write through a read-only mapping: must die */
#include "user/tools/common.h"
#include "src/include/syscall_abi.h"

#define VA_A  0x7000000000ULL
#define VA_B  0x7000400000ULL
#define VA_C  0x7000800000ULL
#define SIZE  10000ULL         /* rounds up to 3 pages */
#define PAGES 3ULL

static int failures;

static void say(const char *a, const char *b) {
    tool_write("shm-test", a, tool_length(a));
    if (b) tool_write("shm-test", b, tool_length(b));
    tool_write("shm-test", "\n", 1);
}

static void check(bool ok, const char *what) {
    say(ok ? "[SHMTEST] ok   " : "[SHMTEST] FAIL ", what);
    if (!ok) failures++;
}

static long sys3(long nr, uintptr_t a, uintptr_t b, uintptr_t c) { return tool_syscall(nr, a, b, c); }

static long spawn_child(const char *mode, int fd) {
    const char *argv[] = {"/bin/shm-test", mode, NULL};
    spawn_fd_action_t dup = {.type = SPAWN_FD_ACTION_DUP2, .dst_fd = 3, .src_fd = fd};
    spawn_opts_t opts = {.size = sizeof(opts), .version = 2, .argv = (uintptr_t)argv,
                         .fd_actions = (uintptr_t)&dup, .action_count = 1};
    return sys3(SYS_SPAWN_EXT, (uintptr_t)"/bin/shm-test", (uintptr_t)&opts, sizeof(opts));
}

static int64_t wait_child(long pid) {
    int64_t status = -1;
    long r;
    do { r = sys3(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0); } while (r == SYSCALL_EINTR);
    return r < 0 ? -1000 : status;
}

static sysinfo_t info(void) {
    sysinfo_t s = {0};
    sys3(SYS_SYSINFO, (uintptr_t)&s, 0, 0);
    return s;
}

static int child(void) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, 3, VA_A, 0);
    if ((long)(uintptr_t)p < 0) return 2;
    for (unsigned i = 0; i < 64; i++) if (p[i] != (uint8_t)(i * 7 + 1)) return 3;
    if (p[PAGES * 4096 - 1] != 0x5a) return 4;
    p[4096] = 0xc3; /* answer, seen by the parent through its own mapping */
    return 0; /* exits with the mapping still in place: teardown drops it */
}

static int child_ro(void) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, 3, VA_A, SHM_MAP_READONLY);
    if ((long)(uintptr_t)p < 0) return 2;
    if (p[0] != 1) return 3;
    say("[SHMTEST] child-ro: read ok, writing through read-only mapping", NULL);
    p[0] = 0xee; /* must fault */
    say("[SHMTEST] child-ro: write did NOT fault", NULL);
    return 0;
}

/* One full lifecycle; returns false on any unexpected result. */
static bool cycle(void) {
    long fd = sys3(SYS_SHM_CREATE, 1 << 20, VFS_O_CLOEXEC, 0);
    if (fd < 0) return false;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_C, 0);
    if ((long)(uintptr_t)p < 0) return false;
    p[0] = 1;
    sys3(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    return sys3(SYS_SHM_UNMAP, VA_C, 0, 0) == 0;
}

static int run(void) {
    /* Argument validation. */
    check(sys3(SYS_SHM_CREATE, 0, 0, 0) == SYSCALL_EINVAL, "create(0) rejected");
    check(sys3(SYS_SHM_CREATE, 33ULL << 20, 0, 0) == SYSCALL_EINVAL, "create(33 MiB) rejected");
    check(sys3(SYS_SHM_CREATE, 4096, 0x1234, 0) == SYSCALL_EINVAL, "unknown create flags rejected");
    long fd = sys3(SYS_SHM_CREATE, SIZE, 0, 0);
    check(fd >= 0, "create(10000) returns an fd");
    if (fd < 0) return 1;
    stat_ext_v1_t st = {.size = sizeof(st), .version = 1};
    check(sys3(SYS_FSTAT, (uintptr_t)fd, (uintptr_t)&st, 0) == 0 && st.file_size == PAGES * 4096,
          "fstat size rounded to 3 pages");
    check(sys3(SYS_SHM_MAP, 1, VA_A, 0) == SYSCALL_EBADF, "map of a non-shm fd rejected");
    check(sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_A + 1, 0) == SYSCALL_EINVAL, "unaligned address rejected");
    check(sys3(SYS_SHM_MAP, (uintptr_t)fd, 0x1000, 0) == SYSCALL_EINVAL, "low address rejected");
    check(sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_A, 8) == SYSCALL_EINVAL, "unknown map flags rejected");

    /* Map, zeroed, write. */
    volatile uint8_t *a = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_A, 0);
    check((uintptr_t)a == VA_A, "map read-write at requested address");
    bool zero = true;
    for (unsigned i = 0; i < PAGES * 4096; i++) if (a[i]) zero = false;
    check(zero, "new object is zero-filled");
    for (unsigned i = 0; i < 64; i++) a[i] = (uint8_t)(i * 7 + 1);
    a[PAGES * 4096 - 1] = 0x5a;
    check(sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_A, 0) == SYSCALL_EEXIST, "mapping over a mapping rejected");

    /* Second, read-only view of the same pages in the same process. */
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, (uintptr_t)fd, VA_B, SHM_MAP_READONLY);
    check((uintptr_t)b == VA_B && b[5] == 36 && b[PAGES * 4096 - 1] == 0x5a, "second mapping aliases the same pages");
    a[100] = 0x77;
    check(b[100] == 0x77, "write through one mapping visible in the other");
    check(sys3(SYS_WRITE, 1, VA_A, 4) == SYSCALL_EFAULT, "shared pages refused as syscall buffers");

    /* Another process, handed the fd by spawn DUP2. */
    long pid = spawn_child("child", (int)fd);
    check(pid > 0 && wait_child(pid) == 0, "child mapped inherited fd and verified data");
    check(a[4096] == 0xc3, "child's write visible to the parent");
    pid = spawn_child("child-ro", (int)fd);
    int64_t status = pid > 0 ? wait_child(pid) : 0;
    check(pid > 0 && status != 0 && a[0] == 1, "write through read-only mapping kills the writer");

    /* The fd can go away while mappings keep the pages alive. */
    check(sys3(SYS_CLOSE, (uintptr_t)fd, 0, 0) == 0, "close fd while mapped");
    check(a[5] == 36 && b[100] == 0x77, "mappings survive closing the fd");
    check(sys3(SYS_SHM_UNMAP, VA_A, 0, 0) == 0, "unmap first mapping");
    check(sys3(SYS_SHM_UNMAP, VA_A, 0, 0) == SYSCALL_EINVAL, "second unmap of same address rejected");
    check(b[5] == 36, "other mapping still valid after unmap");
    check(sys3(SYS_SHM_UNMAP, VA_B, 0, 0) == 0, "unmap last mapping (object freed)");

    /* Leaks: after warm-up (page tables at VA_C), repeated full cycles and
     * children exiting with live mappings must return every frame and byte. */
    if (!cycle()) check(false, "warm-up cycle");
    sysinfo_t before = info();
    bool all = true;
    for (int i = 0; i < 40 && all; i++) all = cycle();
    /* 256 pages: losing this one object at child exit exceeds the tolerance. */
    long cfd = sys3(SYS_SHM_CREATE, 1 << 20, 0, 0);
    volatile uint8_t *c = (volatile uint8_t *)(uintptr_t)sys3(SYS_SHM_MAP, (uintptr_t)cfd, VA_A, 0);
    for (unsigned i = 0; i < 64; i++) c[i] = (uint8_t)(i * 7 + 1);
    c[PAGES * 4096 - 1] = 0x5a;
    for (int i = 0; i < 10 && all; i++) all = wait_child(spawn_child("child", (int)cfd)) == 0;
    sys3(SYS_CLOSE, (uintptr_t)cfd, 0, 0);
    sys3(SYS_SHM_UNMAP, VA_A, 0, 0);
    sysinfo_t after = info();
    check(all, "40 create/map/close/unmap cycles and 10 child exits");
    char line[160] = "free RAM before/after: ";
    size_t at = tool_length(line);
    at += tool_format_u64(line + at, before.free_ram_bytes / 4096);
    line[at++] = '/';
    at += tool_format_u64(line + at, after.free_ram_bytes / 4096);
    const char *mid = " pages, kernel heap ";
    for (size_t i = 0; mid[i]; i++) line[at++] = mid[i];
    at += tool_format_u64(line + at, before.kernel_heap_used);
    line[at++] = '/';
    at += tool_format_u64(line + at, after.kernel_heap_used);
    line[at] = 0;
    say("[SHMTEST] info ", line);
    /* Background services allocate and free concurrently, so exact equality
     * is noise. A real leak is far larger than the tolerance: 40 x 1 MiB
     * objects leak 10240 frames, their bookkeeping >= 80 KiB of heap. */
    check(after.free_ram_bytes + 64 * 4096 >= before.free_ram_bytes, "no frames leaked (tolerance 64 pages)");
    check(after.kernel_heap_used <= before.kernel_heap_used + 16384, "no kernel heap leaked (tolerance 16 KiB)");

    say(failures ? "[SHMTEST] RESULT FAIL" : "[SHMTEST] RESULT PASS", NULL);
    return failures ? 1 : 0;
}

int shm_test_main(int argc, char **argv) {
    if (argc == 2 && tool_equal(argv[1], "child")) return child();
    if (argc == 2 && tool_equal(argv[1], "child-ro")) return child_ro();
    return run();
}
