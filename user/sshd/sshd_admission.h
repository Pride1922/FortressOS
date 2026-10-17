#ifndef USER_SSHD_ADMISSION_H
#define USER_SSHD_ADMISSION_H

#include "types.h"
#include "ssh_packet.h"

#define SSHD_MAX_CONCURRENT_SESSIONS 2

typedef struct {
    bool    active;
    int64_t pid;
    int     p_mon_in;   /* Monitor reads worker requests */
    int     p_mon_out;  /* Monitor sends replies */
} sshd_worker_slot_t;

/* Format an unencrypted RFC 4253 SSH_MSG_DISCONNECT packet with reason code and message.
 * Returns wire length written to out_wire, or < 0 on buffer overflow.
 */
int sshd_format_disconnect_packet(uint32_t reason, const char *msg,
                                  uint8_t *out_wire, size_t wire_cap);

/* Send version banner and SSH_MSG_DISCONNECT packet, then close client descriptor */
void sshd_reject_connection_capacity(long client_fd, const char *version_str);

/* Admission manager: finds a free slot among global slots, or returns -1 if capacity reached */
int sshd_admission_find_slot(const sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS]);

#endif /* USER_SSHD_ADMISSION_H */
