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
#include "heap.h"
#include "elf.h"
#include "usb_mount.h"
#include "xhci.h"
#include "dmesg.h"
#include "terminal.h"
#include "console.h"

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

int64_t syscall_from_vfs_error(int64_t vfs_err) {
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
        case -VFS_ENOTEMPTY:   return SYSCALL_ENOTEMPTY;   /* -19 */
        case -VFS_EOPNOTSUPP:  return SYSCALL_EOPNOTSUPP;  /* -14 */
        case -7:               return SYSCALL_EISDIR;      /* -7 */
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

    int64_t res = vfs_write(file, (const void *)user_buf, count);
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
        return (file->node == vfs_get_terminal_node()) ? 1 : 0;
    }
    if (size != sizeof(terminal_info_t) || op > TERM_SET) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(), ptr, size, true)) return SYSCALL_EFAULT;
    terminal_info_t *info = (terminal_info_t *)ptr;
    if (op == TERM_SET) {
        if (info->version != 1 || info->mode > TERM_PLAIN ||
            (info->cols && (info->cols < 20 || info->cols > 512))) return SYSCALL_EINVAL;
        if (info->mode == TERM_LOCAL && !console_is_initialized()) return SYSCALL_EOPNOTSUPP;
        if (info->mode == TERM_SERIAL && !serial_is_available()) return SYSCALL_EOPNOTSUPP;
        t->terminal_mode = info->mode;
        t->terminal_cols = info->cols;
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
    return input_read_timeout((void *)ptr, count, timeout);
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

static int resolve_path(tcb_t *proc, const char *in_path, char *out_path, size_t out_cap) {
    if (!in_path || !out_path || out_cap < 2) return SYSCALL_EINVAL;

    char combined[VFS_MAX_PATH * 2];
    size_t in_len = strlen(in_path);

    if (in_path[0] == '/') {
        if (in_len >= sizeof(combined)) return SYSCALL_EINVAL;
        memcpy(combined, in_path, in_len + 1);
    } else {
        const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
        size_t cwd_len = strlen(cwd);
        if (cwd_len + 1 + in_len >= sizeof(combined)) return SYSCALL_EINVAL;
        memcpy(combined, cwd, cwd_len);
        if (cwd_len > 0 && combined[cwd_len - 1] != '/') {
            combined[cwd_len] = '/';
            cwd_len++;
        }
        memcpy(combined + cwd_len, in_path, in_len + 1);
    }

    char *out = out_path;
    *out++ = '/';
    *out = '\0';

    const char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg_start);

        if (seg_len == 1 && seg_start[0] == '.') {
            continue;
        }

        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            if (out > out_path + 1) {
                out--;
                while (out > out_path && *out != '/') out--;
                if (out == out_path) {
                    out = out_path + 1;
                    *out = '\0';
                } else {
                    *out = '\0';
                }
            }
            continue;
        }

        size_t current_len = (size_t)(out - out_path);
        size_t needed = (current_len > 1 ? 1 : 0) + seg_len + 1;
        if (current_len + needed > out_cap) {
            return SYSCALL_EINVAL;
        }

        if (current_len > 1) {
            *out++ = '/';
        }
        memcpy(out, seg_start, seg_len);
        out += seg_len;
        *out = '\0';
    }

    if (out == out_path) {
        out_path[0] = '/';
        out_path[1] = '\0';
    }
    return SYSCALL_SUCCESS;
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
    if (user_opts_size != sizeof(spawn_opts_t)) {
        return SYSCALL_EINVAL;
    }

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_opts_ptr, sizeof(spawn_opts_t), false)) {
        return SYSCALL_EFAULT;
    }

    spawn_opts_t opts;
    memcpy(&opts, (const void *)user_opts_ptr, sizeof(spawn_opts_t));

    if (opts.size != sizeof(spawn_opts_t) || opts.version != 1 || opts.flags != 0 ||
        opts.reserved0 != 0 || opts.reserved1 != 0 || opts.reserved2 != 0) {
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
        kcwd = cwd_buf;
    }

    int64_t pid = 0;
    int64_t result = process_spawn_from_vfs_ext(path, argc, kargv, envc, opts.envp ? kenvp : NULL, kcwd,
                                               opts.action_count, opts.action_count ? kactions : NULL, &pid);
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
    if (!process_wait_child(pid, &status)) return SYSCALL_ECHILD;
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
    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        if (!curr->fd_table[i]) {
            fd_avail = true;
            break;
        }
    }
    if (!fd_avail) {
        return SYSCALL_EMFILE;
    }

    int vfs_err = 0;
    file_t *file = vfs_open_ext(kpath, flags & ~VFS_O_CLOEXEC, &vfs_err);
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
    if (!curr || !curr->fd_table[fd]) {
        return SYSCALL_EBADF;
    }

    switch (cmd) {
        case F_GETFD:
            return (int64_t)curr->fd_flags[fd];

        case F_SETFD:
            if (arg & ~(uint64_t)FD_FLAG_CLOEXEC) {
                return SYSCALL_EINVAL;
            }
            curr->fd_flags[fd] = (uint8_t)arg;
            return SYSCALL_SUCCESS;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int min_fd = (int)arg;
            if (min_fd < 0 || min_fd >= MAX_PROCESS_FDS) {
                return SYSCALL_EINVAL;
            }
            for (int i = min_fd; i < MAX_PROCESS_FDS; i++) {
                if (!curr->fd_table[i]) {
                    curr->fd_table[i] = curr->fd_table[fd];
                    __atomic_fetch_add(&curr->fd_table[i]->ref_count, 1, __ATOMIC_ACQ_REL);
                    curr->fd_flags[i] = (cmd == F_DUPFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0;
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

    vfs_node_t *node = vfs_lookup(kpath);
    if (!node) {
        return SYSCALL_ENOENT;
    }

    vfs_stat_t st;
    vfs_stat(node, &st);
    memcpy((void *)user_statbuf, &st, sizeof(vfs_stat_t));
    return SYSCALL_SUCCESS;
}

static int64_t sys_dmesg(uintptr_t user_buf, uint64_t cap) {
    if (cap == 0) return 0;
    if (cap > DMESG_SIZE) cap = DMESG_SIZE;

    uint64_t *active_pml4 = vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(active_pml4, user_buf, cap, true)) {
        return SYSCALL_EFAULT;
    }

    return (int64_t)dmesg_read((char *)user_buf, (size_t)cap);
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

    int res = vfs_mkdir(kpath, (uint32_t)mode);
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

    int res = vfs_unlink(kpath);
    if (res < 0) return syscall_from_vfs_error(res);
    return SYSCALL_SUCCESS;
}

static int64_t sys_rename(uintptr_t user_oldpath, uintptr_t user_newpath) {
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

    int res = vfs_rename(koldpath, knewpath);
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

    vfs_node_t *node = vfs_lookup(resolved);
    if (!node) return SYSCALL_ENOENT;
    if (node->type != VFS_DIRECTORY) return SYSCALL_ENOTDIR;

    size_t rlen = strlen(resolved);
    if (rlen >= sizeof(curr->cwd)) return SYSCALL_EINVAL;
    memcpy(curr->cwd, resolved, rlen + 1);
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
    /* Flush the writable /mnt device via the durability barrier.
     * Returns SYSCALL_SUCCESS (0) on success; SYSCALL_EIO on failure or no RW mount. */
    bool ok = usb_mount_sync();
    if (!ok) {
        usb_report_flush_failure();
        return SYSCALL_EIO;
    }
    return SYSCALL_SUCCESS;
}

int64_t syscall_dispatch(interrupt_frame_t *frame) {
    if (!frame) return SYSCALL_EINVAL;

    uint64_t syscall_nr = frame->rax;
    int64_t result = SYSCALL_ENOSYS;

    switch (syscall_nr) {
        case SYS_SPAWN:
            result = sys_spawn(frame->rdi, frame->rsi);
            break;
        case SYS_TERMCTL:
            result = sys_termctl(frame->rdi, frame->rsi, frame->rdx);
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

        case SYS_DMESG:
            result = sys_dmesg(frame->rdi, frame->rsi);
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

        case SYS_MKDIR:
            result = sys_mkdir(frame->rdi, frame->rsi);
            break;

        case SYS_UNLINK:
            result = sys_unlink(frame->rdi);
            break;

        case SYS_RENAME:
            result = sys_rename(frame->rdi, frame->rsi);
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
