#include "ntp.h"
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void ntp_request(uint8_t p[48], const uint8_t nonce[8]) {
    for (int i = 0; i < 48; i++)
        p[i] = 0;
    p[0] = 0x23; /* version 4, client mode */
    for (int i = 0; i < 8; i++)
        p[40 + i] = nonce[i];
}
static int64_t decode(const uint8_t *p) {
    uint64_t sec = get32(p);
    /* Era 0 until wrap in 2036, era 1 thereafter. Unix 1970..2100. */
    if (sec < 2208988800ULL)
        sec += 4294967296ULL;
    sec -= 2208988800ULL;
    if (sec > 4133980799ULL)
        return -1;
    return (int64_t)sec * 1000000000LL + ((uint64_t)get32(p + 4) * 1000000000ULL >> 32);
}
bool ntp_sample(const uint8_t *p, size_t n, const uint8_t nonce[8], int64_t elapsed,
                timespec_t *out) {
    if (!p || !nonce || !out || n != 48 || elapsed < 0 || elapsed > 5000000000LL)
        return false;
    int version = p[0] >> 3 & 7;
    if ((p[0] >> 6) == 3 || (version != 3 && version != 4) || (p[0] & 7) != 4 || p[1] == 0 ||
        p[1] > 15)
        return false;
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++)
        diff |= p[24 + i] ^ nonce[i];
    if (diff)
        return false;
    if (get32(p + 8) > 16u * 65536u)
        return false; /* root dispersion */
    if (!(get32(p + 32) | get32(p + 36)) || !(get32(p + 40) | get32(p + 44)))
        return false;
    int64_t receive = decode(p + 32), transmit = decode(p + 40);
    if (receive < 0 || transmit < receive || transmit - receive > elapsed)
        return false;
    int64_t now = transmit + (elapsed - (transmit - receive)) / 2;
    timespec_t sample = {now / 1000000000LL, now % 1000000000LL};
    if (sample.tv_sec > 4133980799LL)
        return false;
    *out = sample;
    return true;
}
