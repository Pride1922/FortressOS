#include "sshd_admission.h"
#include "syscall_abi.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <unistd.h>
#endif

#ifndef SSHD_SYSCALL
static inline long sshd_admission_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                          uintptr_t d, uintptr_t e, uintptr_t f) {
#ifndef CRYPTO_HOST_TEST
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
#else
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -1;
#endif
}
#define SSHD_SYSCALL sshd_admission_syscall
#endif

int sshd_format_disconnect_packet(uint32_t reason, const char *msg,
                                  uint8_t *out_wire, size_t wire_cap) {
    if (!out_wire) return -1;
    size_t msg_len = msg ? strlen(msg) : 0;

    /* RFC 4253 §11.1 SSH_MSG_DISCONNECT payload:
     * byte      SSH_MSG_DISCONNECT (1)
     * uint32    reason code (big-endian)
     * string    description (uint32 length + text)
     * string    language tag (uint32 length = 0)
     */
    size_t payload_len = 1 + 4 + (4 + msg_len) + 4;
    size_t unpadded = 4 + 1 + payload_len;
    size_t pad = 8 - (unpadded % 8);
    if (pad < 4) pad += 8;

    uint32_t packet_len = (uint32_t)(1 + payload_len + pad);
    size_t total_wire = 4 + packet_len;
    if (total_wire > wire_cap) return -1;

    /* packet_length (u32 big-endian) */
    out_wire[0] = (uint8_t)((packet_len >> 24) & 0xff);
    out_wire[1] = (uint8_t)((packet_len >> 16) & 0xff);
    out_wire[2] = (uint8_t)((packet_len >> 8) & 0xff);
    out_wire[3] = (uint8_t)(packet_len & 0xff);

    /* padding_length */
    out_wire[4] = (uint8_t)pad;

    /* SSH_MSG_DISCONNECT */
    out_wire[5] = (uint8_t)SSH_MSG_DISCONNECT;

    /* reason code (u32 big-endian) */
    out_wire[6] = (uint8_t)((reason >> 24) & 0xff);
    out_wire[7] = (uint8_t)((reason >> 16) & 0xff);
    out_wire[8] = (uint8_t)((reason >> 8) & 0xff);
    out_wire[9] = (uint8_t)(reason & 0xff);

    /* description length (u32 big-endian) */
    out_wire[10] = (uint8_t)((msg_len >> 24) & 0xff);
    out_wire[11] = (uint8_t)((msg_len >> 16) & 0xff);
    out_wire[12] = (uint8_t)((msg_len >> 8) & 0xff);
    out_wire[13] = (uint8_t)(msg_len & 0xff);

    /* description string */
    if (msg_len > 0) {
        memcpy(out_wire + 14, msg, msg_len);
    }
    size_t off = 14 + msg_len;

    /* language tag length = 0 (empty string) */
    out_wire[off++] = 0;
    out_wire[off++] = 0;
    out_wire[off++] = 0;
    out_wire[off++] = 0;

    /* padding bytes (deterministic counter pattern) */
    for (size_t i = 0; i < pad; i++) {
        out_wire[off++] = (uint8_t)(i + 1);
    }

    return (int)total_wire;
}

void sshd_reject_connection_capacity(long client_fd, const char *version_str) {
    if (client_fd < 0) return;

    if (version_str) {
        size_t vlen = strlen(version_str);
#ifndef CRYPTO_HOST_TEST
        (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)version_str, vlen, 0, 0, 0);
#else
        ssize_t nw = write((int)client_fd, version_str, vlen);
        (void)nw;
#endif
    }

    uint8_t pkt[128];
    int pkt_len = sshd_format_disconnect_packet(SSH_DISCONNECT_BY_APPLICATION,
                                                "Too many concurrent sessions",
                                                pkt, sizeof(pkt));
    if (pkt_len > 0) {
#ifndef CRYPTO_HOST_TEST
        (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)pkt, (size_t)pkt_len, 0, 0, 0);
#else
        ssize_t nw = write((int)client_fd, pkt, (size_t)pkt_len);
        (void)nw;
#endif
    }

#ifndef CRYPTO_HOST_TEST
    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)client_fd, 0, 0, 0, 0, 0);
#else
    close((int)client_fd);
#endif
}

int sshd_admission_find_slot(const sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS]) {
    if (!slots) return -1;
    for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
        if (!slots[i].active) {
            return i;
        }
    }
    return -1;
}
