#include "crash_dump.h"
#include "string.h"
#include "serial.h"
#include "dmesg.h"
#include "signal_abi.h"
#include "spinlock.h"
#include "percpu.h"
#include "thread.h"
#include "vmm.h"

unsigned crash_dump_vector_to_signal(uint64_t vector) {
    switch (vector) {
        case 0:  /* Divide-by-zero (#DE) */
        case 4:  /* Overflow (#OF) */
        case 7:  /* Device Not Available / FPU (#NM) */
        case 16: /* x87 FPU Floating-Point (#MF) */
        case 19: /* SIMD Floating-Point (#XM) */
            return SIGFPE;

        case 6:  /* Invalid Opcode (#UD) */
            return SIGILL;

        case 11: /* Segment Not Present (#NP) */
        case 12: /* Stack Segment Fault (#SS) */
        case 17: /* Alignment Check (#AC) */
            return SIGBUS;

        case 5:  /* Bound Range Exceeded (#BR) */
        case 13: /* General Protection Fault (#GP) */
        case 14: /* Page Fault (#PF) */
        default:
            return SIGSEGV;
    }
}

const char *crash_dump_vector_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "#DE (Divide Error)";
        case 1:  return "#DB (Debug)";
        case 2:  return "#NMI (Non-Maskable Interrupt)";
        case 3:  return "#BP (Breakpoint Trap)";
        case 4:  return "#OF (Overflow Trap)";
        case 5:  return "#BR (Bound Range Exceeded)";
        case 6:  return "#UD (Invalid Opcode)";
        case 7:  return "#NM (Device Not Available / FPU)";
        case 8:  return "#DF (Double Fault)";
        case 10: return "#TS (Invalid TSS)";
        case 11: return "#NP (Segment Not Present)";
        case 12: return "#SS (Stack-Segment Fault)";
        case 13: return "#GP (General Protection Fault)";
        case 14: return "#PF (Page Fault)";
        case 16: return "#MF (x87 Floating-Point Exception)";
        case 17: return "#AC (Alignment Check)";
        case 18: return "#MC (Machine Check)";
        case 19: return "#XM (SIMD Floating-Point Exception)";
        default: return "Unknown Exception";
    }
}

const char *crash_dump_signal_name(unsigned sig) {
    switch (sig) {
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGSEGV: return "SIGSEGV";
        default:      return "SIGUNKNOWN";
    }
}

static void buf_append(char *dst, size_t dst_cap, size_t *pos, const char *src) {
    if (!dst || !pos || *pos >= dst_cap - 1 || !src) return;
    while (*src && *pos < dst_cap - 1) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
}

void crash_dump_format_pf_error(uint64_t error_code, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';
    size_t pos = 0;

    buf_append(buf, buf_len, &pos, "[");
    buf_append(buf, buf_len, &pos, (error_code & 1) ? "prot-violation" : "not-present");
    buf_append(buf, buf_len, &pos, (error_code & 2) ? ", write" : ", read");
    if (error_code & 4)  buf_append(buf, buf_len, &pos, ", user");
    if (error_code & 8)  buf_append(buf, buf_len, &pos, ", rsvd");
    if (error_code & 16) buf_append(buf, buf_len, &pos, ", exec/nx");
    if (error_code & 32) buf_append(buf, buf_len, &pos, ", pk");
    if (error_code & 64) buf_append(buf, buf_len, &pos, ", shadow-stack");
    buf_append(buf, buf_len, &pos, "]");
}

void crash_dump_format_rflags(uint64_t rflags, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';
    size_t pos = 0;

    buf_append(buf, buf_len, &pos, "[");
    bool first = true;
    #define CHECK_FLAG(bit, name) \
        if (rflags & (1ULL << (bit))) { \
            if (!first) buf_append(buf, buf_len, &pos, " "); \
            buf_append(buf, buf_len, &pos, name); \
            first = false; \
        }
    CHECK_FLAG(0, "CF")
    CHECK_FLAG(2, "PF")
    CHECK_FLAG(4, "AF")
    CHECK_FLAG(6, "ZF")
    CHECK_FLAG(7, "SF")
    CHECK_FLAG(8, "TF")
    CHECK_FLAG(9, "IF")
    CHECK_FLAG(10, "DF")
    CHECK_FLAG(11, "OF")
    #undef CHECK_FLAG
    buf_append(buf, buf_len, &pos, "]");
}

/* Internal formatted log emitter to COM1 and kernel dmesg ring buffer */
static void crash_log(const char *s) {
    if (!s) return;
    serial_raw_puts(s);
    dmesg_append_str(s, strlen(s));
}

static void crash_log_hex64(uint64_t val) {
    const char hex_chars[] = "0123456789abcdef";
    char str[19];
    str[0] = '0';
    str[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        str[2 + i] = hex_chars[(val >> shift) & 0x0F];
    }
    str[18] = '\0';
    crash_log(str);
}

static void crash_log_hex8(uint8_t val) {
    const char hex_chars[] = "0123456789abcdef";
    char str[3];
    str[0] = hex_chars[(val >> 4) & 0x0F];
    str[1] = hex_chars[val & 0x0F];
    str[2] = '\0';
    crash_log(str);
}

static void crash_log_dec(uint64_t val) {
    char buf[24];
    size_t idx = 0;
    if (val == 0) {
        crash_log("0");
        return;
    }
    while (val > 0) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t i = 0; i < idx / 2; i++) {
        char tmp = buf[i];
        buf[i] = buf[idx - 1 - i];
        buf[idx - 1 - i] = tmp;
    }
    buf[idx] = '\0';
    crash_log(buf);
}

void crash_dump_user(interrupt_frame_t *frame) {
    if (!frame || (frame->cs & 3) != 3) return;

    /* Serialize concurrent core dumps across SMP cores */
    static spinlock_t s_crash_lock = SPINLOCK_RANKED(5, "user_crash_dump");
    uint64_t irq_state = spin_lock_irqsave(&s_crash_lock);

    tcb_t *curr = thread_current();
    uint64_t pid = 0;
    uint64_t tid = 0;
    const char *pname = "unknown";
    uint64_t *pml4 = NULL;

    if (curr) {
        tid = curr->tid;
        pname = curr->name[0] ? curr->name : "unnamed";
        pml4 = curr->pml4_virt;
        if (curr->process) {
            pid = curr->process->pid;
            if (!pml4) pml4 = curr->process->pml4_virt;
        } else {
            pid = curr->tid;
        }
    }

    uint64_t vector = frame->vector;
    unsigned signo = crash_dump_vector_to_signal(vector);
    const char *vec_name = crash_dump_vector_name(vector);
    const char *sig_name = crash_dump_signal_name(signo);
    size_t cid = cpu_current()->id;

    uint64_t cr2 = 0;
    if (vector == 14) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    /* 1. Header Banner */
    crash_log("\n[CRASH] ==================== USER SPACE CRASH DUMP ====================\n");
    crash_log("[CRASH] Process:     ");
    crash_log(pname);
    crash_log(" (PID: ");
    crash_log_dec(pid);
    crash_log(", TID: ");
    crash_log_dec(tid);
    crash_log(", CPU: ");
    crash_log_dec(cid);
    crash_log(")\n");

    crash_log("[CRASH] Signal:      ");
    crash_log(sig_name);
    crash_log(" (");
    crash_log_dec(signo);
    crash_log(") - Fault: ");
    crash_log(vec_name);
    crash_log(" (Vector ");
    crash_log_dec(vector);
    crash_log(")\n");

    if (vector == 14) {
        crash_log("[CRASH] Fault Addr:  CR2 = ");
        crash_log_hex64(cr2);
        crash_log("\n");
        char pf_decoded[64];
        crash_dump_format_pf_error(frame->error_code, pf_decoded, sizeof(pf_decoded));
        crash_log("[CRASH] Error Code:  ");
        crash_log_hex64(frame->error_code);
        crash_log(" ");
        crash_log(pf_decoded);
        crash_log("\n");
    } else {
        crash_log("[CRASH] Error Code:  ");
        crash_log_hex64(frame->error_code);
        crash_log("\n");
    }

    /* 2. Architectural Registers */
    crash_log("[CRASH]\n[CRASH] --- Architectural Registers ---\n");
    crash_log("[CRASH] RAX: "); crash_log_hex64(frame->rax);
    crash_log("  RBX: ");     crash_log_hex64(frame->rbx);
    crash_log("  RCX: ");     crash_log_hex64(frame->rcx);
    crash_log("\n[CRASH] RDX: "); crash_log_hex64(frame->rdx);
    crash_log("  RSI: ");     crash_log_hex64(frame->rsi);
    crash_log("  RDI: ");     crash_log_hex64(frame->rdi);
    crash_log("\n[CRASH] RBP: "); crash_log_hex64(frame->rbp);
    crash_log("  RSP: ");     crash_log_hex64(frame->rsp);
    crash_log("  R8:  ");     crash_log_hex64(frame->r8);
    crash_log("\n[CRASH] R9:  "); crash_log_hex64(frame->r9);
    crash_log("  R10: ");     crash_log_hex64(frame->r10);
    crash_log("  R11: ");     crash_log_hex64(frame->r11);
    crash_log("\n[CRASH] R12: "); crash_log_hex64(frame->r12);
    crash_log("  R13: ");     crash_log_hex64(frame->r13);
    crash_log("  R14: ");     crash_log_hex64(frame->r14);
    crash_log("\n[CRASH] R15: "); crash_log_hex64(frame->r15);
    crash_log("  RIP: ");     crash_log_hex64(frame->rip);
    crash_log("\n[CRASH] RFLAGS: "); crash_log_hex64(frame->rflags);
    char rflags_buf[48];
    crash_dump_format_rflags(frame->rflags, rflags_buf, sizeof(rflags_buf));
    crash_log(" "); crash_log(rflags_buf);
    crash_log("  CS: "); crash_log_hex64(frame->cs);
    crash_log("  SS: "); crash_log_hex64(frame->ss);
    crash_log("\n");

    /* 3. Bounded Code Window at RIP (16 bytes) */
    crash_log("[CRASH]\n[CRASH] --- Code at RIP ---\n");
    if (pml4 && vmm_validate_user_range(pml4, frame->rip, CRASH_DUMP_CODE_BYTES, false)) {
        const uint8_t *code = (const uint8_t *)frame->rip;
        crash_log("[CRASH] ");
        crash_log_hex64(frame->rip);
        crash_log(": ");
        for (size_t i = 0; i < CRASH_DUMP_CODE_BYTES; i++) {
            crash_log_hex8(code[i]);
            if (i + 1 < CRASH_DUMP_CODE_BYTES) crash_log(" ");
        }
        crash_log("\n");
    } else {
        crash_log("[CRASH] <instruction address unmapped or inaccessible>\n");
    }

    /* 4. Bounded User Stack Window at RSP (16 QWORDs) */
    crash_log("[CRASH]\n[CRASH] --- Stack Window (RSP: ");
    crash_log_hex64(frame->rsp);
    crash_log(") ---\n");

    if (!pml4) {
        crash_log("[CRASH] <address space inaccessible>\n");
    } else {
        uintptr_t sp = frame->rsp;
        bool valid_any = false;
        for (size_t i = 0; i < CRASH_DUMP_STACK_QWORDS; i++) {
            uintptr_t addr = sp + (i * sizeof(uint64_t));
            if (vmm_validate_user_range(pml4, addr, sizeof(uint64_t), false)) {
                valid_any = true;
                crash_log("[CRASH] [RSP+0x");
                crash_log_hex8((uint8_t)(i * sizeof(uint64_t)));
                crash_log("] (");
                crash_log_hex64(addr);
                crash_log("): ");
                uint64_t val = *(const uint64_t *)addr;
                crash_log_hex64(val);
                crash_log("\n");
            } else {
                crash_log("[CRASH] [RSP+0x");
                crash_log_hex8((uint8_t)(i * sizeof(uint64_t)));
                crash_log("] <unmapped page boundary reached>\n");
                break;
            }
        }
        if (!valid_any) {
            crash_log("[CRASH] <user stack unmapped or corrupted>\n");
        }
    }

    /* 5. Footer & Termination Action */
    crash_log("[CRASH] ===============================================================\n");
    crash_log("[CRASH] Action: Terminating process PID ");
    crash_log_dec(pid);
    crash_log(" with exit status ");
    crash_log_dec(128 + signo);
    crash_log(" (kernel remains operational)\n\n");

    if (curr) {
        curr->exit_signal = signo;
    }

    spin_unlock_irqrestore(&s_crash_lock, irq_state);

    /* Clean process termination */
    process_exit(128 + signo);
    for (;;) { __asm__ volatile("cli; hlt"); }
}
