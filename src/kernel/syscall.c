#include "syscall.h"
#include "input.h"
#include "vmm.h"
#include "serial.h"
#include "gdt.h"
#include "thread.h"
#include "msr.h"
#include "vfs.h"
#include "string.h"
#include "pmm.h"
#include "power.h"
#include "keyboard.h"
#include "ext2.h"

extern void syscall_entry_stub(void);

static volatile bool      g_user_exit_called     = false;
static volatile uint64_t  g_user_exit_code       = 0;
static volatile uintptr_t g_syscall_recovery_rip = 0;
static volatile uintptr_t g_syscall_recovery_rsp = 0;

void syscall_init(void) {
    g_user_exit_called     = false;
    g_user_exit_code       = 0;
    g_syscall_recovery_rip = 0;
    g_syscall_recovery_rsp = 0;
    syscall_init_msrs();
}

void syscall_init_msrs(void) {
    /* 1. Enable SCE (System Call Extensions) in IA32_EFER */
    uint64_t efer = rdmsr(IA32_EFER_MSR);
    efer |= EFER_SCE;
    wrmsr(IA32_EFER_MSR, efer);

    /* 2. Configure IA32_STAR:
     *    Bits [47:32]: Kernel CS / SS for 'syscall'
     *      CS = 0x08 (GDT_KERNEL_CODE)
     *      SS = 0x08 + 8 = 0x10 (GDT_KERNEL_DATA)
     *    Bits [63:48]: User CS / SS for 'sysret'
     *      Target 64-bit SS = (0x10 + 8)  | 3 = 0x1B (GDT_USER_DATA)
     *      Target 64-bit CS = (0x10 + 16) | 3 = 0x23 (GDT_USER_CODE)
     */
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(IA32_STAR_MSR, star);

    /* 3. Configure IA32_LSTAR with target 64-bit syscall entry stub */
    wrmsr(IA32_LSTAR_MSR, (uint64_t)syscall_entry_stub);

    /* 4. Configure IA32_SFMASK:
     *    Masks IF (bit 9), TF (bit 8), DF (bit 10), and arithmetic flags upon 'syscall'
     */
    wrmsr(IA32_SFMASK_MSR, SYSCALL_SFMASK_DEFAULT);
}

bool syscall_verify_msrs(void) {
    uint64_t efer = rdmsr(IA32_EFER_MSR);
    if (!(efer & EFER_SCE)) {
        serial_puts("       [DEBUG] EFER.SCE not set: ");
        serial_print_hex(efer);
        serial_puts("\n");
        return false;
    }

    uint64_t expected_star = ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    uint64_t star = rdmsr(IA32_STAR_MSR);
    if (star != expected_star) {
        serial_puts("       [DEBUG] STAR mismatch! Got: ");
        serial_print_hex(star);
        serial_puts(", Expected: ");
        serial_print_hex(expected_star);
        serial_puts("\n");
        return false;
    }

    uint64_t lstar = rdmsr(IA32_LSTAR_MSR);
    if (lstar != (uint64_t)syscall_entry_stub) {
        serial_puts("       [DEBUG] LSTAR mismatch! Got: ");
        serial_print_hex(lstar);
        serial_puts(", Expected: ");
        serial_print_hex((uint64_t)syscall_entry_stub);
        serial_puts("\n");
        return false;
    }

    uint64_t sfmask = rdmsr(IA32_SFMASK_MSR);
    if (!(sfmask & RFLAGS_IF) || !(sfmask & RFLAGS_DF)) {
        serial_puts("       [DEBUG] SFMASK mismatch! Got: ");
        serial_print_hex(sfmask);
        serial_puts("\n");
        return false;
    }

    return true;
}

void syscall_set_recovery(uintptr_t rip, uintptr_t rsp) {
    g_user_exit_called     = false;
    g_user_exit_code       = 0;
    g_syscall_recovery_rip = rip;
    g_syscall_recovery_rsp = rsp;
}

void syscall_clear_recovery(void) {
    g_syscall_recovery_rip = 0;
    g_syscall_recovery_rsp = 0;
}

bool syscall_was_exit_called(uint64_t *out_exit_code) {
    if (!g_user_exit_called) return false;
    if (out_exit_code) *out_exit_code = g_user_exit_code;
    return true;
}

static int64_t syscall_from_vfs_error(int64_t vfs_err) {
    switch (vfs_err) {
        case 0:                return SYSCALL_SUCCESS;
        case -VFS_ENOENT:      return SYSCALL_ENOENT;      /* -5 */
        case -VFS_EIO:         return SYSCALL_EIO;         /* -9 */
        case -VFS_EBADF:       return SYSCALL_EBADF;       /* -3 */
        case -VFS_ENOMEM:      return SYSCALL_ENOMEM;      /* -10 */
        case -VFS_EEXIST:      return SYSCALL_EEXIST;      /* -15 */
        case -VFS_EINVAL:      return SYSCALL_EINVAL;      /* -1 */
        case -VFS_EFBIG:       return SYSCALL_EFBIG;       /* -12 */
        case -VFS_ENOSPC:      return SYSCALL_ENOSPC;      /* -13 */
        case -VFS_EROFS:       return SYSCALL_EROFS;       /* -11 */
        case -VFS_EOPNOTSUPP:  return SYSCALL_EOPNOTSUPP;  /* -14 */
        case -7:               return SYSCALL_EISDIR;      /* -7 */
        case -8:               return SYSCALL_ENOTDIR;     /* -8 */
        default:               return SYSCALL_EINVAL;      /* -1 */
    }
}

static int64_t sys_write(uint64_t fd, uintptr_t user_buf, size_t count) {
    if (fd == 0 || fd >= 32) {
        return SYSCALL_EBADF;
    }

    /* 2. Zero-length writes succeed immediately as no-op */
    if (count == 0) {
        return 0;
    }

    /* 3. Bound check: reject oversized buffers */
    if (count > MAX_SYSCALL_WRITE_LEN) {
        return SYSCALL_EINVAL;
    }

    /* 4. Validate user memory range against active address space */
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_buf, count, false)) {
        return SYSCALL_EFAULT;
    }

    /* 5. Standard output (1) or standard error (2) -> serial / console */
    if (fd == 1 || fd == 2) {
        const char *ptr = (const char *)user_buf;
        for (size_t i = 0; i < count; i++) {
            serial_putc(ptr[i]);
        }
        return (int64_t)count;
    }

    /* 6. Regular file descriptor (fd >= 3) */
    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    file_t *file = fd_get(curr, (int)fd);
    if (!file) {
        return SYSCALL_EBADF;
    }

    int64_t res = vfs_write(file, (const void *)user_buf, count);
    if (res < 0) {
        return syscall_from_vfs_error(res);
    }
    return res;
}

static int64_t sys_exit(uint64_t exit_code, interrupt_frame_t *frame) {
    g_user_exit_called = true;
    g_user_exit_code   = exit_code;

    if (g_syscall_recovery_rip != 0) {
        /* Legacy test harness redirect back to test function in Ring 0 */
        frame->rip    = g_syscall_recovery_rip;
        frame->cs     = GDT_KERNEL_CODE;
        frame->ss     = GDT_KERNEL_DATA;
        frame->rsp    = g_syscall_recovery_rsp;
        frame->rflags = 0x002; /* Clean kernel RFLAGS with IF=0 */
        return 0;
    }

    /* General scheduled process exit: terminate and switch to next runnable context */
    tcb_t *curr = thread_current();
    if (curr && curr->is_user) {
        process_exit(exit_code);
        /* Never reached */
    }

    return 0;
}

/*
 * Canonical lower-half user address limits:
 * Range [PAGE_SIZE, USER_SPACE_TOP)
 * User space addresses must be >= 0x1000 (guarding NULL / page 0)
 * and strictly < 0x0000800000000000ULL (canonical lower-half limit).
 */
#define USER_CANONICAL_MIN   0x1000ULL
#define USER_CANONICAL_LIMIT 0x0000800000000000ULL

/*
 * Allowed user RFLAGS mask:
 * User code can manipulate standard arithmetic & status flags:
 *   CF (0x1), PF (0x4), AF (0x10), ZF (0x40), SF (0x80), DF (0x400), OF (0x800)
 *   AC (0x40000), ID (0x200000)
 * Prohibited / strictly sanitized:
 *   IF: Forced to 1 (0x200) so user mode is always interruptible
 *   Bit 1: Reserved, must be 1 (0x2)
 *   IOPL: Stripped to 0 (bits 12-13) - user mode has no port I/O access
 *   NT: Stripped to 0 (bit 14)
 *   TF: Stripped to 0 (bit 8)
 *   VM: Stripped to 0 (bit 17)
 */
#define USER_RFLAGS_ALLOWED_MASK (0x0000000000240CD5ULL)
#define USER_RFLAGS_FORCED       (0x0000000000000202ULL)

bool syscall_validate_return_state(interrupt_frame_t *frame) {
    if (!frame) return false;

    /* If frame has been redirected to kernel (e.g. test harness recovery), skip user checks */
    if (frame->cs != GDT_USER_CODE) {
        return true;
    }

    /* 1. Validate Return RIP against canonical lower-half user address space */
    if (frame->rip < USER_CANONICAL_MIN || frame->rip >= USER_CANONICAL_LIMIT) {
        serial_puts("[SYSCALL] Hardening violation: non-canonical or kernel return RIP: ");
        serial_print_hex(frame->rip);
        serial_puts("\n");
        return false;
    }

    /* 2. Validate Return RSP against canonical lower-half user address space [PAGE_SIZE, 0x0000800000000000ULL) */
    if (frame->rsp < USER_CANONICAL_MIN || frame->rsp >= USER_CANONICAL_LIMIT) {
        serial_puts("[SYSCALL] Hardening violation: non-canonical or kernel return RSP: ");
        serial_print_hex(frame->rsp);
        serial_puts("\n");
        return false;
    }

    /* 3. Sanitize RFLAGS: enforce user mask, force IF=1 and bit 1=1, clear IOPL/NT/TF/VM */
    frame->rflags = (frame->rflags & USER_RFLAGS_ALLOWED_MASK) | USER_RFLAGS_FORCED;

    return true;
}

static int copy_user_string(uint64_t *pml4, uintptr_t user_ptr, char *dest, size_t max_len) {
    if (!pml4 || !dest || max_len == 0) return SYSCALL_EINVAL;
    if (user_ptr < USER_CANONICAL_MIN || user_ptr >= USER_CANONICAL_LIMIT) {
        return SYSCALL_EFAULT;
    }

    if (!vmm_validate_user_range(pml4, user_ptr, 1, false)) {
        return SYSCALL_EFAULT;
    }

    const char *src = (const char *)user_ptr;
    for (size_t i = 0; i < max_len; i++) {
        uintptr_t curr_addr = user_ptr + i;
        if (curr_addr >= USER_CANONICAL_LIMIT) {
            return SYSCALL_EFAULT;
        }
        if ((curr_addr % PAGE_SIZE) == 0) {
            if (!vmm_validate_user_range(pml4, curr_addr, 1, false)) {
                return SYSCALL_EFAULT;
            }
        }
        dest[i] = src[i];
        if (src[i] == '\0') {
            return SYSCALL_SUCCESS;
        }
    }

    dest[max_len - 1] = '\0';
    return SYSCALL_EINVAL;
}

static int64_t sys_open(uintptr_t user_path, int flags) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char kpath[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    /* Check whether an fd is available before any destructive open/truncation */
    bool fd_avail = false;
    for (int i = 3; i < 32; i++) {
        if (!curr->fd_table[i]) {
            fd_avail = true;
            break;
        }
    }
    if (!fd_avail) {
        return SYSCALL_EMFILE;
    }

    int vfs_err = 0;
    file_t *file = vfs_open_ext(kpath, flags, &vfs_err);
    if (!file) {
        return syscall_from_vfs_error(vfs_err);
    }

    int fd = fd_alloc(curr, file);
    if (fd < 0) {
        vfs_close(file);
        return SYSCALL_EMFILE;
    }

    return (int64_t)fd;
}

static int64_t sys_close(int fd) {
    if (fd < 3 || fd >= 32) {
        return SYSCALL_EBADF;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    int res = fd_free(curr, fd);
    if (res < 0) {
        return SYSCALL_EBADF;
    }

    return SYSCALL_SUCCESS;
}

static int64_t sys_read(int fd, uintptr_t user_buf, size_t count) {
    if (count == 0) {
        return 0;
    }

    if (count > MAX_SYSCALL_WRITE_LEN) {
        count = MAX_SYSCALL_WRITE_LEN;
    }

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    /* CRITICAL: Must verify PTE_WRITABLE since kernel writes into user buffer! */
    if (!vmm_validate_user_range(active_pml4, user_buf, count, true)) {
        return SYSCALL_EFAULT;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    if (fd == 0) return input_read((void *)user_buf, count);

    file_t *file = fd_get(curr, fd);
    if (!file) {
        return SYSCALL_EBADF;
    }

    int64_t res = vfs_read(file, (void *)user_buf, count);
    if (res < 0) {
        return syscall_from_vfs_error(res);
    }
    return res;
}

static int64_t sys_stat(uintptr_t user_path, uintptr_t user_statbuf) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char kpath[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    if (!vmm_validate_user_range(active_pml4, user_statbuf, sizeof(vfs_stat_t), true)) {
        return SYSCALL_EFAULT;
    }

    vfs_node_t *node = vfs_lookup(kpath);
    if (!node) {
        return SYSCALL_ENOENT;
    }

    vfs_stat_t st;
    vfs_stat(node, &st);
    memcpy((void *)user_statbuf, &st, sizeof(vfs_stat_t));
    return SYSCALL_SUCCESS;
}

static int64_t sys_readdir(int fd, uintptr_t user_dirent) {
    if (fd < 0 || fd >= 32) {
        return SYSCALL_EBADF;
    }

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_dirent, sizeof(vfs_dirent_t), true)) {
        return SYSCALL_EFAULT;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    file_t *file = fd_get(curr, fd);
    if (!file) {
        return SYSCALL_EBADF;
    }

    if (file->node->type != VFS_DIRECTORY) {
        return SYSCALL_ENOTDIR;
    }

    vfs_dirent_t dent;
    int res = vfs_readdir(file->node, file->offset, &dent);
    if (res == 1) {
        file->offset++;
        memcpy((void *)user_dirent, &dent, sizeof(vfs_dirent_t));
        return 1;
    }
    if (res == 0) {
        return 0; /* EOF */
    }
    return SYSCALL_EINVAL;
}

static int64_t sys_reboot(uint64_t cmd) {
    if (cmd == REBOOT_CMD_RESTART) {
        if (!ext2_sync_all()) return SYSCALL_EIO;
        power_reboot();
    } else if (cmd == REBOOT_CMD_POWEROFF) {
        if (!ext2_sync_all()) return SYSCALL_EIO;
        power_shutdown();
    }
    return SYSCALL_EINVAL;
}

static int64_t sys_kbd_layout(int64_t layout) {
    if (layout == KBD_LAYOUT_US) {
        keyboard_set_layout(KBD_LAYOUT_US);
        return 0;
    } else if (layout == KBD_LAYOUT_AZERTY) {
        keyboard_set_layout(KBD_LAYOUT_AZERTY);
        return 1;
    } else if (layout < 0) {
        return (int64_t)keyboard_get_layout();
    }
    return SYSCALL_EINVAL;
}

int64_t syscall_dispatch(interrupt_frame_t *frame) {
    if (!frame) return SYSCALL_EINVAL;

    uint64_t syscall_nr = frame->rax;
    int64_t result = SYSCALL_ENOSYS;

    switch (syscall_nr) {
        case SYS_EXIT:
            result = sys_exit(frame->rdi, frame);
            break;

        case SYS_WRITE:
            result = sys_write(frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_OPEN:
            result = sys_open(frame->rdi, (int)frame->rsi);
            break;

        case SYS_CLOSE:
            result = sys_close((int)frame->rdi);
            break;

        case SYS_READ:
            result = sys_read((int)frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_STAT:
            result = sys_stat(frame->rdi, frame->rsi);
            break;

        case SYS_READDIR:
            result = sys_readdir((int)frame->rdi, frame->rsi);
            break;

        case SYS_REBOOT:
            result = sys_reboot(frame->rdi);
            break;

        case SYS_KBD_LAYOUT:
            result = sys_kbd_layout((int64_t)frame->rdi);
            break;

        default:
            result = SYSCALL_ENOSYS;
            break;
    }

    frame->rax = (uint64_t)result;

    /* Validate and sanitize return state before returning to assembly stub */
    if (!syscall_validate_return_state(frame)) {
        if (g_syscall_recovery_rip != 0) {
            /* Test harness recovery mode: redirect frame to kernel recovery handler */
            frame->rip    = g_syscall_recovery_rip;
            frame->cs     = GDT_KERNEL_CODE;
            frame->ss     = GDT_KERNEL_DATA;
            frame->rsp    = g_syscall_recovery_rsp;
            frame->rflags = 0x002;
            frame->rax    = (uint64_t)SYSCALL_EFAULT;
            return SYSCALL_EFAULT;
        }

        /* Rogue user process attempting invalid return state: terminate on kernel stack */
        tcb_t *curr = thread_current();
        if (curr && curr->is_user) {
            serial_puts("[SYSCALL] Terminating rogue process due to invalid return state (exit 141)\n");
            process_exit(141); /* 128 + 13 (#GP) */
            /* Never reached */
        }
    }

    return result;
}
