#include "syscall.h"
#include "vmm.h"
#include "serial.h"
#include "gdt.h"
#include "thread.h"
#include "msr.h"

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

static int64_t sys_write(uint64_t fd, uintptr_t user_buf, size_t count) {
    /* 1. Validate file descriptor: standard output (1) or standard error (2) */
    if (fd != 1 && fd != 2) {
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

    /* 5. Memory is verified present and user-readable. Emit to serial */
    const char *ptr = (const char *)user_buf;
    for (size_t i = 0; i < count; i++) {
        serial_putc(ptr[i]);
    }

    return (int64_t)count;
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

    /* 2. Validate Return RSP against canonical lower-half user address space */
    if (frame->rsp < USER_CANONICAL_MIN || frame->rsp > USER_CANONICAL_LIMIT) {
        serial_puts("[SYSCALL] Hardening violation: non-canonical or kernel return RSP: ");
        serial_print_hex(frame->rsp);
        serial_puts("\n");
        return false;
    }

    /* 3. Sanitize RFLAGS: enforce user mask, force IF=1 and bit 1=1, clear IOPL/NT/TF/VM */
    frame->rflags = (frame->rflags & USER_RFLAGS_ALLOWED_MASK) | USER_RFLAGS_FORCED;

    return true;
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
