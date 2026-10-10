#include "types.h"
#include "syscall_abi.h"

#ifndef BUILD_GIT_HASH
#define BUILD_GIT_HASH "unknown"
#endif

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

/* Static BSS storage to strictly respect the 512-byte Ring 3 stack budget. */
static sysinfo_t s_info;
static sysinfo_mem_t s_mem;
static proc_info_t s_proc;
static block_info_t s_block;
static mount_info_t s_mount;
static char s_buf[2048];

static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_meminfo(sysinfo_mem_t *buf, size_t size) {
    long nr = SYS_MEMINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf), "S"((uintptr_t)size)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_sysinfo(sysinfo_t *buf) {
    long nr = SYS_SYSINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_procinfo(uint64_t index, proc_info_t *buf) {
    long nr = SYS_PROCINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(index), "S"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_blockinfo(uint32_t index, block_info_t *buf) {
    long nr = SYS_BLOCKINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)index), "S"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_mountinfo(uint32_t index, mount_info_t *buf) {
    long nr = SYS_MOUNTINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)index), "S"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static int write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        long w = sys_write(fd, buf, count);
        if (w <= 0) {
            return -1;
        }
        buf += w;
        count -= (size_t)w;
    }
    return 0;
}

static void buf_append_char(char *buf, size_t *pos, size_t max, char c) {
    if (*pos + 1 < max) {
        buf[(*pos)++] = c;
    }
}

static void buf_append_str(char *buf, size_t *pos, size_t max, const char *str) {
    if (!str) return;
    while (*str != '\0' && *pos + 1 < max) {
        buf[(*pos)++] = *str++;
    }
}

static void buf_append_u64(char *buf, size_t *pos, size_t max, uint64_t val) {
    if (val == 0) {
        buf_append_char(buf, pos, max, '0');
        return;
    }
    char tmp[32];
    size_t tpos = 0;
    while (val > 0) {
        tmp[tpos++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (tpos > 0) {
        buf_append_char(buf, pos, max, tmp[--tpos]);
    }
}

static void buf_append_2digits(char *buf, size_t *pos, size_t max, uint64_t val) {
    buf_append_char(buf, pos, max, (char)('0' + ((val / 10) % 10)));
    buf_append_char(buf, pos, max, (char)('0' + (val % 10)));
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static bool is_tsc_invariant(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000007) {
        cpuid(0x80000007, 0, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 8)) {
            return true;
        }
    }
    return false;
}

static void format_ram_val(char *buf, size_t *pos, size_t max, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        uint64_t gib_int = bytes / (1024ULL * 1024ULL * 1024ULL);
        uint64_t rem = bytes % (1024ULL * 1024ULL * 1024ULL);
        uint64_t gib_tenth = (rem * 10ULL + (512ULL * 1024ULL * 1024ULL)) / (1024ULL * 1024ULL * 1024ULL);
        if (gib_tenth >= 10) {
            gib_int++;
            gib_tenth = 0;
        }
        buf_append_u64(buf, pos, max, gib_int);
        if (gib_tenth > 0) {
            buf_append_char(buf, pos, max, '.');
            buf_append_u64(buf, pos, max, gib_tenth);
        }
        buf_append_str(buf, pos, max, " GiB");
    } else if (bytes >= 1024ULL * 1024ULL) {
        uint64_t mib = bytes / (1024ULL * 1024ULL);
        buf_append_u64(buf, pos, max, mib);
        buf_append_str(buf, pos, max, " MiB");
    } else if (bytes >= 1024ULL) {
        uint64_t kib = bytes / 1024ULL;
        buf_append_u64(buf, pos, max, kib);
        buf_append_str(buf, pos, max, " KiB");
    } else {
        buf_append_u64(buf, pos, max, bytes);
        buf_append_str(buf, pos, max, " B");
    }
}

static void format_heap_val(char *buf, size_t *pos, size_t max, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL) {
        uint64_t mib = bytes / (1024ULL * 1024ULL);
        buf_append_u64(buf, pos, max, mib);
        buf_append_str(buf, pos, max, " MiB");
    } else if (bytes >= 1024ULL) {
        uint64_t kib = bytes / 1024ULL;
        buf_append_u64(buf, pos, max, kib);
        buf_append_str(buf, pos, max, " KiB");
    } else {
        buf_append_u64(buf, pos, max, bytes);
        buf_append_str(buf, pos, max, " B");
    }
}

static bool str_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static size_t str_len(const char *s, size_t max) {
    size_t len = 0;
    while (len < max && s[len] != '\0') {
        len++;
    }
    return len;
}

static int format_memory_report(void) {
    long ret = sys_meminfo(&s_mem, sizeof(s_mem));
    if (ret < 0) {
        static const char err_msg[] = "sysinfo: error reading memory information\n";
        write_all(2, err_msg, sizeof(err_msg) - 1);
        return 1;
    }

    size_t pos = 0;

    buf_append_str(s_buf, &pos, sizeof(s_buf), "FortressOS Memory Subsystem Observability\n\n");

    /* Section 1: Physical Memory (PMM) */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "Physical Memory (PMM):\n");
    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Total managed:      ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.pmm_total_frames);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " frames (");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_mem.pmm_total_frames * 4096ULL);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ")\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Used / allocated:   ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.pmm_used_frames);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " frames (");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_mem.pmm_used_frames * 4096ULL);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ")\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Free / available:   ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.pmm_free_frames);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " frames (");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_mem.pmm_free_frames * 4096ULL);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ")\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Allocatable (<1G):  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.pmm_allocatable_frames);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " frames (");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_mem.pmm_allocatable_frames * 4096ULL);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ")\n\n");

    /* Section 2: Kernel Dynamic Heap */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "Kernel Dynamic Heap:\n");
    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Live used:          ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_used_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " B (including 32B block metadata)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Reusable free:      ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_free_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " B (within committed capacity)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Committed backing:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_committed_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " B (");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_committed_bytes / 4096ULL);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " physical frames)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Largest free chunk: ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_largest_payload);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " B payload (excludes block tags)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Free blocks count:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.heap_free_blocks);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "\n\n");

    /* Section 3: Virtual Memory Management (VMM) */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "Virtual Memory Management (VMM):\n");
    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Page-table frames:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.vmm_table_frames);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " frames (kernel + user hierarchy)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Deferred teardown:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), s_mem.vmm_deferred_spaces);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " queued address spaces\n\n");

    /* Section 4: Snapshot Notice */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "Snapshot Notice:\n");
    buf_append_str(s_buf, &pos, sizeof(s_buf), "  Counters are individually coherent; cross-subsystem values\n");
    buf_append_str(s_buf, &pos, sizeof(s_buf), "  are observed sequentially without global lock nesting.\n");

    if (pos >= sizeof(s_buf)) {
        pos = sizeof(s_buf) - 1;
    }

    if (write_all(1, s_buf, pos) != 0) {
        return 1;
    }
    return 0;
}

int sysinfo_main(int argc, char **argv) {
    bool mem_only = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (str_equal(arg, "-m") || str_equal(arg, "--memory")) {
            mem_only = true;
        } else if (str_equal(arg, "-h") || str_equal(arg, "--help")) {
            static const char help_msg[] =
                "Usage: sysinfo [-m|--memory] [-h|--help]\n"
                "  (no flags)    Display system identity, CPU, tasks, storage and memory overview\n"
                "  -m, --memory  Display detailed memory subsystem observability (PMM, Heap, VMM)\n";
            write_all(1, help_msg, sizeof(help_msg) - 1);
            return 0;
        } else {
            static const char err_prefix[] = "sysinfo: unknown option: ";
            static const char err_suffix[] = "\nUsage: sysinfo [-m|--memory] [-h|--help]\n";
            write_all(2, err_prefix, sizeof(err_prefix) - 1);
            write_all(2, arg, str_len(arg, 64));
            write_all(2, err_suffix, sizeof(err_suffix) - 1);
            return 1;
        }
    }

    if (mem_only) {
        return format_memory_report();
    }

    long ret = sys_sysinfo(&s_info);
    if (ret < 0) {
        static const char err_msg[] = "sysinfo: error reading system information\n";
        write_all(2, err_msg, sizeof(err_msg) - 1);
        return 1;
    }

    uint64_t hz = s_info.tick_hz ? s_info.tick_hz : 100;
    uint64_t total_sec = s_info.uptime_ticks / hz;
    uint64_t hours = total_sec / 3600;
    uint64_t mins = (total_sec % 3600) / 60;
    uint64_t secs = total_sec % 60;

    uint64_t used_bytes = (s_info.total_ram_bytes >= s_info.free_ram_bytes)
                              ? (s_info.total_ram_bytes - s_info.free_ram_bytes)
                              : 0;

    size_t pos = 0;

    /* Section 1: Kernel / OS identity */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "FortressOS 1.0 (x86_64 SMP, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)s_info.cpu_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), (s_info.cpu_count == 1) ? " CPU)\n" : " CPUs)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Build: " BUILD_GIT_HASH " (" BUILD_DATE ")\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Uptime: ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), hours);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "h ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), mins);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "m ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), secs);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "s\n\n");

    /* Section 2: CPU */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "CPUs online:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)s_info.cpu_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " / ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)s_info.cpu_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Kernel:       SMP, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), hz);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " Hz preemption\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "TSC:          ");
    if (s_info.tsc_hz > 0) {
        uint64_t ghz_int = s_info.tsc_hz / 1000000000ULL;
        uint64_t rem = s_info.tsc_hz % 1000000000ULL;
        uint64_t ghz_cents = (rem + 5000000ULL) / 10000000ULL;
        if (ghz_cents >= 100) {
            ghz_int++;
            ghz_cents = 0;
        }
        buf_append_u64(s_buf, &pos, sizeof(s_buf), ghz_int);
        buf_append_char(s_buf, &pos, sizeof(s_buf), '.');
        buf_append_2digits(s_buf, &pos, sizeof(s_buf), ghz_cents);
        buf_append_str(s_buf, &pos, sizeof(s_buf), " GHz (");
        buf_append_str(s_buf, &pos, sizeof(s_buf), is_tsc_invariant() ? "invariant" : "standard");
        buf_append_str(s_buf, &pos, sizeof(s_buf), ")\n\n");
    } else {
        buf_append_str(s_buf, &pos, sizeof(s_buf), "unavailable\n\n");
    }

    /* Section 3: Memory */
    buf_append_str(s_buf, &pos, sizeof(s_buf), "RAM:  total ");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_info.total_ram_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ", used ");
    format_ram_val(s_buf, &pos, sizeof(s_buf), used_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), ", free ");
    format_ram_val(s_buf, &pos, sizeof(s_buf), s_info.free_ram_bytes);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Heap: ");
    format_heap_val(s_buf, &pos, sizeof(s_buf), s_info.kernel_heap_used);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " / ");
    format_heap_val(s_buf, &pos, sizeof(s_buf), s_info.kernel_heap_total);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " committed (max 512 MiB)\n\n");

    /* Section 4: Tasks */
    uint32_t running = 0;
    uint32_t sleeping = 0;
    uint32_t zombie = 0;
    for (uint64_t i = 0; i < PROC_INFO_MAX; i++) {
        long ret_proc = sys_procinfo(i, &s_proc);
        if (ret_proc == 0) break;
        if (ret_proc < 0) continue;
        if (s_proc.state == PROC_STATE_RUNNING) {
            running++;
        } else if (s_proc.state == PROC_STATE_ZOMBIE) {
            zombie++;
        } else {
            sleeping++;
        }
    }
    uint32_t total_procs = running + sleeping + zombie;
    if (total_procs == 0 && s_info.task_count > 0) {
        total_procs = s_info.task_count;
    }
    uint32_t threads = s_info.thread_count ? s_info.thread_count : total_procs;

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Processes:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)total_procs);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " (");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)running);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " running, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)sleeping);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " sleeping, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)zombie);
    buf_append_str(s_buf, &pos, sizeof(s_buf), (zombie == 1) ? " zombie)\n" : " zombies)\n");

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Threads:    ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)threads);
    buf_append_str(s_buf, &pos, sizeof(s_buf), "\n\n");

    /* Section 5: Storage summary */
    uint32_t dev_count = 0;
    uint32_t part_count = 0;
    for (uint32_t i = 0; i < 32; i++) {
        long ret_blk = sys_blockinfo(i, &s_block);
        if (ret_blk == 0) break;
        if (ret_blk < 0) continue;
        if (str_equal(s_block.name, "initramfs")) continue;
        size_t len = str_len(s_block.name, sizeof(s_block.name));
        if (len >= 2 && s_block.name[len - 1] >= '0' && s_block.name[len - 1] <= '9' && s_block.name[len - 2] == 'p') {
            part_count++;
        } else {
            dev_count++;
        }
    }
    uint32_t mount_count = 0;
    for (uint32_t i = 0; i < 32; i++) {
        long ret_mnt = sys_mountinfo(i, &s_mount);
        if (ret_mnt == 1) {
            mount_count++;
        } else if (ret_mnt == 0) {
            break;
        }
    }

    buf_append_str(s_buf, &pos, sizeof(s_buf), "Storage:  ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)dev_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), (dev_count == 1) ? " device, " : " devices, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)part_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), (part_count == 1) ? " partition, " : " partitions, ");
    buf_append_u64(s_buf, &pos, sizeof(s_buf), (uint64_t)mount_count);
    buf_append_str(s_buf, &pos, sizeof(s_buf), " mounted\n");

    if (pos >= sizeof(s_buf)) {
        pos = sizeof(s_buf) - 1;
    }

    if (write_all(1, s_buf, pos) != 0) {
        return 1;
    }

    return 0;
}
