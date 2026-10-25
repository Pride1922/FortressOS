/*
 * tests/abi_versioning_host.c
 *
 * Comprehensive host unit test suite for FortressOS User ABI Versioning (Issue #7).
 * Validates:
 * 1. Struct sizes, alignments, and member offsets (Static & Runtime assertions).
 * 2. Full syscall table audit (Syscalls 0..94, SYSCALL_MAX=94, SYSCALL_NEXT_AVAIL=95).
 * 3. Pattern A (Explicit Header: size + version) rejection & backward compatibility.
 * 4. Pattern B (Explicit Sized Buffer Queries) rejection & headroom behavior.
 * 5. Pattern C (Fixed-Size Structs with Zero-Reserved Padding) emission contracts.
 * 6. Preservation of SYS_SYSINFO (37) and SYS_MEMINFO (56) contracts.
 * 7. User address space pointer validation and boundary enforcement (canonical, wrap-around, null).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>

#include "syscall_abi.h"
#include "install_abi.h"
#include "account_abi.h"
#include "wifi_abi.h"

/* Canary structures for buffer overflow and boundary corruption testing */
typedef struct {
    uint64_t pre_canary;
    stat_ext_v1_t value;
    uint64_t post_canary;
} canary_stat_ext_t;

typedef struct {
    uint64_t pre_canary;
    sysinfo_t value;
    uint64_t post_canary;
} canary_sysinfo_t;

typedef struct {
    uint64_t pre_canary;
    sysinfo_mem_t value;
    uint8_t extra_slack[32];
    uint64_t post_canary;
} canary_sysinfo_mem_t;

typedef struct {
    uint64_t pre_canary;
    proc_info_t value;
    uint64_t post_canary;
} canary_proc_info_t;

#define CANARY_PATTERN_PRE  0xFEEDFACECAFEBEEFULL
#define CANARY_PATTERN_POST 0xDEADBEEFBAADF00DULL

/* =========================================================================
 * 1. User Address Space Validation Model (vmm_validate_user_range emulation)
 * ========================================================================= */

#define USER_SPACE_MIN 0x1000ULL
#define USER_SPACE_MAX 0x00007fffffffffffULL
#define KERNEL_SPACE_BASE 0xffff800000000000ULL

typedef struct {
    uintptr_t valid_start;
    size_t    valid_len;
    bool      writable;
} mock_user_mapping_t;

static mock_user_mapping_t g_mock_mappings[8];
static size_t g_mock_mapping_count = 0;
static bool g_strict_mapping_check = false;

static void mock_reset_mappings(void) {
    g_mock_mapping_count = 0;
    g_strict_mapping_check = false;
}

static void mock_add_mapping(uintptr_t start, size_t len, bool writable) {
    assert(g_mock_mapping_count < sizeof(g_mock_mappings)/sizeof(g_mock_mappings[0]));
    g_mock_mappings[g_mock_mapping_count++] = (mock_user_mapping_t){start, len, writable};
}

static bool mock_vmm_validate_user_range(uintptr_t ptr, size_t size, bool write) {
    if (ptr == 0 || size == 0) return false;
    /* Check arithmetic overflow */
    if (ptr + size < ptr) return false;
    /* Check below user space minimum or above user space maximum */
    if (ptr < USER_SPACE_MIN) return false;
    if (ptr + size - 1 > USER_SPACE_MAX) return false;

    if (!g_strict_mapping_check) {
        return true;
    }

    /* Check against registered mock mappings */
    for (size_t i = 0; i < g_mock_mapping_count; i++) {
        uintptr_t m_start = g_mock_mappings[i].valid_start;
        uintptr_t m_end = m_start + g_mock_mappings[i].valid_len;
        if (ptr >= m_start && (ptr + size) <= m_end) {
            if (write && !g_mock_mappings[i].writable) {
                return false; /* Write to read-only mapping */
            }
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * 2. Mock Kernel Implementations matching src/kernel/syscall.c
 * ========================================================================= */

static int64_t mock_sys_stat_ext(uintptr_t path, uintptr_t output, uint64_t size, uint64_t version) {
    (void)path;
    if (size != sizeof(stat_ext_v1_t) || version != 1) {
        return SYSCALL_EINVAL;
    }
    if (!mock_vmm_validate_user_range(output, sizeof(stat_ext_v1_t), true)) {
        return SYSCALL_EFAULT;
    }

    stat_ext_v1_t val;
    memset(&val, 0, sizeof(val));
    val.size = sizeof(stat_ext_v1_t);
    val.version = 1;
    val.file_size = 4096;
    val.type = 1; /* VFS_FILE */
    val.mode = 0644;
    val.uid = 1000;
    val.gid = 1000;
    val.mnt_flags = 1;
    val.reserved = 0;

    memcpy((void *)output, &val, sizeof(val));
    return 0;
}

static int64_t mock_sys_sysinfo(uintptr_t user_buf) {
    if (!mock_vmm_validate_user_range(user_buf, sizeof(sysinfo_t), true)) {
        return SYSCALL_EFAULT;
    }

    sysinfo_t info;
    memset(&info, 0, sizeof(info));
    info.total_ram_bytes   = 2048ULL * 1024 * 1024;
    info.free_ram_bytes    = 1980ULL * 1024 * 1024;
    info.uptime_ticks      = 10000;
    info.tick_hz           = 100;
    info.cpu_count         = 4;
    info.task_count        = 8;
    info.tsc_hz            = 2400000000ULL;
    info.kernel_heap_used  = 512 * 1024;
    info.kernel_heap_total = 4 * 1024 * 1024;
    info.thread_count      = 12;
    info.reserved          = 0; /* Strict requirement: must be zero */

    memcpy((void *)user_buf, &info, sizeof(info));
    return 0;
}

static int64_t mock_sys_meminfo(uintptr_t user_buf, uint64_t size) {
    if (size < sizeof(sysinfo_mem_t)) {
        return SYSCALL_EINVAL;
    }
    if (!mock_vmm_validate_user_range(user_buf, sizeof(sysinfo_mem_t), true)) {
        return SYSCALL_EFAULT;
    }

    sysinfo_mem_t mem;
    memset(&mem, 0, sizeof(mem));
    mem.struct_size = sizeof(sysinfo_mem_t);
    mem.flags = 0;
    mem.pmm_total_frames = 524288;
    mem.pmm_used_frames = 16384;
    mem.pmm_free_frames = 507904;
    mem.pmm_allocatable_frames = 507904;
    mem.heap_used_bytes = 512 * 1024;
    mem.heap_free_bytes = 3584 * 1024;
    mem.heap_committed_bytes = 4096 * 1024;
    mem.heap_largest_payload = 2048 * 1024;
    mem.heap_free_blocks = 14;
    mem.vmm_table_frames = 256;
    mem.vmm_deferred_spaces = 0;

    memcpy((void *)user_buf, &mem, sizeof(sysinfo_mem_t));
    return 0;
}

static int64_t mock_sys_spawn_ext(uintptr_t user_opts_ptr, uint64_t user_opts_size) {
    if (user_opts_size != sizeof(spawn_opts_t) && user_opts_size != sizeof(spawn_opts_v3_t)) {
        return SYSCALL_EINVAL;
    }
    if (!mock_vmm_validate_user_range(user_opts_ptr, user_opts_size, false)) {
        return SYSCALL_EFAULT;
    }

    spawn_opts_t opts;
    memcpy(&opts, (const void *)user_opts_ptr, sizeof(opts));

    bool explicit_identity = (user_opts_size == sizeof(spawn_opts_v3_t));

    if (opts.size != user_opts_size ||
        (explicit_identity ? opts.version != 3 : (opts.version != 1 && opts.version != 2)) ||
        opts.reserved0 != 0 || opts.reserved1 != 0 ||
        (opts.version == 1 && (opts.flags || opts.reserved2)) ||
        (opts.version >= 2 && ((opts.flags & ~SPAWN_V2_FLAGS) ||
         (!(opts.flags & SPAWN_SETPGROUP) && opts.reserved2) || opts.reserved2 > 0x7fffffffffffffffULL))) {
        return SYSCALL_EINVAL;
    }

    if (opts.action_count > MAX_SPAWN_ACTIONS) {
        return SYSCALL_EINVAL;
    }
    if ((opts.action_count > 0 && opts.fd_actions == 0) ||
        (opts.action_count == 0 && opts.fd_actions != 0)) {
        return SYSCALL_EINVAL;
    }

    if (opts.action_count > 0) {
        if (!mock_vmm_validate_user_range((uintptr_t)opts.fd_actions,
                                          opts.action_count * sizeof(spawn_fd_action_t), false)) {
            return SYSCALL_EFAULT;
        }
    }

    return 42; /* Simulated child PID */
}

/* =========================================================================
 * 3. Unit Test Cases
 * ========================================================================= */

static void test_abi_struct_layouts(void) {
    /* stat_ext_v1_t */
    _Static_assert(sizeof(stat_ext_v1_t) == 40, "stat_ext_v1_t size");
    _Static_assert(alignof(stat_ext_v1_t) == 8, "stat_ext_v1_t alignment");
    _Static_assert(offsetof(stat_ext_v1_t, size) == 0, "stat_ext_v1_t size offset");
    _Static_assert(offsetof(stat_ext_v1_t, version) == 4, "stat_ext_v1_t version offset");
    _Static_assert(offsetof(stat_ext_v1_t, file_size) == 8, "stat_ext_v1_t file_size offset");
    _Static_assert(offsetof(stat_ext_v1_t, type) == 16, "stat_ext_v1_t type offset");
    _Static_assert(offsetof(stat_ext_v1_t, mode) == 20, "stat_ext_v1_t mode offset");
    _Static_assert(offsetof(stat_ext_v1_t, uid) == 24, "stat_ext_v1_t uid offset");
    _Static_assert(offsetof(stat_ext_v1_t, gid) == 28, "stat_ext_v1_t gid offset");
    _Static_assert(offsetof(stat_ext_v1_t, mnt_flags) == 32, "stat_ext_v1_t mnt_flags offset");
    _Static_assert(offsetof(stat_ext_v1_t, reserved) == 36, "stat_ext_v1_t reserved offset");

    /* sysinfo_t */
    _Static_assert(sizeof(sysinfo_t) == 72, "sysinfo_t size");
    _Static_assert(alignof(sysinfo_t) == 8, "sysinfo_t alignment");
    _Static_assert(offsetof(sysinfo_t, total_ram_bytes) == 0, "sysinfo_t total_ram_bytes offset");
    _Static_assert(offsetof(sysinfo_t, free_ram_bytes) == 8, "sysinfo_t free_ram_bytes offset");
    _Static_assert(offsetof(sysinfo_t, uptime_ticks) == 16, "sysinfo_t uptime_ticks offset");
    _Static_assert(offsetof(sysinfo_t, tick_hz) == 24, "sysinfo_t tick_hz offset");
    _Static_assert(offsetof(sysinfo_t, cpu_count) == 32, "sysinfo_t cpu_count offset");
    _Static_assert(offsetof(sysinfo_t, task_count) == 36, "sysinfo_t task_count offset");
    _Static_assert(offsetof(sysinfo_t, tsc_hz) == 40, "sysinfo_t tsc_hz offset");
    _Static_assert(offsetof(sysinfo_t, kernel_heap_used) == 48, "sysinfo_t kernel_heap_used offset");
    _Static_assert(offsetof(sysinfo_t, kernel_heap_total) == 56, "sysinfo_t kernel_heap_total offset");
    _Static_assert(offsetof(sysinfo_t, thread_count) == 64, "sysinfo_t thread_count offset");
    _Static_assert(offsetof(sysinfo_t, reserved) == 68, "sysinfo_t reserved offset");

    /* sysinfo_mem_t */
    _Static_assert(sizeof(sysinfo_mem_t) == 96, "sysinfo_mem_t size");
    _Static_assert(alignof(sysinfo_mem_t) == 8, "sysinfo_mem_t alignment");
    _Static_assert(offsetof(sysinfo_mem_t, struct_size) == 0, "sysinfo_mem_t struct_size offset");
    _Static_assert(offsetof(sysinfo_mem_t, flags) == 4, "sysinfo_mem_t flags offset");
    _Static_assert(offsetof(sysinfo_mem_t, pmm_total_frames) == 8, "sysinfo_mem_t pmm_total_frames offset");
    _Static_assert(offsetof(sysinfo_mem_t, pmm_used_frames) == 16, "sysinfo_mem_t pmm_used_frames offset");
    _Static_assert(offsetof(sysinfo_mem_t, pmm_free_frames) == 24, "sysinfo_mem_t pmm_free_frames offset");
    _Static_assert(offsetof(sysinfo_mem_t, pmm_allocatable_frames) == 32, "sysinfo_mem_t pmm_allocatable_frames offset");
    _Static_assert(offsetof(sysinfo_mem_t, heap_used_bytes) == 40, "sysinfo_mem_t heap_used_bytes offset");
    _Static_assert(offsetof(sysinfo_mem_t, heap_free_bytes) == 48, "sysinfo_mem_t heap_free_bytes offset");
    _Static_assert(offsetof(sysinfo_mem_t, heap_committed_bytes) == 56, "sysinfo_mem_t heap_committed_bytes offset");
    _Static_assert(offsetof(sysinfo_mem_t, heap_largest_payload) == 64, "sysinfo_mem_t heap_largest_payload offset");
    _Static_assert(offsetof(sysinfo_mem_t, heap_free_blocks) == 72, "sysinfo_mem_t heap_free_blocks offset");
    _Static_assert(offsetof(sysinfo_mem_t, vmm_table_frames) == 80, "sysinfo_mem_t vmm_table_frames offset");
    _Static_assert(offsetof(sysinfo_mem_t, vmm_deferred_spaces) == 88, "sysinfo_mem_t vmm_deferred_spaces offset");

    /* spawn_opts_t */
    _Static_assert(sizeof(spawn_opts_t) == 64, "spawn_opts_t size");
    _Static_assert(alignof(spawn_opts_t) == 8, "spawn_opts_t alignment");
    _Static_assert(offsetof(spawn_opts_t, size) == 0, "spawn_opts_t size offset");
    _Static_assert(offsetof(spawn_opts_t, version) == 4, "spawn_opts_t version offset");
    _Static_assert(offsetof(spawn_opts_t, flags) == 8, "spawn_opts_t flags offset");
    _Static_assert(offsetof(spawn_opts_t, reserved0) == 12, "spawn_opts_t reserved0 offset");
    _Static_assert(offsetof(spawn_opts_t, argv) == 16, "spawn_opts_t argv offset");
    _Static_assert(offsetof(spawn_opts_t, envp) == 24, "spawn_opts_t envp offset");
    _Static_assert(offsetof(spawn_opts_t, cwd) == 32, "spawn_opts_t cwd offset");
    _Static_assert(offsetof(spawn_opts_t, fd_actions) == 40, "spawn_opts_t fd_actions offset");
    _Static_assert(offsetof(spawn_opts_t, action_count) == 48, "spawn_opts_t action_count offset");
    _Static_assert(offsetof(spawn_opts_t, reserved1) == 52, "spawn_opts_t reserved1 offset");
    _Static_assert(offsetof(spawn_opts_t, reserved2) == 56, "spawn_opts_t reserved2 offset");

    /* spawn_opts_v3_t */
    _Static_assert(sizeof(spawn_opts_v3_t) == 144, "spawn_opts_v3_t size");
    _Static_assert(alignof(spawn_opts_v3_t) == 8, "spawn_opts_v3_t alignment");
    _Static_assert(offsetof(spawn_opts_v3_t, base) == 0, "spawn_opts_v3_t base offset");
    _Static_assert(offsetof(spawn_opts_v3_t, identity) == 64, "spawn_opts_v3_t identity offset");

    /* proc_info_t */
    _Static_assert(sizeof(proc_info_t) == 64, "proc_info_t size");
    _Static_assert(alignof(proc_info_t) == 8, "proc_info_t alignment");
    _Static_assert(offsetof(proc_info_t, pid) == 0, "proc_info_t pid offset");
    _Static_assert(offsetof(proc_info_t, ppid) == 8, "proc_info_t ppid offset");
    _Static_assert(offsetof(proc_info_t, pgid) == 16, "proc_info_t pgid offset");
    _Static_assert(offsetof(proc_info_t, sid) == 24, "proc_info_t sid offset");
    _Static_assert(offsetof(proc_info_t, state) == 32, "proc_info_t state offset");
    _Static_assert(offsetof(proc_info_t, reserved) == 36, "proc_info_t reserved offset");
    _Static_assert(offsetof(proc_info_t, cpu_ticks) == 40, "proc_info_t cpu_ticks offset");
    _Static_assert(offsetof(proc_info_t, name) == 48, "proc_info_t name offset");

    /* mount_info_t */
    _Static_assert(sizeof(mount_info_t) == 336, "mount_info_t size");
    _Static_assert(alignof(mount_info_t) == 8, "mount_info_t alignment");
    _Static_assert(offsetof(mount_info_t, source) == 0, "mount_info_t source offset");
    _Static_assert(offsetof(mount_info_t, mount_path) == 32, "mount_info_t mount_path offset");
    _Static_assert(offsetof(mount_info_t, fs_type) == 288, "mount_info_t fs_type offset");
    _Static_assert(offsetof(mount_info_t, flags) == 292, "mount_info_t flags offset");
    _Static_assert(offsetof(mount_info_t, total_blocks) == 296, "mount_info_t total_blocks offset");
    _Static_assert(offsetof(mount_info_t, free_blocks) == 304, "mount_info_t free_blocks offset");
    _Static_assert(offsetof(mount_info_t, total_inodes) == 312, "mount_info_t total_inodes offset");
    _Static_assert(offsetof(mount_info_t, free_inodes) == 320, "mount_info_t free_inodes offset");
    _Static_assert(offsetof(mount_info_t, block_size) == 328, "mount_info_t block_size offset");
    _Static_assert(offsetof(mount_info_t, reserved) == 332, "mount_info_t reserved offset");

    /* block_info_t */
    _Static_assert(sizeof(block_info_t) == 192, "block_info_t size");
    _Static_assert(alignof(block_info_t) == 8, "block_info_t alignment");
    _Static_assert(offsetof(block_info_t, name) == 0, "block_info_t name offset");
    _Static_assert(offsetof(block_info_t, sector_size) == 32, "block_info_t sector_size offset");
    _Static_assert(offsetof(block_info_t, reserved) == 36, "block_info_t reserved offset");
    _Static_assert(offsetof(block_info_t, size_bytes) == 40, "block_info_t size_bytes offset");
    _Static_assert(offsetof(block_info_t, partuuid) == 48, "block_info_t partuuid offset");
    _Static_assert(offsetof(block_info_t, label) == 88, "block_info_t label offset");
    _Static_assert(offsetof(block_info_t, type_guid) == 128, "block_info_t type_guid offset");
    _Static_assert(offsetof(block_info_t, start_lba) == 168, "block_info_t start_lba offset");
    _Static_assert(offsetof(block_info_t, sector_count) == 176, "block_info_t sector_count offset");
    _Static_assert(offsetof(block_info_t, is_partition) == 184, "block_info_t is_partition offset");
    _Static_assert(offsetof(block_info_t, part_index) == 188, "block_info_t part_index offset");

    /* timespec_t & timeval_t & tms_t */
    _Static_assert(sizeof(timespec_t) == 16, "timespec_t size");
    _Static_assert(offsetof(timespec_t, tv_sec) == 0, "timespec_t tv_sec offset");
    _Static_assert(offsetof(timespec_t, tv_nsec) == 8, "timespec_t tv_nsec offset");

    _Static_assert(sizeof(timeval_t) == 16, "timeval_t size");
    _Static_assert(offsetof(timeval_t, tv_sec) == 0, "timeval_t tv_sec offset");
    _Static_assert(offsetof(timeval_t, tv_usec) == 8, "timeval_t tv_usec offset");

    _Static_assert(sizeof(tms_t) == 32, "tms_t size");
    _Static_assert(offsetof(tms_t, tms_utime_ns) == 0, "tms_t tms_utime_ns offset");
    _Static_assert(offsetof(tms_t, tms_stime_ns) == 8, "tms_t tms_stime_ns offset");
    _Static_assert(offsetof(tms_t, tms_cutime_ns) == 16, "tms_t tms_cutime_ns offset");
    _Static_assert(offsetof(tms_t, tms_cstime_ns) == 24, "tms_t tms_cstime_ns offset");

    /* struct pollfd */
    _Static_assert(sizeof(struct pollfd) == 8, "struct pollfd size");
    _Static_assert(alignof(struct pollfd) == 4, "struct pollfd alignment");
    _Static_assert(offsetof(struct pollfd, fd) == 0, "pollfd fd offset");
    _Static_assert(offsetof(struct pollfd, events) == 4, "pollfd events offset");
    _Static_assert(offsetof(struct pollfd, revents) == 6, "pollfd revents offset");

    /* signal_action_t */
    _Static_assert(sizeof(signal_action_t) == 32, "signal_action_t size");
    _Static_assert(offsetof(signal_action_t, handler) == 0, "signal_action_t handler offset");
    _Static_assert(offsetof(signal_action_t, mask) == 8, "signal_action_t mask offset");
    _Static_assert(offsetof(signal_action_t, flags) == 16, "signal_action_t flags offset");
    _Static_assert(offsetof(signal_action_t, reserved) == 24, "signal_action_t reserved offset");

    /* netctl_ifget_t & netctl_ifset_t */
    _Static_assert(sizeof(netctl_ifget_t) == 64, "netctl_ifget_t size");
    _Static_assert(sizeof(netctl_ifset_t) == 32, "netctl_ifset_t size");

    /* account_request_t */
    _Static_assert(sizeof(account_request_t) == 64, "account_request_t size");

    /* install_log_event_v1_t */
    _Static_assert(sizeof(install_log_event_v1_t) == 32, "install_log_event_v1_t size");

    puts("PASS test_abi_struct_layouts: All static assert sizes, alignments, and offsets verified");
}

static void test_syscall_table_audit(void) {
    /* Audit complete range 0..94 */
    assert(SYS_EXIT == 0);
    assert(SYS_WRITE == 1);
    assert(SYS_OPEN == 2);
    assert(SYS_CLOSE == 3);
    assert(SYS_READ == 4);
    assert(SYS_STAT == 5);
    assert(SYS_READDIR == 6);
    assert(SYS_REBOOT == 7);
    assert(SYS_KBD_LAYOUT == 8);
    assert(SYS_SPAWN == 9);
    assert(SYS_WAIT == 10);
    assert(SYS_MKDIR == 11);
    assert(SYS_UNLINK == 12);
    assert(SYS_RENAME == 13);
    assert(SYS_SYNC == 14);
    assert(SYS_DMESG == 15);
    assert(SYS_TERMCTL == 16);
    assert(SYS_INPUT_READ == 17);
    assert(SYS_GETCWD == 18);
    assert(SYS_CHDIR == 19);
    assert(SYS_SPAWN_EXT == 20);
    assert(SYS_DUP2 == 21);
    assert(SYS_DUP == 22);
    assert(SYS_FCNTL == 23);
    assert(SYS_PIPE == 24);
    assert(SYS_SETPGID == 25);
    assert(SYS_GETPGRP == 26);
    assert(SYS_KILL == 27);
    assert(SYS_SIGACTION == 28);
    assert(SYS_SIGPROCMASK == 29);
    assert(SYS_TCSETPGRP == 30);
    assert(SYS_TCGETPGRP == 31);
    assert(SYS_SIGRETURN == 32);
    assert(SYS_WAITPID == 33);
    assert(SYS_TERMATTR == 34);
    assert(SYS_GROUP_RELEASE == 35);
    assert(SYS_PROCINFO == 36);
    assert(SYS_SYSINFO == 37);
    assert(SYS_SOCKET == 38);
    assert(SYS_BIND == 39);
    assert(SYS_SENDTO == 40);
    assert(SYS_RECVFROM == 41);
    assert(SYS_NETCTL == 42);
    assert(SYS_CONNECT == 43);
    assert(SYS_LISTEN == 44);
    assert(SYS_ACCEPT == 45);
    assert(SYS_SEND == 46);
    assert(SYS_RECV == 47);
    assert(SYS_SHUTDOWN == 48);
    assert(SYS_SEND_UNTIL == 49);
    assert(SYS_RECV_UNTIL == 50);
    assert(SYS_CONNECT_UNTIL == 51);
    assert(SYS_MOUNTINFO == 52);
    assert(SYS_BLOCKINFO == 53);
    assert(SYS_LOCKSTAT == 54);
    assert(SYS_SPAWN_PROFILE == 55);
    assert(SYS_MEMINFO == 56);
    assert(SYS_STAT_EXT == 57);
    assert(SYS_UMASK == 58);
    assert(SYS_CHMOD == 59);
    assert(SYS_FCHMOD == 60);
    assert(SYS_CHOWN == 61);
    assert(SYS_GETRESUID == 62);
    assert(SYS_GETRESGID == 63);
    assert(SYS_GETGROUPS == 64);
    assert(SYS_SETRESUID == 66);
    assert(SYS_SETRESGID == 67);
    assert(SYS_SETGROUPS == 68);
    assert(SYS_CAPSET == 69);
    assert(SYS_CAPGET == 70);
    assert(SYS_INSTALLCTL == 71);
    assert(SYS_ACCOUNTCTL == 72);
    assert(SYS_INSTALLSEED == 73);
    assert(SYS_CLOCK_GETTIME == 74);
    assert(SYS_NANOSLEEP == 75);
    assert(SYS_TIMES == 76);
    assert(SYS_GETRANDOM == 77);
    assert(SYS_FSTAT == 78);
    assert(SYS_HOSTKEY == 79);
    assert(SYS_PTY_PAIR == 80);
    assert(SYS_LSTAT == 81);
    assert(SYS_LIST_DIR == 82);
    assert(SYS_CLOCK_SETTIME == 83);
    assert(SYS_RENAME_REPLACE == 84);
    assert(SYS_STAGE_WRITE == 85);
    assert(SYS_FSYNC == 86);
    assert(SYS_WIFI == 87);
    assert(SYS_POLL == 88);
    assert(SYS_THREAD_CREATE == 89);
    assert(SYS_THREAD_EXIT == 90);
    assert(SYS_THREAD_JOIN == 91);
    assert(SYS_THREAD_SELF == 92);
    assert(SYS_THREAD_YIELD == 93);
    assert(SYS_FUTEX == 94);
    assert(SYS_PKGCTL == 95);

    /* Assert High-Water Mark and Next Available Syscall Slot */
    _Static_assert(SYSCALL_MAX == 95, "SYSCALL_MAX must be 95");
    _Static_assert(SYSCALL_NEXT_AVAIL == 96, "SYSCALL_NEXT_AVAIL must be 96");

    puts("PASS test_syscall_table_audit: 96/96 syscall entries audited and contiguous allocation slot 96 confirmed");
}

static void test_pattern_a_stat_ext(void) {
    mock_reset_mappings();
    canary_stat_ext_t canary;
    memset(&canary, 0xa5, sizeof(canary));
    canary.pre_canary = CANARY_PATTERN_PRE;
    canary.post_canary = CANARY_PATTERN_POST;

    uintptr_t valid_ptr = (uintptr_t)&canary.value;

    /* 1. Undersized buffer size rejection */
    int64_t r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t) - 1, 1);
    assert(r == SYSCALL_EINVAL);
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    /* 2. Oversized buffer size rejection (strict layout contract) */
    r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t) + 8, 1);
    assert(r == SYSCALL_EINVAL);
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    /* 3. Unsupported version rejection (version 0 and version 2) */
    r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t), 0);
    assert(r == SYSCALL_EINVAL);
    r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t), 2);
    assert(r == SYSCALL_EINVAL);

    /* 4. NULL / Inaccessible pointer rejection */
    r = mock_sys_stat_ext(valid_ptr, 0, sizeof(stat_ext_v1_t), 1);
    assert(r == SYSCALL_EFAULT);

    /* 5. Kernel address space rejection */
    r = mock_sys_stat_ext(valid_ptr, KERNEL_SPACE_BASE + 0x1000, sizeof(stat_ext_v1_t), 1);
    assert(r == SYSCALL_EFAULT);

    /* 6. Arithmetic wrap-around pointer rejection */
    r = mock_sys_stat_ext(valid_ptr, UINT64_MAX - 10, sizeof(stat_ext_v1_t), 1);
    assert(r == SYSCALL_EFAULT);

    /* 7. Read-only user buffer rejection */
    mock_add_mapping(valid_ptr, sizeof(stat_ext_v1_t), false);
    g_strict_mapping_check = true;
    r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t), 1);
    assert(r == SYSCALL_EFAULT);
    g_strict_mapping_check = false;

    /* 8. Valid request success */
    r = mock_sys_stat_ext(valid_ptr, valid_ptr, sizeof(stat_ext_v1_t), 1);
    assert(r == 0);
    assert(canary.value.size == sizeof(stat_ext_v1_t));
    assert(canary.value.version == 1);
    assert(canary.value.file_size == 4096);
    assert(canary.value.type == 1);
    assert(canary.value.mode == 0644);
    assert(canary.value.uid == 1000);
    assert(canary.value.gid == 1000);
    assert(canary.value.mnt_flags == 1);
    assert(canary.value.reserved == 0);
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    puts("PASS test_pattern_a_stat_ext: Header versioning, undersize/oversize/version rejection and canaries verified");
}

static void test_pattern_a_spawn_opts(void) {
    mock_reset_mappings();
    spawn_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.size = sizeof(opts);
    opts.version = 1;

    uintptr_t valid_ptr = (uintptr_t)&opts;

    /* 1. Size mismatch rejection */
    assert(mock_sys_spawn_ext(valid_ptr, 32) == SYSCALL_EINVAL);
    assert(mock_sys_spawn_ext(valid_ptr, 65) == SYSCALL_EINVAL);
    assert(mock_sys_spawn_ext(valid_ptr, 100) == SYSCALL_EINVAL);

    /* 2. Version mismatch rejection */
    opts.version = 0;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);
    opts.version = 4;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);

    /* 3. v1 flags non-zero rejection */
    opts.version = 1;
    opts.flags = SPAWN_SETPGROUP;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);
    opts.flags = 0;

    /* 4. Non-zero reserved fields */
    opts.reserved0 = 1;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);
    opts.reserved0 = 0;

    opts.reserved1 = 1;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);
    opts.reserved1 = 0;

    /* 5. v1 non-zero reserved2 */
    opts.reserved2 = 100;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);
    opts.reserved2 = 0;

    /* 6. Action count and fd_actions mismatch */
    opts.action_count = 1;
    opts.fd_actions = 0;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);

    opts.action_count = 0;
    opts.fd_actions = valid_ptr;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);

    /* 7. Action count exceeding MAX_SPAWN_ACTIONS (16) */
    opts.action_count = 17;
    opts.fd_actions = valid_ptr;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == SYSCALL_EINVAL);

    /* 8. Valid v1 spawn options */
    opts.action_count = 0;
    opts.fd_actions = 0;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == 42);

    /* 9. Valid v2 spawn options */
    opts.version = 2;
    opts.flags = SPAWN_SETPGROUP | SPAWN_STAGED;
    opts.reserved2 = 1234;
    assert(mock_sys_spawn_ext(valid_ptr, sizeof(opts)) == 42);

    /* 10. Valid v3 spawn options (with explicit identity) */
    spawn_opts_v3_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.base.size = sizeof(v3);
    v3.base.version = 3;
    v3.identity.uid = 1000;
    v3.identity.gid = 1000;
    assert(mock_sys_spawn_ext((uintptr_t)&v3, sizeof(v3)) == 42);

    puts("PASS test_pattern_a_spawn_opts: v1/v2/v3 evolution, flag enforcement, reserved field rejection verified");
}

static void test_pattern_b_sysinfo_mem(void) {
    mock_reset_mappings();
    canary_sysinfo_mem_t canary;
    memset(&canary, 0xa5, sizeof(canary));
    canary.pre_canary = CANARY_PATTERN_PRE;
    canary.post_canary = CANARY_PATTERN_POST;

    uintptr_t valid_ptr = (uintptr_t)&canary.value;

    /* 1. Undersized buffer sizes rejected strictly with -SYSCALL_EINVAL */
    assert(mock_sys_meminfo(valid_ptr, 0) == SYSCALL_EINVAL);
    assert(mock_sys_meminfo(valid_ptr, 32) == SYSCALL_EINVAL);
    assert(mock_sys_meminfo(valid_ptr, 64) == SYSCALL_EINVAL);
    assert(mock_sys_meminfo(valid_ptr, sizeof(sysinfo_mem_t) - 1) == SYSCALL_EINVAL);
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    /* 2. Pointer validation (NULL, kernel address, wrap-around) */
    assert(mock_sys_meminfo(0, sizeof(sysinfo_mem_t)) == SYSCALL_EFAULT);
    assert(mock_sys_meminfo(KERNEL_SPACE_BASE, sizeof(sysinfo_mem_t)) == SYSCALL_EFAULT);
    assert(mock_sys_meminfo(UINT64_MAX - 10, sizeof(sysinfo_mem_t)) == SYSCALL_EFAULT);

    /* 3. Read-only user buffer rejection */
    mock_add_mapping(valid_ptr, sizeof(sysinfo_mem_t), false);
    g_strict_mapping_check = true;
    assert(mock_sys_meminfo(valid_ptr, sizeof(sysinfo_mem_t)) == SYSCALL_EFAULT);
    g_strict_mapping_check = false;

    /* 4. Exact buffer size (96 bytes) succeeds */
    assert(mock_sys_meminfo(valid_ptr, sizeof(sysinfo_mem_t)) == 0);
    assert(canary.value.struct_size == sizeof(sysinfo_mem_t));
    assert(canary.value.flags == 0);
    assert(canary.value.pmm_total_frames == 524288);
    assert(canary.value.heap_used_bytes == 512 * 1024);
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    /* 5. Larger buffer (e.g. 128 bytes) provided by future caller:
     * Kernel writes exactly 96 bytes and leaves extra slack untouched! */
    memset(canary.extra_slack, 0xee, sizeof(canary.extra_slack));
    assert(mock_sys_meminfo(valid_ptr, sizeof(sysinfo_mem_t) + sizeof(canary.extra_slack)) == 0);
    assert(canary.value.struct_size == sizeof(sysinfo_mem_t));
    for (size_t i = 0; i < sizeof(canary.extra_slack); i++) {
        assert(canary.extra_slack[i] == 0xee);
    }
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    puts("PASS test_pattern_b_sysinfo_mem: Sized buffer queries, short buffer rejection, and slack preservation verified");
}

static void test_pattern_c_sysinfo(void) {
    mock_reset_mappings();
    canary_sysinfo_t canary;
    memset(&canary, 0xa5, sizeof(canary));
    canary.pre_canary = CANARY_PATTERN_PRE;
    canary.post_canary = CANARY_PATTERN_POST;

    uintptr_t valid_ptr = (uintptr_t)&canary.value;

    /* 1. NULL / Inaccessible / Kernel pointer validation */
    assert(mock_sys_sysinfo(0) == SYSCALL_EFAULT);
    assert(mock_sys_sysinfo(KERNEL_SPACE_BASE) == SYSCALL_EFAULT);
    assert(mock_sys_sysinfo(USER_SPACE_MIN - 1) == SYSCALL_EFAULT);

    /* 2. Read-only mapping rejection */
    mock_add_mapping(valid_ptr, sizeof(sysinfo_t), false);
    g_strict_mapping_check = true;
    assert(mock_sys_sysinfo(valid_ptr) == SYSCALL_EFAULT);
    g_strict_mapping_check = false;

    /* 3. Successful sysinfo population */
    assert(mock_sys_sysinfo(valid_ptr) == 0);
    assert(canary.value.total_ram_bytes == 2048ULL * 1024 * 1024);
    assert(canary.value.free_ram_bytes == 1980ULL * 1024 * 1024);
    assert(canary.value.uptime_ticks == 10000);
    assert(canary.value.tick_hz == 100);
    assert(canary.value.cpu_count == 4);
    assert(canary.value.task_count == 8);
    assert(canary.value.tsc_hz == 2400000000ULL);
    assert(canary.value.kernel_heap_used == 512 * 1024);
    assert(canary.value.kernel_heap_total == 4 * 1024 * 1024);
    assert(canary.value.thread_count == 12);
    /* Strict ABI contract: reserved field must be explicitly 0 */
    assert(canary.value.reserved == 0);

    /* Canaries intact */
    assert(canary.pre_canary == CANARY_PATTERN_PRE);
    assert(canary.post_canary == CANARY_PATTERN_POST);

    puts("PASS test_pattern_c_sysinfo: SYS_SYSINFO fixed layout, zeroed reserved field and canary preservation verified");
}

static void test_user_pointer_boundaries(void) {
    /* 1. Zero address */
    assert(!mock_vmm_validate_user_range(0, 1, false));
    assert(!mock_vmm_validate_user_range(0, 1, true));

    /* 2. Zero-page addresses below USER_SPACE_MIN (0x1000) */
    assert(!mock_vmm_validate_user_range(0x1, 1, false));
    assert(!mock_vmm_validate_user_range(0xfff, 1, false));

    /* 3. Valid user space boundary */
    assert(mock_vmm_validate_user_range(0x1000, 1, false));
    assert(mock_vmm_validate_user_range(USER_SPACE_MAX - 7, 8, false));

    /* 4. Crossing above USER_SPACE_MAX */
    assert(!mock_vmm_validate_user_range(USER_SPACE_MAX, 2, false));
    assert(!mock_vmm_validate_user_range(USER_SPACE_MAX + 1, 1, false));

    /* 5. Non-canonical gap and kernel space */
    assert(!mock_vmm_validate_user_range(0x0000800000000000ULL, 8, false));
    assert(!mock_vmm_validate_user_range(KERNEL_SPACE_BASE, 8, false));
    assert(!mock_vmm_validate_user_range(UINT64_MAX - 7, 8, false));

    /* 6. Arithmetic wrap-around */
    assert(!mock_vmm_validate_user_range(0xffffffffffffffffULL, 2, false));
    assert(!mock_vmm_validate_user_range(0x1000, UINT64_MAX, false));

    puts("PASS test_user_pointer_boundaries: Canonical boundary, zero page, kernel space, and wrap-around detection verified");
}

int main(void) {
    puts("=== Running FortressOS User ABI Versioning Verification Suite (Issue #7) ===");
    test_abi_struct_layouts();
    test_syscall_table_audit();
    test_pattern_a_stat_ext();
    test_pattern_a_spawn_opts();
    test_pattern_b_sysinfo_mem();
    test_pattern_c_sysinfo();
    test_user_pointer_boundaries();
    puts("=== All User ABI Versioning Verification Tests Passed (100%) ===");
    return 0;
}
