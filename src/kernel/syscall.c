#include "poll.h"
#include "boot_info.h"
#include "fb.h"
#include "shm.h"
#include "pkg_service.h"
#include "syscall.h"
#include "permissions.h"
static bool syscall_actor(creds_t *out);
#include "../net/net.h"
#include "../net/net_ping.h"
#include "../net/net_control.h"
#include "../net/net_wifi_owner.h"
#include "../net/net_socket_syscall.h"
#include "../net/net_socket.h"
#include "../net/net_tcp_syscall.h"
#include "../net/wlan/wlan_mgr.h"
#include "wifi_abi.h"
#include "../drivers/iwlwifi.h"
#include "percpu.h"
#include "process_table.h"
#include "input.h"
#include "vmm.h"
#include "serial.h"
#include "gdt.h"
#include "thread.h"
#include "msr.h"
#include "vfs.h"
#include "pipe.h"
#include "string.h"
#include "pmm.h"
#include "power.h"
#include "keyboard.h"
#include "ext2.h"
#include "ext4.h"
#include "tarfs.h"
#include "block.h"
#include "../fs/gpt.h"
#include "heap.h"
#include "elf.h"
#include "usb_mount.h"
#include "installed_root.h"
#include "accounts.h"
#include "hostkey.h"
#include "random.h"
#include "install_control.h"
#include "xhci.h"
#include "dmesg.h"
#include "terminal.h"
#include "pty.h"
#include "console.h"
#include "smp.h"
#include "apic.h"
#include "time.h"

extern void syscall_entry_stub(void);

static bool nanosleep_ready(void *arg) {
    uint64_t deadline = *(const uint64_t *)arg;
    return apic_timer_get_bsp_ticks() >= deadline;
}

static volatile bool      g_user_exit_called     = false;
static volatile uint64_t  g_user_exit_code       = 0;
static volatile uintptr_t g_syscall_recovery_rip = 0;
static volatile uintptr_t g_syscall_recovery_rsp = 0;

void syscall_init(void) {
    __atomic_store_n(&g_user_exit_called,false,__ATOMIC_RELEASE);
    __atomic_store_n(&g_user_exit_code,0,__ATOMIC_RELAXED);
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
    __atomic_store_n(&g_user_exit_called,false,__ATOMIC_RELEASE);
    __atomic_store_n(&g_user_exit_code,0,__ATOMIC_RELAXED);
    g_syscall_recovery_rip = rip;
    g_syscall_recovery_rsp = rsp;
}

void syscall_clear_recovery(void) {
    g_syscall_recovery_rip = 0;
    g_syscall_recovery_rsp = 0;
}

bool syscall_was_exit_called(uint64_t *out_exit_code) {
    if (!__atomic_load_n(&g_user_exit_called,__ATOMIC_ACQUIRE)) return false;
    if (out_exit_code) *out_exit_code = __atomic_load_n(&g_user_exit_code,__ATOMIC_RELAXED);
    return true;
}

int64_t syscall_from_vfs_error(int64_t vfs_err) {
    switch (vfs_err) {
        case 0:                return SYSCALL_SUCCESS;
        case -VFS_EPERM:       return SYSCALL_EPERM;
        case -VFS_EACCES:      return SYSCALL_EACCES;
        case -VFS_ENOENT:      return SYSCALL_ENOENT;      /* -5 */
        case -VFS_EIO:         return SYSCALL_EIO;         /* -9 */
        case -VFS_EBADF:       return SYSCALL_EBADF;       /* -3 */
        case -VFS_EINTR:       return SYSCALL_EINTR;
        case -VFS_EPIPE:       return SYSCALL_EPIPE;
        case -VFS_EAGAIN:      return SYSCALL_EAGAIN;
        case -VFS_ENOMEM:      return SYSCALL_ENOMEM;      /* -10 */
        case -VFS_EEXIST:      return SYSCALL_EEXIST;      /* -15 */
        case -VFS_EINVAL:      return SYSCALL_EINVAL;      /* -1 */
        case -VFS_EFBIG:       return SYSCALL_EFBIG;       /* -12 */
        case -VFS_ENOSPC:      return SYSCALL_ENOSPC;      /* -13 */
        case -VFS_EROFS:       return SYSCALL_EROFS;       /* -11 */
        case -VFS_ENOTEMPTY:   return SYSCALL_ENOTEMPTY;   /* -19 */
        case -VFS_ELOOP:       return SYSCALL_ELOOP;       /* -33 */
        case -VFS_EOPNOTSUPP:  return SYSCALL_EOPNOTSUPP;  /* -14 */
        case -7:               return SYSCALL_EISDIR;      /* -7 */
        case -VFS_ENOTDIR:    return SYSCALL_ENOTDIR;
        case -8:               return SYSCALL_ENOTDIR;     /* -8 */
        default:               return SYSCALL_EINVAL;      /* -1 */
    }
}

static int64_t sys_write(uint64_t fd, uintptr_t user_buf, size_t count) {
    if (fd >= MAX_PROCESS_FDS) {
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

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    file_t *file = fd_get(curr, (int)fd);
    if (!file) {
        return SYSCALL_EBADF;
    }

    int64_t res;
    if (file->node && file->node->write_actor) {
        creds_t actor;if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
        res=vfs_write_creds(file,(const void *)user_buf,count,&actor);
    } else res = vfs_write(file, (const void *)user_buf, count);
    if (res == -VFS_EPIPE) {
        /* The pipe callback has released its rank-2 lock. Publish only to the
         * writer (selector 0 would signal its entire group). Delivery belongs
         * to the existing user-return boundary, after frame->rax holds EPIPE.
         * Positive short writes remain positive; their next write may fail. */
        spin_debug_assert_unheld();
        (void)process_signal_send_kernel(curr->tid, (int64_t)curr->tid, SIGPIPE);
    }
    if (res < 0) {
        return syscall_from_vfs_error(res);
    }
    return res;
}

static int64_t sys_termctl(uint64_t op, uintptr_t ptr, size_t size) {
    tcb_t *t = thread_current();
    if (!t) return SYSCALL_EINVAL;
    if (op == TERM_ISATTY) {
        int fd = (int)ptr;
        if (fd < 0 || fd >= MAX_PROCESS_FDS) return SYSCALL_EBADF;
        file_t *file = fd_get(t, fd);
        if (!file) return SYSCALL_EBADF;
        return (file->node == vfs_get_terminal_node() || is_pty_slave(file->node) || is_pty_master(file->node)) ? 1 : 0;
    }
    if (size != sizeof(terminal_info_t) || op > TERM_SET) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, true)) return SYSCALL_EFAULT;
    terminal_info_t *info = (terminal_info_t *)ptr;
    if (op == TERM_SET) {
        if (info->version != 1 || info->mode > TERM_PLAIN ||
            (info->cols && (info->cols < 20 || info->cols > 512))) return SYSCALL_EINVAL;
        if (info->mode == TERM_LOCAL && !console_is_initialized()) return SYSCALL_EOPNOTSUPP;
        if (info->mode == TERM_SERIAL && !serial_is_available()) return SYSCALL_EOPNOTSUPP;
        int error=input_control_check();
        if (error) return error;
        t->terminal_mode = info->mode;
        t->terminal_cols = info->cols;
    }
    /* Match /dev/tty's PTY selection. Console redraw/drop counters belong to
     * the physical endpoint: exposing them here makes local typing redraw
     * every SSH prompt. PTY input is backpressured, not dropped, and currently
     * has no asynchronous display-generation counter. */
    for (int fd = 0; fd < MAX_PROCESS_FDS; fd++) {
        file_t *file = fd_get(t, fd);
        if (!file || !file->node || !is_pty_slave(file->node)) continue;
        uint16_t pty_cols, pty_rows;
        int64_t rc = pty_get_winsize(file->node, &pty_cols, &pty_rows);
        if (rc != 0) return rc;
        *info = (terminal_info_t){1, t->terminal_mode, pty_cols, pty_rows, 0, 0};
        return 0;
    }
    uint64_t cols = 80, rows = 25;
    if (console_is_initialized()) console_get_dimensions(&cols, &rows);
    if (t->terminal_mode != TERM_LOCAL && serial_is_available()) {
        uint64_t serial_cols = t->terminal_cols ? t->terminal_cols : 80;
        if (t->terminal_mode == TERM_SERIAL || serial_cols < cols) cols = serial_cols;
    }
    *info = (terminal_info_t){1, t->terminal_mode, (uint32_t)cols, (uint32_t)rows,
                             input_dropped(), console_generation()};
    return 0;
}

static int64_t sys_input_read(uintptr_t ptr, size_t count, int64_t timeout) {
    if (count > MAX_SYSCALL_WRITE_LEN || timeout < -1 || timeout > 1000) return SYSCALL_EINVAL;
    if (!count) return 0;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, count, true)) return SYSCALL_EFAULT;

    tcb_t *curr = thread_current();
    if (curr) {
        file_t *f0 = fd_get(curr, 0);
        if (f0 && f0->node && is_pty_slave(f0->node)) {
            return pty_slave_input_read(f0->node, (void *)ptr, count, timeout);
        }
    }

    return input_read_timeout((void *)ptr, count, timeout);
}

static int64_t sys_termattr(uint64_t fd, uint64_t op, uintptr_t ptr, size_t size) {
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;
    if (fd >= MAX_PROCESS_FDS) return SYSCALL_EBADF;
    file_t *file = fd_get(curr, (int)fd);
    if (!file) return SYSCALL_EBADF;

    if (file->node && (is_pty_slave(file->node) || is_pty_master(file->node))) {
        if (op == TERM_SET_WINSIZE) {
            if (size != sizeof(winsize_t)) return SYSCALL_EINVAL;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, false)) return SYSCALL_EFAULT;
            winsize_t ws;
            memcpy(&ws, (const void *)ptr, sizeof(ws));
            return pty_set_winsize(file->node, ws.ws_col, ws.ws_row);
        }
        if (op == TERM_GET_WINSIZE) {
            if (size != sizeof(winsize_t)) return SYSCALL_EINVAL;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, true)) return SYSCALL_EFAULT;
            winsize_t ws = {0};
            int64_t r = pty_get_winsize(file->node, &ws.ws_col, &ws.ws_row);
            if (!r) memcpy((void *)ptr, &ws, sizeof(ws));
            return r;
        }
        if (op == PTY_SET_SESSION) {
            if (!is_pty_master(file->node)) return SYSCALL_EINVAL;
            if (size != sizeof(uint64_t)) return SYSCALL_EINVAL;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, false)) return SYSCALL_EFAULT;
            uint64_t child_pid = *(const uint64_t *)ptr;
            return pty_set_session(file->node, child_pid);
        }
        if (op == TERM_GET) {
            if (size != sizeof(terminal_attrs_t)) return SYSCALL_EINVAL;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, true)) return SYSCALL_EFAULT;
            terminal_attrs_t attrs = {0};
            int64_t r = pty_get_termattr(file->node, &attrs);
            if (!r) memcpy((void *)ptr, &attrs, sizeof(attrs));
            return r;
        }
        if (op == TERM_SET) {
            if (size != sizeof(terminal_attrs_t)) return SYSCALL_EINVAL;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, false)) return SYSCALL_EFAULT;
            terminal_attrs_t attrs;
            memcpy(&attrs, (const void *)ptr, sizeof(attrs));
            return pty_set_termattr(file->node, &attrs);
        }
        return SYSCALL_EINVAL;
    }

    if (size!=sizeof(terminal_attrs_t) || op>TERM_SET) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),ptr,size,op==TERM_GET))
        return SYSCALL_EFAULT;
    terminal_attrs_t attrs={0};
    if (op==TERM_SET) memcpy(&attrs,(const void *)ptr,sizeof(attrs));
    int result=input_termattr(fd,op,&attrs);
    if (!result && op==TERM_GET) memcpy((void *)ptr,&attrs,sizeof(attrs));
    return result;
}

static int64_t sys_exit(uint64_t exit_code, interrupt_frame_t *frame) {
    /* Legacy observation remains global, but ordinary AP exits may overlap.
     * Recovery execution itself is still an isolated supervisor test contract. */
    __atomic_store_n(&g_user_exit_code,exit_code,__ATOMIC_RELAXED);
    __atomic_store_n(&g_user_exit_called,true,__ATOMIC_RELEASE);

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

static int resolve_path(tcb_t *proc,const char *in_path,char *out_path,size_t out_cap) {
    int r=vfs_join_path(proc && proc->cwd[0] ? proc->cwd : "/",in_path,out_path,out_cap);
    return syscall_from_vfs_error(r);
}

static int copy_user_string_vector(uint64_t *active_pml4, uintptr_t user_vec,
                                   int max_count, size_t max_strlen, size_t max_total_len,
                                   char *buf, const char *out_kvec[], int *out_count) {
    int count = 0;
    size_t buf_offset = 0;

    for (int i = 0; i < max_count; i++) {
        uintptr_t ptr_addr = user_vec + (uintptr_t)i * sizeof(uintptr_t);
        if (!vmm_validate_user_range(active_pml4, ptr_addr, sizeof(uintptr_t), false)) {
            return SYSCALL_EFAULT;
        }

        uintptr_t str_ptr = *(const uintptr_t *)ptr_addr;
        if (str_ptr == 0) {
            break;
        }

        if (buf_offset >= max_total_len) {
            return SYSCALL_E2BIG;
        }

        size_t max_copy = max_total_len - buf_offset;
        if (max_copy > max_strlen) max_copy = max_strlen;

        int err = copy_user_string(active_pml4, str_ptr, &buf[buf_offset], max_copy);
        if (err != SYSCALL_SUCCESS) {
            return err == SYSCALL_EINVAL ? SYSCALL_E2BIG : err;
        }

        out_kvec[count++] = &buf[buf_offset];
        buf_offset += strlen(&buf[buf_offset]) + 1;
    }

    if (count == max_count) {
        uintptr_t term_addr = user_vec + (uintptr_t)max_count * sizeof(uintptr_t);
        if (!vmm_validate_user_range(active_pml4, term_addr, sizeof(uintptr_t), false)) {
            return SYSCALL_EFAULT;
        }
        if (*(const uintptr_t *)term_addr != 0) {
            return SYSCALL_E2BIG;
        }
    }

    out_kvec[count] = NULL;
    *out_count = count;
    return SYSCALL_SUCCESS;
}

static int64_t sys_spawn(uintptr_t user_path, uintptr_t user_argv) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err) return err;

    tcb_t *curr = thread_current();
    char path[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, path, sizeof(path));
    if (err) return err;

    if (!user_argv) {
        const char *kargv[2] = { path, NULL };
        int64_t pid;
        int64_t result = process_spawn_from_vfs(path, 1, kargv, &pid);
        return result ? result : pid;
    }

    char *args_buf = (char *)kmalloc(MAX_TOTAL_ARGS_LEN);
    if (!args_buf) return SYSCALL_ENOMEM;

    const char *kargv[MAX_SPAWN_ARGS + 1];
    int argc = 0;

    err = copy_user_string_vector(active_pml4, user_argv,
                                 MAX_SPAWN_ARGS, MAX_ARG_STRLEN, MAX_TOTAL_ARGS_LEN,
                                 args_buf, kargv, &argc);
    if (err != SYSCALL_SUCCESS) {
        kfree(args_buf);
        return err;
    }

    if (argc == 0) {
        kargv[0] = path;
        kargv[1] = NULL;
        argc = 1;
    }

    int64_t pid;
    int64_t result = process_spawn_from_vfs(path, argc, kargv, &pid);
    kfree(args_buf);
    return result ? result : pid;
}

static int64_t sys_spawn_ext(uintptr_t user_path, uintptr_t user_opts_ptr, uint64_t user_opts_size) {
    if (user_opts_size != sizeof(spawn_opts_t) && user_opts_size != sizeof(spawn_opts_v3_t)) {
        return SYSCALL_EINVAL;
    }

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_opts_ptr, user_opts_size, false)) {
        return SYSCALL_EFAULT;
    }

    spawn_opts_t opts;
    memcpy(&opts, (const void *)user_opts_ptr, sizeof(spawn_opts_t));

    spawn_identity_t identity;
    bool explicit_identity=user_opts_size==sizeof(spawn_opts_v3_t);
    if (explicit_identity) memcpy(&identity,(const void *)(user_opts_ptr+sizeof(opts)),sizeof(identity));

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

    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err) return err;

    tcb_t *curr = thread_current();
    char path[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, path, sizeof(path));
    if (err) return err;

    spawn_fd_action_t uactions[MAX_SPAWN_ACTIONS];
    spawn_kaction_t kactions[MAX_SPAWN_ACTIONS];
    char (*action_paths)[VFS_MAX_PATH] = NULL;

    if (opts.action_count > 0) {
        if (!vmm_validate_user_range(active_pml4, (uintptr_t)opts.fd_actions,
                                    opts.action_count * sizeof(spawn_fd_action_t), false)) {
            return SYSCALL_EFAULT;
        }
        memcpy(uactions, (const void *)opts.fd_actions, opts.action_count * sizeof(spawn_fd_action_t));
        action_paths = kmalloc(opts.action_count * VFS_MAX_PATH);
        if (!action_paths) {
            return SYSCALL_ENOMEM;
        }

        for (uint32_t i = 0; i < opts.action_count; i++) {
            kactions[i].type = uactions[i].type;
            kactions[i].dst_fd = uactions[i].dst_fd;
            kactions[i].src_fd = uactions[i].src_fd;
            kactions[i].flags = uactions[i].flags;
            kactions[i].mode = uactions[i].mode;
            kactions[i].path = NULL;

            if (uactions[i].reserved != 0) {
                kfree(action_paths);
                return SYSCALL_EINVAL;
            }
            if (uactions[i].dst_fd < 0 || uactions[i].dst_fd >= MAX_PROCESS_FDS) {
                kfree(action_paths);
                return SYSCALL_EBADF;
            }

            if (uactions[i].type == SPAWN_FD_ACTION_OPEN) {
                if (uactions[i].path == 0) {
                    kfree(action_paths);
                    return SYSCALL_EINVAL;
                }
                char raw_act_path[VFS_MAX_PATH];
                err = copy_user_string(active_pml4, (uintptr_t)uactions[i].path, raw_act_path, sizeof(raw_act_path));
                if (err) {
                    kfree(action_paths);
                    return err;
                }
                err = resolve_path(curr, raw_act_path, action_paths[i], VFS_MAX_PATH);
                if (err) {
                    kfree(action_paths);
                    return err;
                }
                kactions[i].path = action_paths[i];
            } else if (uactions[i].type == SPAWN_FD_ACTION_DUP2) {
                if (uactions[i].src_fd < 0 || uactions[i].src_fd >= MAX_PROCESS_FDS) {
                    kfree(action_paths);
                    return SYSCALL_EBADF;
                }
            } else if (uactions[i].type == SPAWN_FD_ACTION_CLOSE) {
                /* dst_fd validated */
            } else {
                kfree(action_paths);
                return SYSCALL_EINVAL;
            }
        }
    }

    char *args_buf = (char *)kmalloc(MAX_TOTAL_ARGS_LEN);
    if (!args_buf) {
        if (action_paths) kfree(action_paths);
        return SYSCALL_ENOMEM;
    }

    const char *kargv[MAX_SPAWN_ARGS + 1];
    int argc = 0;

    if (opts.argv) {
        err = copy_user_string_vector(active_pml4, (uintptr_t)opts.argv,
                                     MAX_SPAWN_ARGS, MAX_ARG_STRLEN, MAX_TOTAL_ARGS_LEN,
                                     args_buf, kargv, &argc);
        if (err != SYSCALL_SUCCESS) {
            kfree(args_buf);
            if (action_paths) kfree(action_paths);
            return err;
        }
    }

    if (argc == 0) {
        kargv[0] = path;
        kargv[1] = NULL;
        argc = 1;
    }

    char *env_buf = NULL;
    const char *kenvp[MAX_SPAWN_ENVP + 1];
    int envc = 0;

    if (opts.envp) {
        env_buf = (char *)kmalloc(MAX_TOTAL_ENVP_LEN);
        if (!env_buf) {
            kfree(args_buf);
            if (action_paths) kfree(action_paths);
            return SYSCALL_ENOMEM;
        }
        err = copy_user_string_vector(active_pml4, (uintptr_t)opts.envp,
                                     MAX_SPAWN_ENVP, MAX_ENV_STRLEN, MAX_TOTAL_ENVP_LEN,
                                     env_buf, kenvp, &envc);
        if (err != SYSCALL_SUCCESS) {
            kfree(args_buf);
            kfree(env_buf);
            if (action_paths) kfree(action_paths);
            return err;
        }
    }

    char cwd_buf[VFS_MAX_PATH];
    const char *kcwd = NULL;
    if (opts.cwd) {
        err = copy_user_string(active_pml4, (uintptr_t)opts.cwd, cwd_buf, sizeof(cwd_buf));
        if (err) {
            kfree(args_buf);
            if (env_buf) kfree(env_buf);
            if (action_paths) kfree(action_paths);
            return err;
        }
        /* Admit an explicit spawn cwd through the same canonical path rules
         * used by CHDIR; the spawn path checks its directory/search hook. */
        char resolved_cwd[VFS_MAX_PATH];
        err = resolve_path(thread_current(), cwd_buf, resolved_cwd, sizeof(resolved_cwd));
        if (err) {
            kfree(args_buf);
            if (env_buf) kfree(env_buf);
            if (action_paths) kfree(action_paths);
            return err;
        }
        memcpy(cwd_buf, resolved_cwd, strlen(resolved_cwd) + 1);
        kcwd = cwd_buf;
    }

    int64_t pid = 0;
    int64_t result = process_spawn_from_vfs_identity(path, argc, kargv, envc, opts.envp ? kenvp : NULL, kcwd,
                         opts.action_count, opts.action_count ? kactions : NULL, opts.flags, opts.reserved2,
                         explicit_identity ? &identity : NULL, &pid);
    kfree(args_buf);
    if (env_buf) kfree(env_buf);
    if (action_paths) kfree(action_paths);
    return result ? result : pid;
}

static int64_t sys_wait(uint64_t pid, uintptr_t user_status) {
    if (user_status && !vmm_validate_user_range(vmm_get_active_pml4_virt(),
                                              user_status, sizeof(int64_t), true))
        return SYSCALL_EFAULT;
    uint64_t status;
    if (!pid || pid>0x7fffffffffffffffULL) return SYSCALL_ECHILD;
    int64_t waited=process_waitpid((int64_t)pid, &status, 0, true);
    if (waited<0) return waited;
    /* No shared address spaces or user unmap API: validation survives sleep. */
    if (user_status) memcpy((void *)user_status, &status, sizeof(status));
    return SYSCALL_SUCCESS;
}

static int64_t sys_open(uintptr_t user_path, int flags) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    char kpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    /* Check whether an fd is available before any destructive open/truncation */
    bool fd_avail = false;
    struct file **cur_table = curr->process ? curr->process->fd_table : curr->fd_table;
    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        if (!cur_table[i]) {
            fd_avail = true;
            break;
        }
    }
    if (!fd_avail) {
        return SYSCALL_EMFILE;
    }

    if (strcmp(kpath, "/dev/tty") == 0) {
        file_t *f = fd_get(curr, 0);
        if (!f || !f->node || !is_pty_slave(f->node)) f = fd_get(curr, 1);
        if (!f || !f->node || !is_pty_slave(f->node)) f = fd_get(curr, 2);
        if (!f || !f->node || !is_pty_slave(f->node)) {
            for (int i = 3; i < MAX_PROCESS_FDS; i++) {
                file_t *cand = fd_get(curr, i);
                if (cand && cand->node && is_pty_slave(cand->node)) {
                    f = cand;
                    break;
                }
            }
        }
        if (f && f->node && is_pty_slave(f->node)) {
            if (f->node->open) {
                int pin_err = f->node->open(f->node);
                if (pin_err < 0) return pin_err;
            }
            file_t *file = (file_t *)kmalloc(sizeof(file_t));
            if (!file) {
                if (f->node->close) f->node->close(f->node);
                return SYSCALL_ENOMEM;
            }
            file->node = f->node;
            file->offset = 0;
            file->flags = flags & ~VFS_O_CLOEXEC;
            file->ref_count = 1;
            file->exclusive_owner = false;
            int fd = fd_alloc(curr, file);
            if (fd < 0) {
                if (f->node->close) f->node->close(f->node);
                kfree(file);
                return SYSCALL_EMFILE;
            }
            if (flags & VFS_O_CLOEXEC) {
                curr->fd_flags[fd] |= FD_FLAG_CLOEXEC;
            }
            return (int64_t)fd;
        }
    }

    int vfs_err = 0;
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    file_t *file = vfs_open_creds(kpath, flags & ~VFS_O_CLOEXEC, &actor, &vfs_err);
    if (!file) {
        return syscall_from_vfs_error(vfs_err);
    }

    int fd = fd_alloc(curr, file);
    if (fd < 0) {
        vfs_close(file);
        return SYSCALL_EMFILE;
    }

    if (flags & VFS_O_CLOEXEC) {
        curr->fd_flags[fd] |= FD_FLAG_CLOEXEC;
    }

    return (int64_t)fd;
}

static int64_t sys_pipe_profile(uint64_t action, uintptr_t address, uint64_t size) {
    if (action < PIPE_PROFILE_READ || action > PIPE_PROFILE_DISABLE || size != sizeof(pipe_io_profile_t))
        return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), address, sizeof(pipe_io_profile_t), true))
        return SYSCALL_EFAULT;
    tcb_t *self = thread_current();
    if (!self || !self->is_user) return SYSCALL_EINVAL;
    pipe_io_profile_t snapshot;
    memcpy(&snapshot, (void *)address, sizeof(snapshot));
    file_t *file = snapshot.fd < MAX_PROCESS_FDS ? fd_get(self, (int)snapshot.fd) : NULL;
    if (!file) return SYSCALL_EBADF;
    int64_t result = syscall_from_vfs_error(pipe_profile(file->node, action, &snapshot));
    if (!result) memcpy((void *)address, &snapshot, sizeof(snapshot));
    return result;
}

static int64_t sys_pipe(uintptr_t user_pipefd, uint32_t flags) {
    const size_t bytes = sizeof(int) * 2;
    if (user_pipefd < 0x1000 || user_pipefd > 0x0000800000000000ULL - bytes ||
        !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_pipefd, bytes, true))
        return SYSCALL_EFAULT;
    if (flags & ~VFS_O_CLOEXEC) return SYSCALL_EINVAL;
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;
    unsigned available = 0;
    struct file **cur_pipe_table = curr->process ? curr->process->fd_table : curr->fd_table;
    for (int i = 0; i < MAX_PROCESS_FDS; i++)
        if (!cur_pipe_table[i]) available++;
    if (available < 2) return SYSCALL_EMFILE;

    /* Each process has one thread and owns its fd table. Syscalls enter with
     * IF clear; no publication or sleep occurs between preflight and commit. */
    vfs_node_t *rn, *wn;
    int err = pipe_create(&rn, &wn);
    if (err) return syscall_from_vfs_error(err);
    file_t *rf = kmalloc(sizeof(*rf));
    file_t *wf = kmalloc(sizeof(*wf));
    if (!rf || !wf) {
        kfree(rf);
        kfree(wf);
        pipe_close_endpoint(rn);
        pipe_close_endpoint(wn);
        return SYSCALL_ENOMEM;
    }
    *rf = (file_t){ .node = rn, .flags = VFS_O_RDONLY | flags, .ref_count = 1 };
    *wf = (file_t){ .node = wn, .flags = VFS_O_WRONLY | flags, .ref_count = 1 };
    int rfd = fd_alloc(curr, rf);
    int wfd = rfd < 0 ? -1 : fd_alloc(curr, wf);
    if (wfd < 0) {
        if (rfd >= 0) fd_free(curr, rfd);
        else vfs_close(rf);
        vfs_close(wf);
        return SYSCALL_EMFILE;
    }
    if (flags & VFS_O_CLOEXEC) {
        curr->fd_flags[rfd] = FD_FLAG_CLOEXEC;
        curr->fd_flags[wfd] = FD_FLAG_CLOEXEC;
        if (curr->process) {
            curr->process->fd_flags[rfd] = FD_FLAG_CLOEXEC;
            curr->process->fd_flags[wfd] = FD_FLAG_CLOEXEC;
        }
    }
    int result[2] = {rfd, wfd};
    memcpy((void *)user_pipefd, result, sizeof(result));
    return SYSCALL_SUCCESS;
}

static int64_t sys_pty_pair(uintptr_t user_fds, uint32_t flags) {
    const size_t bytes = sizeof(int) * 2;
    if (user_fds < 0x1000 || user_fds > 0x0000800000000000ULL - bytes ||
        !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_fds, bytes, true))
        return SYSCALL_EFAULT;
    if (flags & ~(uint32_t)(VFS_O_CLOEXEC | VFS_O_NONBLOCK)) return SYSCALL_EINVAL;
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;
    unsigned available = 0;
    struct file **cur_pty_table = curr->process ? curr->process->fd_table : curr->fd_table;
    for (int i = 0; i < MAX_PROCESS_FDS; i++)
        if (!cur_pty_table[i]) available++;
    if (available < 2) return SYSCALL_EMFILE;

    vfs_node_t *mn = NULL, *sn = NULL;
    int err = pty_create(&mn, &sn);
    if (err) return syscall_from_vfs_error(err);

    file_t *mf = kmalloc(sizeof(*mf));
    file_t *sf = kmalloc(sizeof(*sf));
    if (!mf || !sf) {
        if (mf) kfree(mf);
        if (sf) kfree(sf);
        pty_close_endpoint(mn);
        pty_close_endpoint(sn);
        return SYSCALL_ENOMEM;
    }
    *mf = (file_t){ .node = mn, .flags = VFS_O_RDWR | (flags & VFS_O_NONBLOCK), .ref_count = 1 };
    *sf = (file_t){ .node = sn, .flags = VFS_O_RDWR, .ref_count = 1 };

    int mfd = fd_alloc(curr, mf);
    int sfd = mfd < 0 ? -1 : fd_alloc(curr, sf);
    if (sfd < 0) {
        if (mfd >= 0) fd_free(curr, mfd);
        else vfs_close(mf);
        vfs_close(sf);
        return SYSCALL_EMFILE;
    }
    if (flags & VFS_O_CLOEXEC) {
        curr->fd_flags[mfd] = FD_FLAG_CLOEXEC;
        curr->fd_flags[sfd] = FD_FLAG_CLOEXEC;
        if (curr->process) {
            curr->process->fd_flags[mfd] = FD_FLAG_CLOEXEC;
            curr->process->fd_flags[sfd] = FD_FLAG_CLOEXEC;
        }
    }
    int fds[2] = {mfd, sfd};
    memcpy((void *)user_fds, fds, sizeof(fds));
    return SYSCALL_SUCCESS;
}

static int64_t sys_close(int fd) {
    if (fd < 0 || fd >= MAX_PROCESS_FDS) {
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

static int64_t sys_dup2(int oldfd, int newfd) {
    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }
    return (int64_t)fd_dup2(curr, oldfd, newfd);
}

static int64_t sys_dup(int oldfd) {
    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }
    return (int64_t)fd_dup(curr, oldfd);
}

static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_PROCESS_FDS) {
        return SYSCALL_EBADF;
    }
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;
    struct file **table = curr->process ? curr->process->fd_table : curr->fd_table;
    uint32_t *flags = curr->process ? curr->process->fd_flags : curr->fd_flags;
    if (!table[fd]) {
        return SYSCALL_EBADF;
    }

    switch (cmd) {
        case F_GETFD:
            return (int64_t)flags[fd];

        case F_SETFD:
            if (table[fd]->exclusive_owner && arg!=FD_FLAG_CLOEXEC) return SYSCALL_EPERM;
            if (arg & ~(uint64_t)FD_FLAG_CLOEXEC) {
                return SYSCALL_EINVAL;
            }
            flags[fd] = (uint32_t)arg;
            curr->fd_flags[fd] = (uint32_t)arg;
            return SYSCALL_SUCCESS;

        case F_GETFL:
            return (int64_t)table[fd]->flags;

        case F_SETFL:
            if (table[fd]->exclusive_owner) return SYSCALL_EPERM;
            table[fd]->flags = (table[fd]->flags & ~VFS_O_NONBLOCK) |
                                        ((uint32_t)arg & VFS_O_NONBLOCK);
            return SYSCALL_SUCCESS;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            if (table[fd]->exclusive_owner) return SYSCALL_EBADF;
            int min_fd = (int)arg;
            if (min_fd < 0 || min_fd >= MAX_PROCESS_FDS) {
                return SYSCALL_EINVAL;
            }
            for (int i = min_fd; i < MAX_PROCESS_FDS; i++) {
                if (!table[i]) {
                    table[i] = table[fd];
                    __atomic_fetch_add(&table[i]->ref_count, 1, __ATOMIC_ACQ_REL);
                    flags[i] = (cmd == F_DUPFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0;
                    curr->fd_table[i] = table[i];
                    curr->fd_flags[i] = flags[i];
                    return (int64_t)i;
                }
            }
            return SYSCALL_EMFILE;
        }

        default:
            return SYSCALL_EINVAL;
    }
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

    if (fd < 0 || fd >= MAX_PROCESS_FDS) {
        return SYSCALL_EBADF;
    }

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

/* One bounded actor snapshot; G is released before any filesystem work. */
static bool syscall_actor(creds_t *out) {
    tcb_t *curr=thread_current();
    if (!curr) return false;
    uint64_t pid = curr->process ? curr->process->pid : curr->tid;
    return process_record_creds(pid,out);
}

static int64_t sys_stat(uintptr_t user_path, uintptr_t user_statbuf) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    tcb_t *curr = thread_current();
    char kpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    if (!vmm_validate_user_range(active_pml4, user_statbuf, sizeof(vfs_stat_t), true)) {
        return SYSCALL_EFAULT;
    }

    int lookup_error=0;
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    vfs_node_t *node = vfs_lookup_creds(kpath,&actor,&lookup_error);
    if (!node) {
        return syscall_from_vfs_error(lookup_error);
    }

    vfs_stat_t st;
    err=vfs_stat(node, &st);
    vfs_node_put(node);
    if (err) return syscall_from_vfs_error(err);
    memcpy((void *)user_statbuf, &st, sizeof(vfs_stat_t));
    return SYSCALL_SUCCESS;
}

static int64_t sys_stat_ext(uintptr_t path,uintptr_t output,uint64_t size,uint64_t version) {
    if (size!=sizeof(stat_ext_v1_t) || version!=1) return SYSCALL_EINVAL;
    uint64_t *pml4=vmm_get_active_pml4_virt();
    char raw[VFS_MAX_PATH],resolved[VFS_MAX_PATH];
    int r=copy_user_string(pml4,path,raw,sizeof(raw));
    if (r) return r;
    r=resolve_path(thread_current(),raw,resolved,sizeof(resolved));
    if (r) return r;
    if (!vmm_validate_user_range(pml4,output,sizeof(stat_ext_v1_t),true)) return SYSCALL_EFAULT;
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    vfs_node_t *node=vfs_lookup_creds(resolved,&actor,&r);
    if (!node) return syscall_from_vfs_error(r);
    vfs_metadata_t meta;
    r=vfs_metadata(node,&meta);
    vfs_node_put(node);
    if (r) return syscall_from_vfs_error(r);
    stat_ext_v1_t value={.size=sizeof(value),.version=1,.file_size=meta.size,
        .type=meta.type,.mode=meta.mode,.uid=meta.uid,.gid=meta.gid,.mnt_flags=meta.mnt_flags};
    memcpy((void *)output,&value,sizeof(value));
    return 0;
}

/* Metadata belongs to the opened object, never a fresh lookup of node->path.
 * Own-table fd_get is borrowed until signal/wait; retain for the complete
 * snapshot/copy. No new lock or allocation, and no descriptor-position change. */
#include "ext4_stage_test.inc"
static int64_t sys_fstat(uint64_t fd,uintptr_t output) {
    if (fd>=MAX_PROCESS_FDS) return SYSCALL_EBADF;
    tcb_t *self=thread_current();
    file_t *file=self ? fd_get(self,(int)fd) : NULL;
    if (!file) return SYSCALL_EBADF;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),output,sizeof(stat_ext_v1_t),true))
        return SYSCALL_EFAULT;
    if (!vfs_file_get(file)) return SYSCALL_EIO;
    vfs_metadata_t meta={0};
    int r=file->node ? vfs_metadata(file->node,&meta) : -VFS_EIO;
    if (!r) {
        stat_ext_v1_t value={.size=sizeof(value),.version=1,.file_size=meta.size,
            .type=meta.type,.mode=meta.mode,.uid=meta.uid,.gid=meta.gid,.mnt_flags=meta.mnt_flags};
        memcpy((void *)output,&value,sizeof(value));
    }
    vfs_close(file);
    return r ? SYSCALL_EIO : 0;
}

#include "permissions_syscalls.inc"

static int64_t sys_dmesg(uintptr_t user_buf, uint64_t cap) {
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    /* Phase 5 policy is effective-root only, including zero-length probes.
     * Reject before profile publication or touching the caller's buffer. */
    if (actor.euid != 0) return SYSCALL_EPERM;
    if (cap == 0) return 0;
    if (cap > DMESG_SIZE) cap = DMESG_SIZE;

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_buf, cap, true)) {
        return SYSCALL_EFAULT;
    }

    /* On-demand snapshot only; syscall entry keeps IRQs masked on BSP. No
     * hot-path output, new syscall or network configuration semantics. */
    if (cpu_current()->id==0) {
        char profile[1024];
        size_t n=net_poll_profile_format(profile,sizeof(profile));
        dmesg_append_str(profile,n);
    }
    return (int64_t)dmesg_read((char *)user_buf, (size_t)cap);
}

static int64_t sys_net_ifset(uintptr_t user_config, uint64_t size) {
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    if (permission_capability(&actor,CAP_SYS_ADMIN)) return SYSCALL_EPERM;
    if (size != sizeof(netctl_ifset_t)) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),user_config,size,false))
        return SYSCALL_EFAULT;
    netctl_ifset_t input;
    memcpy(&input,(const void *)user_config,sizeof(input));
    net_config_t config;
    int64_t result=net_validate_ifset(&input,&config);
    if (result) return result;
    net_control_value_t value={.set=input};
    return net_control_call(NETCTL_IFSET,&value);
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
    int res = vfs_readdir_file(file, &dent);
    if (res == 1) {
        memcpy((void *)user_dirent, &dent, sizeof(vfs_dirent_t));
        return 1;
    }
    if (res == 0) {
        return 0; /* EOF */
    }
    return syscall_from_vfs_error(res);
}

static int64_t sys_list_dir(uintptr_t user_path, uintptr_t user_names, uint64_t max_entries) {
    if (max_entries == 0) return 0;
    if (max_entries > 16) max_entries = 16;

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    char kpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) {
        return err;
    }

    size_t out_bytes = max_entries * VFS_MAX_NAME; /* 64 bytes per entry */
    if (!vmm_validate_user_range(active_pml4, user_names, out_bytes, true)) {
        return SYSCALL_EFAULT;
    }

    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;

    int open_err = 0;
    file_t *file = vfs_open_creds(kpath, VFS_O_RDONLY, &actor, &open_err);
    if (!file) {
        return syscall_from_vfs_error(open_err);
    }

    if (file->node->type != VFS_DIRECTORY) {
        vfs_close(file);
        return SYSCALL_ENOTDIR;
    }

    uint64_t count = 0;
    char (*names_buf)[VFS_MAX_NAME] = (char (*)[VFS_MAX_NAME])user_names;

    while (count < max_entries) {
        vfs_dirent_t dent;
        int r = vfs_readdir_file(file, &dent);
        if (r == 0) {
            /* EOF */
            break;
        }
        if (r < 0) {
            vfs_close(file);
            return syscall_from_vfs_error(r);
        }
        /* Filter out "." and ".." */
        if ((dent.name[0] == '.' && dent.name[1] == '\0') ||
            (dent.name[0] == '.' && dent.name[1] == '.' && dent.name[2] == '\0')) {
            continue;
        }
        memset(names_buf[count], 0, VFS_MAX_NAME);
        for (size_t i = 0; i < VFS_MAX_NAME - 1 && dent.name[i] != '\0'; i++) {
            names_buf[count][i] = dent.name[i];
        }
        count++;
    }

    vfs_close(file);
    return (int64_t)count;
}

static int64_t sys_reboot(uint64_t cmd) {
    if (cmd!=REBOOT_CMD_RESTART && cmd!=REBOOT_CMD_POWEROFF) return SYSCALL_EINVAL;
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    if (permission_capability(&actor,CAP_SYS_BOOT)) return SYSCALL_EPERM;
    if (cmd == REBOOT_CMD_RESTART) {
        int r=storage_freeze_all();if (r) return syscall_from_vfs_error(r);
        if (!installed_root_quiesce()) return SYSCALL_EIO;
        power_reboot();
    } else if (cmd == REBOOT_CMD_POWEROFF) {
        int r=storage_freeze_all();if (r) return syscall_from_vfs_error(r);
        if (!installed_root_quiesce()) return SYSCALL_EIO;
        power_shutdown();
    }
    return SYSCALL_EINVAL;
}

static int64_t sys_kbd_layout(int64_t layout) {
    if (layout == KBD_LAYOUT_US) {
        keyboard_set_layout(KBD_LAYOUT_US);
        return SYSCALL_SUCCESS;
    } else if (layout == KBD_LAYOUT_AZERTY) {
        keyboard_set_layout(KBD_LAYOUT_AZERTY);
        return SYSCALL_SUCCESS;
    } else if (layout < 0) {
        return (int64_t)keyboard_get_layout();
    }
    return SYSCALL_EINVAL;
}

static int64_t sys_mkdir(uintptr_t user_path, uint64_t mode) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) return err;

    tcb_t *curr = thread_current();
    char kpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) return err;

    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    int res = vfs_mkdir_creds(kpath, (uint32_t)mode, &actor);
    if (res < 0) return syscall_from_vfs_error(res);
    return SYSCALL_SUCCESS;
}

static int64_t sys_unlink(uintptr_t user_path) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) return err;

    tcb_t *curr = thread_current();
    char kpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, kpath, sizeof(kpath));
    if (err != SYSCALL_SUCCESS) return err;

    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    int res = vfs_unlink_creds(kpath, &actor);
    if (res < 0) return syscall_from_vfs_error(res);
    return SYSCALL_SUCCESS;
}

static int64_t sys_rename_paths(uintptr_t user_oldpath, uintptr_t user_newpath,bool replace) {
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_old[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_oldpath, raw_old, sizeof(raw_old));
    if (err != SYSCALL_SUCCESS) return err;

    char raw_new[VFS_MAX_PATH];
    err = copy_user_string(active_pml4, user_newpath, raw_new, sizeof(raw_new));
    if (err != SYSCALL_SUCCESS) return err;

    tcb_t *curr = thread_current();
    char koldpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_old, koldpath, sizeof(koldpath));
    if (err != SYSCALL_SUCCESS) return err;

    char knewpath[VFS_MAX_PATH];
    err = resolve_path(curr, raw_new, knewpath, sizeof(knewpath));
    if (err != SYSCALL_SUCCESS) return err;

    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    int res = replace ? vfs_replace_creds(koldpath,knewpath,&actor) : vfs_rename_creds(koldpath, knewpath, &actor);
    if (res < 0) return syscall_from_vfs_error(res);
    return SYSCALL_SUCCESS;
}

static int64_t sys_chdir(uintptr_t user_path) {
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    char raw_path[VFS_MAX_PATH];
    int err = copy_user_string(active_pml4, user_path, raw_path, sizeof(raw_path));
    if (err != SYSCALL_SUCCESS) return err;

    char resolved[VFS_MAX_PATH];
    err = resolve_path(curr, raw_path, resolved, sizeof(resolved));
    if (err != SYSCALL_SUCCESS) return err;

    int lookup_error=0;
    creds_t actor;
    if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    vfs_node_t *node = vfs_lookup_creds(resolved,&actor,&lookup_error);
    if (!node) return syscall_from_vfs_error(lookup_error);
    bool directory=node->type==VFS_DIRECTORY;
    int access=directory ? vfs_permission(node,VFS_MAY_EXEC,&actor) : 0;
    vfs_node_put(node);
    if (access) return syscall_from_vfs_error(access);
    if (!directory) return SYSCALL_ENOTDIR;

    char canonical[VFS_MAX_PATH];
    int canon=vfs_canonical_path(resolved,canonical,sizeof(canonical));
    if (canon) return syscall_from_vfs_error(canon);
    size_t rlen = strlen(canonical);
    if (rlen >= sizeof(curr->cwd)) return SYSCALL_EINVAL;
    memcpy(curr->cwd, canonical, rlen + 1);
    return SYSCALL_SUCCESS;
}

static int64_t sys_getcwd(uintptr_t user_buf, uint64_t size) {
    if (size == 0) return SYSCALL_EINVAL;
    tcb_t *curr = thread_current();
    if (!curr) return SYSCALL_EBADF;

    const char *cwd = curr->cwd[0] ? curr->cwd : "/";
    size_t cwd_len = strlen(cwd) + 1;
    if (size < cwd_len) return SYSCALL_EINVAL;

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_buf, cwd_len, true)) {
        return SYSCALL_EFAULT;
    }

    memcpy((void *)user_buf, cwd, cwd_len);
    return (int64_t)cwd_len;
}

static int64_t sys_sync(void) {
    /* Drain eligible cleanup and barrier installed root plus external storage.
     * Preserve the first backend error (including EROFS with no writable mount).
     * Mid-session only: does not freeze writers or mark ext4 clean. */
    int sync_result = storage_sync_all();
    usb_report_io_profile();
    char ext4_profile[2048];
    size_t profile_len=ext4_io_profile_format(ext4_profile,sizeof(ext4_profile));
    dmesg_append_str(ext4_profile,profile_len);
    if (sync_result) {
        usb_report_flush_failure();
        return syscall_from_vfs_error(sync_result);
    }
    return SYSCALL_SUCCESS;
}

static int64_t sys_mountinfo(uint32_t index, uintptr_t user_buf) {
    uint64_t *pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(pml4, user_buf, sizeof(mount_info_t), true)) {
        return SYSCALL_EFAULT;
    }

    /* Index 0: Root filesystem */
    if (index == 0) {
        mount_info_t info;
        memset(&info, 0, sizeof(info));
        memcpy(info.mount_path, "/", 2);

        vfs_node_t *root = vfs_lookup_kernel("/");
        if (root && ext4_get_mount_info(root, &info)) {
            memcpy(info.mount_path, "/", 2);
            memcpy((void *)user_buf, &info, sizeof(info));
            return 1;
        }
        if (root && ext2_get_mount_info(root, &info)) {
            memcpy(info.mount_path, "/", 2);
            memcpy((void *)user_buf, &info, sizeof(info));
            return 1;
        }

        /* Default fallback: Root filesystem (TarFS / initramfs) */
        memcpy(info.source, "initramfs", 10);
        memcpy(info.mount_path, "/", 2);
        info.fs_type = VFS_FS_TARFS;
        info.flags = MOUNT_FLAGS_RO;
        info.block_size = 512;
        size_t archive_size = 0;
        uint32_t file_count = 0;
        tarfs_get_stats(&archive_size, &file_count);
        info.total_blocks = archive_size ? (archive_size / 512) : 0;
        info.free_blocks = 0;
        info.total_inodes = file_count;
        info.free_inodes = 0;
        memcpy((void *)user_buf, &info, sizeof(info));
        return 1;
    }

    /* Index 1: Persistent storage (/mnt) if mounted */
    if (index == 1) {
        vfs_node_t *node = vfs_lookup_kernel("/mnt");
        if (!node) return 0;

        mount_info_t info;
        memset(&info, 0, sizeof(info));
        memcpy(info.mount_path, "/mnt", 5);

        if (ext2_get_mount_info(node, &info)) {
            memcpy(info.mount_path, "/mnt", 5);
            memcpy((void *)user_buf, &info, sizeof(info));
            return 1;
        }

        if (ext4_get_mount_info(node, &info)) {
            memcpy(info.mount_path, "/mnt", 5);
            memcpy((void *)user_buf, &info, sizeof(info));
            return 1;
        }

        /* Check if other mounted filesystem */
        memcpy(info.source, "unknown", 8);
        info.fs_type = VFS_FS_NONE;
        info.flags = (node->write != NULL) ? MOUNT_FLAGS_RW : MOUNT_FLAGS_RO;
        info.block_size = 4096;
        info.total_blocks = 0;
        info.free_blocks = 0;
        info.total_inodes = 0;
        info.free_inodes = 0;
        memcpy((void *)user_buf, &info, sizeof(info));
        return 1;
    }

    return 0;
}

static int64_t sys_blockinfo(uint32_t index, uintptr_t user_buf) {
    uint64_t *pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(pml4, user_buf, sizeof(block_info_t), true)) {
        return SYSCALL_EFAULT;
    }

    /* Index 0: initramfs (TarFS / RAM) */
    if (index == 0) {
        block_info_t info;
        memset(&info, 0, sizeof(info));
        memcpy(info.name, "initramfs", 10);
        info.sector_size = 512;
        size_t archive_size = 0;
        uint32_t file_count = 0;
        tarfs_get_stats(&archive_size, &file_count);
        info.size_bytes = (uint64_t)archive_size;
        memcpy((void *)user_buf, &info, sizeof(info));
        return 1;
    }

    /* Index 1..N: Registered block devices */
    size_t dev_idx = (size_t)(index - 1);
    block_dev_t *dev = block_get_dev_by_index(dev_idx);
    if (!dev) {
        return 0;
    }

    block_info_t info;
    memset(&info, 0, sizeof(info));
    size_t nlen = strlen(dev->name);
    if (nlen >= sizeof(info.name)) nlen = sizeof(info.name) - 1;
    memcpy(info.name, dev->name, nlen);
    info.name[nlen] = '\0';
    info.sector_size = dev->sector_size;
    info.size_bytes = dev->sector_count * (uint64_t)dev->sector_size;

    /* Check if this device is a GPT partition */
    if (dev->priv != NULL) {
        for (size_t p = 0; p < gpt_get_partition_count(); p++) {
            gpt_partition_t *part = gpt_get_partition(p);
            if (part && (&part->block_dev == dev || part == dev->priv)) {
                info.is_partition = 1;
                info.part_index = part->part_index;
                info.start_lba = part->starting_lba;
                info.sector_count = part->sector_count;
                gpt_guid_to_str(&part->unique_guid, info.partuuid);
                gpt_guid_to_str(&part->type_guid, info.type_guid);
                size_t llen = strlen(part->label);
                if (llen >= sizeof(info.label)) llen = sizeof(info.label) - 1;
                memcpy(info.label, part->label, llen);
                info.label[llen] = '\0';
                break;
            }
        }
    }

    memcpy((void *)user_buf, &info, sizeof(info));
    return 1;
}

static int64_t sys_lockstat(uintptr_t user_buf, uint64_t cap) {
    if (user_buf == 0 || cap == 0) {
        lockstat_dump(NULL, 0);
        return 0;
    }
    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_buf, cap, true)) {
        return SYSCALL_EFAULT;
    }
    return (int64_t)lockstat_dump((char *)user_buf, (size_t)cap);
}

static int64_t sys_fbinfo(uintptr_t user_info_ptr, uintptr_t req_vaddr) {
    tcb_t *curr=thread_current();
    if (!curr || !curr->is_user) return SYSCALL_EPERM;
    uint64_t *root=vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(root,user_info_ptr,sizeof(fb_info_v1_t),true)) return SYSCALL_EFAULT;
    fb_info_v1_t info;
    int64_t result=fb_map(curr->cr3,root,req_vaddr,&info);
    if (!result) memcpy((void *)user_info_ptr,&info,sizeof(info));
    return result;
}

static int64_t sys_mmap(uint64_t bytes) {
    if (!bytes || bytes>32*1024*1024ULL) return SYSCALL_EINVAL;
    bytes=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    const uintptr_t base=0x4000000000ULL;
    int error=vmm_map_anonymous_range(vmm_get_active_pml4_virt(),base,bytes);
    return error==VMM_OK ? (int64_t)base : error==VMM_ERR_ALREADY_MAPPED ? SYSCALL_EBUSY :
        error==VMM_ERR_NOMEM ? SYSCALL_ENOMEM : SYSCALL_EINVAL;
}

int64_t syscall_dispatch(interrupt_frame_t *frame) {
    if (!frame) return SYSCALL_EINVAL;

    tcb_t *curr = thread_current();
    if (curr) {
        time_account_user_to_kernel(curr);
    }

    uint64_t syscall_nr = frame->rax;
    int64_t result = SYSCALL_ENOSYS;
    int return_disposition = 0; /* 0=normal, 1=SIGRETURN (iretq, skip rax write) */

    switch (syscall_nr) {
        case SYS_SPAWN:
            result = sys_spawn(frame->rdi, frame->rsi);
            break;
        case SYS_TERMCTL:
            result = sys_termctl(frame->rdi, frame->rsi, frame->rdx);
            break;
        case SYS_TCSETPGRP: {
            tcb_t *curr = thread_current();
            file_t *f = curr && frame->rdi < MAX_PROCESS_FDS ? fd_get(curr, (int)frame->rdi) : NULL;
            if (f && is_pty_slave(f->node)) result = pty_tcsetpgrp(f->node, frame->rsi);
            else result = input_tcsetpgrp(frame->rdi, frame->rsi);
            break;
        }
        case SYS_TCGETPGRP: {
            tcb_t *curr = thread_current();
            file_t *f = curr && frame->rdi < MAX_PROCESS_FDS ? fd_get(curr, (int)frame->rdi) : NULL;
            if (f && is_pty_slave(f->node)) {
                int64_t pgid = 0;
                int64_t r = pty_tcgetpgrp(f->node, &pgid);
                result = r ? r : pgid;
            } else result = input_tcgetpgrp(frame->rdi);
            break;
        }
        case SYS_TERMATTR:
            result=sys_termattr(frame->rdi,frame->rsi,frame->rdx,frame->r10);
            break;
        case SYS_INPUT_READ:
            result = sys_input_read(frame->rdi, frame->rsi, (int64_t)frame->rdx);
            break;
        case SYS_WAIT:
            result = sys_wait(frame->rdi, frame->rsi);
            break;
        case SYS_EXIT:
            result = sys_exit(frame->rdi, frame);
            break;

        case SYS_WRITE:
            if (frame->rdi<MAX_PROCESS_FDS && net_socket_stream(fd_get(thread_current(),(int)frame->rdi)))
                result=net_tcp_syscall(frame);
            else result = sys_write(frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_OPEN:
            result = sys_open(frame->rdi, (int)frame->rsi);
            break;

        case SYS_CLOSE:
            result = sys_close((int)frame->rdi);
            break;

        case SYS_READ:
            if (frame->rdi>=MAX_PROCESS_FDS) result=SYSCALL_EBADF;
            else if (net_socket_stream(fd_get(thread_current(),(int)frame->rdi)))
                result=net_tcp_syscall(frame);
            else result = sys_read((int)frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_DMESG:
            result = sys_dmesg(frame->rdi, frame->rsi);
            break;

        case SYS_STAT_EXT:
            result=sys_stat_ext(frame->rdi,frame->rsi,frame->rdx,frame->r10);
            break;
        case SYS_UMASK: result=sys_umask(frame->rdi);break;
        case SYS_CHMOD: result=sys_chmod(frame->rdi,frame->rsi);break;
        case SYS_FCHMOD: result=sys_fchmod(frame->rdi,frame->rsi);break;
        case SYS_CHOWN: result=sys_chown(frame->rdi,frame->rsi,frame->rdx,frame->r10);break;
        case SYS_GETRESUID: result=sys_getres(false,frame->rdi,frame->rsi,frame->rdx);break;
        case SYS_GETRESGID: result=sys_getres(true,frame->rdi,frame->rsi,frame->rdx);break;
        case SYS_GETGROUPS: result=sys_getgroups(frame->rdi,frame->rsi);break;
        case SYS_SETRESUID: result=sys_setres(false,frame->rdi,frame->rsi,frame->rdx,frame->r10);break;
        case SYS_SETRESGID: result=sys_setres(true,frame->rdi,frame->rsi,frame->rdx,frame->r10);break;
        case SYS_SETGROUPS: result=sys_setgroups(frame->rdi,frame->rsi);break;
        case SYS_CAPSET: result=sys_capset(frame->rdi);break;
        case SYS_CAPGET: result=sys_capget();break;
        case SYS_INSTALLCTL: {
            creds_t actor;
            if (frame->rdi>INSTALL_LOG) {result=SYSCALL_EINVAL;break;}
            if (!syscall_actor(&actor)) {result=SYSCALL_ESRCH;break;}
            result=install_control((unsigned)frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8,&actor);break;
        }
        case SYS_ACCOUNTCTL: {
            creds_t actor;tcb_t *current=thread_current();
            result=current && syscall_actor(&actor) ? accounts_control(frame->rdi,frame->rsi,frame->rdx,&actor,current->tid) : SYSCALL_ESRCH;
            break;
        }
        case SYS_GETRANDOM:
            result=random_user(frame->rdi,frame->rsi,frame->rdx);
            break;
        case SYS_INSTALLSEED: {
            creds_t actor;
            result=syscall_actor(&actor) ? accounts_seed(frame->rdi,frame->rsi,&actor) : SYSCALL_ESRCH;
            break;
        }
#ifdef TEST_PERMISSIONS_ENFORCEMENT
        case SYS_TEST_SETCREDS: result=sys_test_setcreds();break;
#endif

        case SYS_STAT:
        case SYS_LSTAT:
            result = sys_stat(frame->rdi, frame->rsi);
            break;
        case SYS_HOSTKEY: {
            creds_t actor;
            result=syscall_actor(&actor) ? hostkey_control(frame->rdi,frame->rsi,frame->rdx,&actor) : SYSCALL_ESRCH;
            break;
        }
        case SYS_FSTAT:
            result = sys_fstat(frame->rdi,frame->rsi);
            break;
        case SYS_STAGE_WRITE:
            result=sys_stage_test_write(frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8);
            break;
        case SYS_FSYNC:
            result=sys_stage_test_sync(frame->rdi);
            break;
        case SYS_WIFI:
            result = net_wifi_syscall(frame->rdi, frame->rsi, frame->rdx);
            break;
        case SYS_POLL:
            result = sys_poll(frame->rdi, frame->rsi, (int64_t)frame->rdx);
            break;

        case SYS_THREAD_CREATE:
            result = sys_thread_create(frame->rdi, frame->rsi, frame->rdx, (uint64_t *)frame->r10);
            break;

        case SYS_THREAD_EXIT:
            result = sys_thread_exit(frame->rdi);
            break;

        case SYS_THREAD_JOIN:
            result = sys_thread_join(frame->rdi, (uint64_t *)frame->rsi);
            break;

        case SYS_THREAD_SELF:
            result = (int64_t)sys_thread_self();
            break;

        case SYS_THREAD_YIELD:
            result = sys_thread_yield();
            break;

        case SYS_FUTEX:
            result = sys_futex((uint32_t *)frame->rdi, (int)frame->rsi, (uint32_t)frame->rdx, (const timespec_t *)frame->r10);
            break;

        case SYS_READDIR:
            result = sys_readdir((int)frame->rdi, frame->rsi);
            break;

        case SYS_LIST_DIR:
            result = sys_list_dir(frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_REBOOT:
            result = sys_reboot(frame->rdi);
            break;

        case SYS_KBD_LAYOUT:
            result = sys_kbd_layout((int64_t)frame->rdi);
            break;

        case SYS_MKDIR:
            result = sys_mkdir(frame->rdi, frame->rsi);
            break;

        case SYS_UNLINK:
            result = sys_unlink(frame->rdi);
            break;

        case SYS_RENAME:
            result = sys_rename_paths(frame->rdi, frame->rsi,false);
            break;
        case SYS_RENAME_REPLACE:
            result = frame->rdx ? SYSCALL_EINVAL : sys_rename_paths(frame->rdi,frame->rsi,true);
            break;

        case SYS_SYNC:
            result = sys_sync();
            break;

        case SYS_GETCWD:
            result = sys_getcwd(frame->rdi, frame->rsi);
            break;

        case SYS_CHDIR:
            result = sys_chdir(frame->rdi);
            break;

        case SYS_SPAWN_EXT:
            result = sys_spawn_ext(frame->rdi, frame->rsi, frame->rdx);
            break;

        case SYS_DUP2:
            result = sys_dup2((int)frame->rdi, (int)frame->rsi);
            break;

        case SYS_DUP:
            result = sys_dup((int)frame->rdi);
            break;

        case SYS_FCNTL:
            result = sys_fcntl((int)frame->rdi, (int)frame->rsi, frame->rdx);
            break;

        case SYS_KILL:
            result=process_signal_send_creds(thread_current()->tid,(int64_t)frame->rdi,frame->rsi);
            break;
        case SYS_SIGACTION: {
            signal_action_t act, old;
            uint64_t *pml4=vmm_get_active_pml4_virt();
            if ((frame->rsi && !vmm_validate_user_range(pml4,frame->rsi,sizeof(act),false)) ||
                (frame->rdx && !vmm_validate_user_range(pml4,frame->rdx,sizeof(old),true))) {
                result=SYSCALL_EFAULT; break;
            }
            if (frame->rsi) memcpy(&act,(void *)frame->rsi,sizeof(act));
            result=process_signal_action(thread_current()->tid,frame->rdi,frame->rsi ? &act:NULL,&old);
            if (!result && frame->rdx) memcpy((void *)frame->rdx,&old,sizeof(old));
            break;
        }
        case SYS_SIGPROCMASK: {
            uint64_t mask, old;
            uint64_t *pml4=vmm_get_active_pml4_virt();
            if ((frame->rsi && !vmm_validate_user_range(pml4,frame->rsi,sizeof(mask),false)) ||
                (frame->rdx && !vmm_validate_user_range(pml4,frame->rdx,sizeof(old),true))) {
                result=SYSCALL_EFAULT; break;
            }
            if (frame->rsi) memcpy(&mask,(void *)frame->rsi,sizeof(mask));
            result=process_signal_mask(thread_current()->tid,frame->rdi,frame->rsi ? &mask:NULL,&old);
            if (!result && frame->rdx) memcpy((void *)frame->rdx,&old,sizeof(old));
            break;
        }
        case SYS_SETPGID:
            result = process_setpgid(frame->rdi, frame->rsi);
            break;
        case SYS_GETPGRP:
            result = process_getpgrp();
            break;
        case SYS_GROUP_RELEASE:
            result = frame->rsi > GROUP_CANCEL ? SYSCALL_EINVAL :
                     process_group_release(frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_WAITPID: {
            uint64_t status;
            if (frame->rdx > 0xffffffffULL) { result=SYSCALL_EINVAL; break; }
            if (frame->rsi && !vmm_validate_user_range(vmm_get_active_pml4_virt(),
                                      frame->rsi, sizeof(status), true)) {
                result=SYSCALL_EFAULT; break;
            }
            result=process_waitpid((int64_t)frame->rdi, &status, (uint32_t)frame->rdx, false);
            if (result > 0 && frame->rsi) memcpy((void *)frame->rsi, &status, sizeof(status));
            break;
        }

        case SYS_PROCINFO: {
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), frame->rsi,
                                         sizeof(proc_info_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            process_snapshot_t snap;
            bool have_snapshot = false;
            if (frame->rdi == PROC_INFO_SELF) {
                tcb_t *self = thread_current();
                have_snapshot = self && process_record_snapshot_pid(self->tid, &snap);
                if (have_snapshot) snap.cpu_ticks = self->total_ticks;
            } else {
                process_refresh_cpu_ticks();
                have_snapshot = frame->rdi < PROC_INFO_MAX && process_record_snapshot(frame->rdi, &snap);
            }
            if (have_snapshot) {
                proc_info_t info;
                memset(&info, 0, sizeof(info));
                info.pid = (int64_t)snap.pid;
                info.ppid = (int64_t)snap.parent;
                info.pgid = (int64_t)snap.pgid;
                info.sid = (int64_t)snap.sid;
                info.state = (uint32_t)snap.state;
                info.reserved = 0;
                info.cpu_ticks = snap.cpu_ticks;
                memcpy(info.name, snap.name, sizeof(info.name));
                info.name[sizeof(info.name) - 1] = '\0';
                memcpy((void *)frame->rsi, &info, sizeof(info));
                result = 1;
            } else {
                result = 0;
            }
            break;
        }

        case SYS_SOCKET:
        case SYS_BIND:
        case SYS_SENDTO:
        case SYS_RECVFROM:
        case SYS_CONNECT:
        case SYS_LISTEN:
        case SYS_ACCEPT:
        case SYS_SEND:
        case SYS_RECV:
        case SYS_SHUTDOWN:
        case SYS_SEND_UNTIL:
        case SYS_RECV_UNTIL:
        case SYS_CONNECT_UNTIL:
            result=net_socket_syscall(frame);
            break;

        case SYS_NETCTL: {
            if (frame->rdi == NETCTL_IFSET) {
                result=sys_net_ifset(frame->rsi,frame->rdx);
                break;
            }
            unsigned kind=(unsigned)frame->rdi;
            size_t size=kind==NETCTL_PING ? sizeof(net_ping_v1_t) :
                kind==NETCTL_TRACE_PROBE ? sizeof(net_trace_v1_t) :
                kind==NETCTL_IFGET ? sizeof(netctl_ifget_t) : 0;
            if (!size || frame->rdi!=kind || frame->rdx!=size) {
                result=SYSCALL_EINVAL; break;
            }
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),frame->rsi,size,true)) {
                result=SYSCALL_EFAULT; break;
            }
            net_control_value_t value={0};
            memcpy(&value,(const void *)frame->rsi,size);
            result=kind==NETCTL_PING ? net_ping_validate(&value.ping) :
                kind==NETCTL_TRACE_PROBE ? net_trace_validate(&value.trace) :
                (value.get.struct_version!=1 || value.get.reserved) ? SYSCALL_EINVAL : 0;
            if (result) break;
            result=net_control_call(kind,&value);
            if (result) break;
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),frame->rsi,size,true)) {
                result=SYSCALL_EFAULT; break;
            }
            memcpy((void *)frame->rsi,&value,size);
            break;
        }

        case SYS_SPAWN_PROFILE: {
            if (frame->rdi >= PIPE_PROFILE_READ && frame->rdi <= PIPE_PROFILE_DISABLE) {
                result = sys_pipe_profile(frame->rdi, frame->rsi, frame->rdx);
                break;
            }
            if (frame->rdi >= WAIT_PROFILE_READ && frame->rdi <= WAIT_PROFILE_DISABLE) {
                if (frame->rdx != sizeof(wait_profile_t)) { result = SYSCALL_EINVAL; break; }
                if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), frame->rsi,
                                             sizeof(wait_profile_t), true)) { result = SYSCALL_EFAULT; break; }
                tcb_t *self = thread_current();
                if (!self || !self->is_user) { result = SYSCALL_EINVAL; break; }
                if (frame->rdi == WAIT_PROFILE_ENABLE) {
                    memset(&self->wait_trace, 0, sizeof(self->wait_trace));
                    self->wait_trace.counters.valid = 1;
                    self->wait_trace.enabled = true;
                } else if (frame->rdi == WAIT_PROFILE_DISABLE) self->wait_trace.enabled = false;
                if (self->wait_trace.stage) self->wait_trace.counters.valid = 0;
                memcpy((void *)frame->rsi, &self->wait_trace.counters, sizeof(wait_profile_t));
                result = 0;
                break;
            }
            if (frame->rdi > SPAWN_PROFILE_DISABLE || frame->rdx != sizeof(spawn_profile_t)) {
                result = SYSCALL_EINVAL;
                break;
            }
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), frame->rsi,
                                         sizeof(spawn_profile_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            tcb_t *self = thread_current();
            if (!self || !self->is_user) { result = SYSCALL_EINVAL; break; }
            if (frame->rdi == SPAWN_PROFILE_ENABLE) {
                uint64_t queued = self->spawn_profile.queued_cycles;
                uint64_t first = self->spawn_profile.first_run_cycles;
                memset(&self->spawn_profile, 0, sizeof(self->spawn_profile));
                self->spawn_profile.queued_cycles = queued;
                self->spawn_profile.first_run_cycles = first;
                self->spawn_profile.valid = 1;
                self->spawn_profile_enabled = true;
            } else if (frame->rdi == SPAWN_PROFILE_DISABLE) {
                self->spawn_profile_enabled = false;
            }
            self->spawn_profile.birth_valid = self->spawn_profile.queued_cycles > 0 &&
                self->spawn_profile.first_run_cycles >= self->spawn_profile.queued_cycles;
            memcpy((void *)frame->rsi, &self->spawn_profile, sizeof(spawn_profile_t));
            result = 0;
            break;
        }

        case SYS_SYSINFO: {
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), frame->rdi,
                                         sizeof(sysinfo_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            sysinfo_t info;
            memset(&info, 0, sizeof(info));
            info.total_ram_bytes   = pmm_get_managed_ram_bytes();
            info.free_ram_bytes    = (uint64_t)pmm_get_free_pages() * PAGE_SIZE;
            info.uptime_ticks      = apic_timer_get_bsp_ticks();
            info.tick_hz           = apic_timer_get_frequency();
            info.cpu_count         = (uint32_t)smp_get_cpu_count();
            info.task_count        = process_record_count_enumerable();
            info.tsc_hz            = apic_poll_clock_hz();
            info.kernel_heap_used  = heap_get_used_bytes();
            info.kernel_heap_total = heap_get_total_bytes();
            info.thread_count      = (uint32_t)sched_get_thread_count();
            memcpy((void *)frame->rdi, &info, sizeof(info));
            result = 0;
            break;
        }

        case SYS_PIPE:
            result = sys_pipe(frame->rdi, (uint32_t)frame->rsi);
            break;

        case SYS_PTY_PAIR:
            result = sys_pty_pair(frame->rdi, (uint32_t)frame->rsi);
            break;

        case SYS_MOUNTINFO:
            result = sys_mountinfo((uint32_t)frame->rdi, frame->rsi);
            break;

        case SYS_BLOCKINFO:
            result = sys_blockinfo((uint32_t)frame->rdi, frame->rsi);
            break;

        case SYS_LOCKSTAT:
            result = sys_lockstat(frame->rdi, frame->rsi);
            break;

        case SYS_MEMINFO: {
            if (frame->rsi < sizeof(sysinfo_mem_t)) {
                result = SYSCALL_EINVAL;
                break;
            }
            if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), frame->rdi,
                                         sizeof(sysinfo_mem_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            sysinfo_mem_t mem;
            memset(&mem, 0, sizeof(mem));
            mem.struct_size = sizeof(sysinfo_mem_t);
            mem.flags = 0;

            pmm_stats_t pmm;
            pmm_get_stats(&pmm);
            mem.pmm_total_frames       = (uint64_t)pmm.total_pages;
            mem.pmm_used_frames        = (uint64_t)pmm.used_pages;
            mem.pmm_free_frames        = (uint64_t)pmm.free_pages;
            mem.pmm_allocatable_frames = (uint64_t)pmm.allocatable_pages;

            heap_stats_t heap;
            heap_get_stats(&heap);
            mem.heap_used_bytes      = (uint64_t)heap.used_bytes;
            mem.heap_free_bytes      = (uint64_t)heap.free_bytes;
            mem.heap_committed_bytes = (uint64_t)heap.total_bytes;
            mem.heap_largest_payload = (uint64_t)heap.largest_free_payload;
            mem.heap_free_blocks     = (uint64_t)heap.free_blocks;

            mem.vmm_table_frames     = (uint64_t)vmm_get_allocated_table_frames();
            mem.vmm_deferred_spaces  = (uint64_t)vmm_get_deferred_count();

            memcpy((void *)frame->rdi, &mem, sizeof(mem));
            result = 0;
            break;
        }

        case SYS_CLOCK_SETTIME: {
            creds_t actor;
            if (!syscall_actor(&actor)) { result=SYSCALL_ESRCH; break; }
            if (permission_capability(&actor,CAP_SYS_ADMIN)) { result=SYSCALL_EPERM; break; }
            if (frame->rdi!=CLOCK_REALTIME) { result=SYSCALL_EINVAL; break; }
            if (!frame->rsi || !vmm_validate_user_range(vmm_get_active_pml4_virt(),frame->rsi,sizeof(timespec_t),false)) {
                result=SYSCALL_EFAULT; break;
            }
            timespec_t ts;
            memcpy(&ts,(const void *)frame->rsi,sizeof(ts));
            result=time_set_realtime(&ts);
            break;
        }
        case SYS_CLOCK_GETTIME: {
            uint32_t clk_id = (uint32_t)frame->rdi;
            uintptr_t user_ts = frame->rsi;
            if (!user_ts || !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_ts, sizeof(timespec_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            timespec_t ts;
            int ret = time_get_timespec(clk_id, &ts);
            if (ret < 0) {
                result = ret;
                break;
            }
            memcpy((void *)user_ts, &ts, sizeof(timespec_t));
            result = 0;
            break;
        }

        case SYS_NANOSLEEP: {
            uintptr_t user_req = frame->rdi;
            uintptr_t user_rem = frame->rsi;
            if (!user_req || !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_req, sizeof(timespec_t), false)) {
                result = SYSCALL_EFAULT;
                break;
            }
            if (user_rem != 0 && !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_rem, sizeof(timespec_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            timespec_t req;
            memcpy(&req, (const void *)user_req, sizeof(timespec_t));
            if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) {
                result = SYSCALL_EINVAL;
                break;
            }
            if (req.tv_sec == 0 && req.tv_nsec == 0) {
                thread_yield();
                result = 0;
                break;
            }
            uint64_t hz = apic_timer_get_frequency();
            if (!hz) hz = 100;
            uint64_t sec_ticks = 0;
            if ((uint64_t)req.tv_sec > UINT64_MAX / hz) {
                sec_ticks = UINT64_MAX - 1000000ULL;
            } else {
                sec_ticks = (uint64_t)req.tv_sec * hz;
            }
            uint64_t ns_per_tick = 1000000000ULL / hz;
            uint64_t nsec_ticks = ((uint64_t)req.tv_nsec + ns_per_tick - 1) / ns_per_tick;
            uint64_t total_ticks = sec_ticks + nsec_ticks;

            uint64_t start_ticks = apic_timer_get_bsp_ticks();
            uint64_t deadline = (total_ticks > UINT64_MAX - start_ticks) ? UINT64_MAX : (start_ticks + total_ticks);

            sched_wait_until(time_get_sleep_channel(), nanosleep_ready, &deadline);

            uint64_t end_ticks = apic_timer_get_bsp_ticks();
            if (end_ticks < deadline && process_signal_pending()) {
                if (user_rem != 0) {
                    uint64_t rem_ticks = deadline - end_ticks;
                    timespec_t rem;
                    rem.tv_sec = (int64_t)(rem_ticks / hz);
                    rem.tv_nsec = (int64_t)((rem_ticks % hz) * ns_per_tick);
                    memcpy((void *)user_rem, &rem, sizeof(timespec_t));
                }
                result = SYSCALL_EINTR;
                break;
            }
            result = 0;
            break;
        }

        case SYS_TIMES: {
            int64_t target_pid = (int64_t)frame->rdi;
            uintptr_t user_buf = frame->rsi;
            if (!user_buf || !vmm_validate_user_range(vmm_get_active_pml4_virt(), user_buf, sizeof(tms_t), true)) {
                result = SYSCALL_EFAULT;
                break;
            }
            uint64_t caller_pid = curr ? curr->tid : 0;
            tms_t tms;
            result = process_get_times(caller_pid, target_pid, &tms);
            if (result == 0) {
                memcpy((void *)user_buf, &tms, sizeof(tms_t));
            }
            break;
        }

        case SYS_PKGCTL:
            result=pkg_service_control(frame->rdi,frame->rsi,frame->rdx);
            break;

        case SYS_FB_ACQUIRE: {
            creds_t actor;
            result=(!curr || !curr->is_user || !syscall_actor(&actor) || actor.euid!=0)
                ? SYSCALL_EPERM : fb_acquire(curr->cr3);
            break;
        }
        case SYS_FB_RELEASE:
            result=curr && curr->is_user ? fb_release(curr->cr3) : SYSCALL_EPERM;
            break;
        case SYS_MMAP:
            result=curr && curr->is_user ? sys_mmap(frame->rdi) : SYSCALL_EPERM;
            break;
        case SYS_SHM_CREATE:
            result=frame->rsi > UINT32_MAX ? SYSCALL_EINVAL : shm_create(frame->rdi,(uint32_t)frame->rsi);
            break;
        case SYS_SHM_MAP:
            if (frame->rdi >= MAX_PROCESS_FDS) result=SYSCALL_EBADF;
            else if (frame->rdx > UINT32_MAX) result=SYSCALL_EINVAL;
            else result=shm_map((int)frame->rdi,frame->rsi,(uint32_t)frame->rdx);
            break;
        case SYS_SHM_UNMAP:
            result=shm_unmap(frame->rdi);
            break;
        case SYS_FBINFO:
            result = sys_fbinfo(frame->rdi, frame->rsi);
            break;

        case SYS_SIGRETURN:
            result = sys_sigreturn(frame, &return_disposition);
            break;

        default:
            result = SYSCALL_ENOSYS;
            break;
    }

    /* RETURN_SIGRETURN: sys_sigreturn committed full context into the frame.
     * Signal the assembly stub to use IRETQ (not SYSRET) by writing the
     * SIGRETURN_VECTOR_MARKER (0x100) into frame->vector. After GPR pops,
     * the assembly checks [rsp] == 0x100 and jumps to syscall_sigreturn_iretq.
     * frame->rax already holds kf.rax; do NOT overwrite. Skip signal delivery. */
    if (return_disposition == 1) {
        frame->vector = 0x100; /* SIGRETURN_VECTOR_MARKER — matches syscall_entry.asm */
        if (curr) {
            time_account_kernel_to_user(curr);
        }
        return 0;
    }

    frame->rax = (uint64_t)result;
    sched_resched_user_return(frame);
    process_signal_user_return(frame);

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
        if (curr && curr->is_user) {
            serial_puts("[SYSCALL] Terminating rogue process due to invalid return state (exit 141)\n");
            process_exit(141); /* 128 + 13 (#GP) */
            /* Never reached */
        }
    }

    if (curr) {
        time_account_kernel_to_user(curr);
    }
    return result;
}
