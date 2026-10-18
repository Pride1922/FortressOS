#ifndef FORTRESS_NTP_H
#define FORTRESS_NTP_H
#include "syscall_abi.h"
/* SNTP one-server sample. Caller checks source IPv4/port and owns nonce. */
void ntp_request(uint8_t packet[48], const uint8_t nonce[8]);
bool ntp_sample(const uint8_t *packet, size_t length, const uint8_t nonce[8], int64_t elapsed_ns,
                timespec_t *utc);
#endif
