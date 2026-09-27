#ifndef FORTRESS_SIGNAL_FRAME_H
#define FORTRESS_SIGNAL_FRAME_H
/*
 * FortressOS v1 signal frame ABI — shared between kernel and user code.
 *
 * Version-1 layout (224 bytes starting at F = align_down(S - 224, 16)):
 *
 *   Offset -8 (H = F - 8): handler return-address slot — USER_SIGRESTORER_VIRT
 *   Offset   0: version = 1
 *   Offset   8: size = 224
 *   Offset  16: frame_id (matches TCB active-frame entry generation)
 *   Offset  24: reserved = 0
 *   Offset  32: saved RAX
 *   Offset  40: saved RBX
 *   Offset  48: saved RCX
 *   Offset  56: saved RDX
 *   Offset  64: saved RSI
 *   Offset  72: saved RDI
 *   Offset  80: saved RBP
 *   Offset  88: saved R8
 *   Offset  96: saved R9
 *   Offset 104: saved R10
 *   Offset 112: saved R11
 *   Offset 120: saved R12
 *   Offset 128: saved R13
 *   Offset 136: saved R14
 *   Offset 144: saved R15
 *   Offset 152: saved interrupted RFLAGS (including DF)
 *   Offset 160: saved user RIP
 *   Offset 168: saved user RSP = S (interrupted stack pointer)
 *   Offset 176: saved user CS = 0x23
 *   Offset 184: saved user SS = 0x1b
 *   Offset 192: saved old blocked mask (pre-delivery)
 *   Offset 200: delivered signal number
 *   Offset 208: reserved padding, zero-filled (16 bytes)
 *   --- Total: 224 bytes ---
 *
 * Handler entry:
 *   RSP = H (F - 8), RIP = handler address, RDI = signal number.
 *   RSP is 8 mod 16 — exactly as after a SysV call instruction.
 *   handler's ret pops [H] -> RSP = F -> enters restorer with RSP 0 mod 16.
 *
 * Restorer stub at USER_SIGRESTORER_VIRT:
 *   mov rdi, rsp     ; RDI = F (address of the frame)
 *   mov eax, SYS_SIGRETURN
 *   syscall
 *   ; on error: SYS_EXIT with non-zero code (never returns via ret)
 */

#include "types.h"

/* Return-address slot sits 8 bytes before the frame base (at H = F - 8). */
#define SIGFRAME_RETURN_SLOT_OFFSET  (-8)

/* Version-1 frame size in bytes. */
#define SIGFRAME_V1_SIZE             224

/* Field offsets from the start of the frame (F). */
#define SIGFRAME_OFF_VERSION         0
#define SIGFRAME_OFF_SIZE            8
#define SIGFRAME_OFF_FRAME_ID        16
#define SIGFRAME_OFF_RESERVED0       24
#define SIGFRAME_OFF_RAX             32
#define SIGFRAME_OFF_RBX             40
#define SIGFRAME_OFF_RCX             48
#define SIGFRAME_OFF_RDX             56
#define SIGFRAME_OFF_RSI             64
#define SIGFRAME_OFF_RDI             72
#define SIGFRAME_OFF_RBP             80
#define SIGFRAME_OFF_R8              88
#define SIGFRAME_OFF_R9              96
#define SIGFRAME_OFF_R10             104
#define SIGFRAME_OFF_R11             112
#define SIGFRAME_OFF_R12             120
#define SIGFRAME_OFF_R13             128
#define SIGFRAME_OFF_R14             136
#define SIGFRAME_OFF_R15             144
#define SIGFRAME_OFF_RFLAGS          152
#define SIGFRAME_OFF_RIP             160
#define SIGFRAME_OFF_RSP             168
#define SIGFRAME_OFF_CS              176
#define SIGFRAME_OFF_SS              184
#define SIGFRAME_OFF_OLD_MASK        192
#define SIGFRAME_OFF_SIGNO           200
#define SIGFRAME_OFF_RESERVED1       208

/* C structure for the frame body (224 bytes, does NOT include the return slot). */
typedef struct signal_frame_v1 {
    uint64_t version;       /* 0x00:  must be 1 */
    uint64_t size;          /* 0x08:  must be 224 */
    uint64_t frame_id;      /* 0x10:  matches TCB active-frame generation */
    uint64_t reserved0;     /* 0x18:  must be 0 */
    uint64_t rax;           /* 0x20 */
    uint64_t rbx;           /* 0x28 */
    uint64_t rcx;           /* 0x30 */
    uint64_t rdx;           /* 0x38 */
    uint64_t rsi;           /* 0x40 */
    uint64_t rdi;           /* 0x48 */
    uint64_t rbp;           /* 0x50 */
    uint64_t r8;            /* 0x58 */
    uint64_t r9;            /* 0x60 */
    uint64_t r10;           /* 0x68 */
    uint64_t r11;           /* 0x70 */
    uint64_t r12;           /* 0x78 */
    uint64_t r13;           /* 0x80 */
    uint64_t r14;           /* 0x88 */
    uint64_t r15;           /* 0x90 */
    uint64_t rflags;        /* 0x98:  interrupted RFLAGS including DF */
    uint64_t rip;           /* 0xa0:  interrupted user RIP */
    uint64_t rsp;           /* 0xa8:  interrupted user RSP = S */
    uint64_t cs;            /* 0xb0:  must restore 0x23 */
    uint64_t ss;            /* 0xb8:  must restore 0x1b */
    uint64_t old_mask;      /* 0xc0:  blocked mask before delivery */
    uint64_t signo;         /* 0xc8:  delivered signal number */
    uint64_t reserved1[2];  /* 0xd0:  must be 0 (16 bytes padding) */
} signal_frame_v1_t;

/* Compile-time layout verification — single source of truth. */
_Static_assert(sizeof(signal_frame_v1_t) == SIGFRAME_V1_SIZE,
               "signal_frame_v1_t must be exactly 224 bytes");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, version)   == SIGFRAME_OFF_VERSION,  "frame version offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, size)      == SIGFRAME_OFF_SIZE,     "frame size offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, frame_id)  == SIGFRAME_OFF_FRAME_ID, "frame_id offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, reserved0) == SIGFRAME_OFF_RESERVED0,"frame reserved0 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rax)       == SIGFRAME_OFF_RAX,      "frame rax offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rbx)       == SIGFRAME_OFF_RBX,      "frame rbx offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rcx)       == SIGFRAME_OFF_RCX,      "frame rcx offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rdx)       == SIGFRAME_OFF_RDX,      "frame rdx offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rsi)       == SIGFRAME_OFF_RSI,      "frame rsi offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rdi)       == SIGFRAME_OFF_RDI,      "frame rdi offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rbp)       == SIGFRAME_OFF_RBP,      "frame rbp offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r8)        == SIGFRAME_OFF_R8,       "frame r8 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r9)        == SIGFRAME_OFF_R9,       "frame r9 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r10)       == SIGFRAME_OFF_R10,      "frame r10 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r11)       == SIGFRAME_OFF_R11,      "frame r11 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r12)       == SIGFRAME_OFF_R12,      "frame r12 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r13)       == SIGFRAME_OFF_R13,      "frame r13 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r14)       == SIGFRAME_OFF_R14,      "frame r14 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, r15)       == SIGFRAME_OFF_R15,      "frame r15 offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rflags)    == SIGFRAME_OFF_RFLAGS,   "frame rflags offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rip)       == SIGFRAME_OFF_RIP,      "frame rip offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, rsp)       == SIGFRAME_OFF_RSP,      "frame rsp offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, cs)        == SIGFRAME_OFF_CS,       "frame cs offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, ss)        == SIGFRAME_OFF_SS,       "frame ss offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, old_mask)  == SIGFRAME_OFF_OLD_MASK, "frame old_mask offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, signo)     == SIGFRAME_OFF_SIGNO,    "frame signo offset");
_Static_assert(__builtin_offsetof(signal_frame_v1_t, reserved1) == SIGFRAME_OFF_RESERVED1,"frame reserved1 offset");

#endif /* FORTRESS_SIGNAL_FRAME_H */
