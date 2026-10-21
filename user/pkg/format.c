#include "format.h"

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p+4)<<32;
}
int fpk_identity_valid(const uint8_t *p, size_t n) {
    if (!p || !n || n>32) return 0;
    for (size_t i=0;i<n;i++) {
        if ((p[i]>='a' && p[i]<='z') || (p[i]>='0' && p[i]<='9')) continue;
        if (p[i]!='-' || !i || i+1==n || p[i-1]=='-') return 0;
    }
    return 1;
}
int fpk_parse_envelope(const uint8_t *p, size_t n, fpk_envelope *out) {
    static const uint8_t magic[8]={'F','O','R','T','F','P','K',0};
    if (!p || !out || n!=FPK_HEADER_SIZE) return -1;
    for (size_t i=0;i<8;i++) if (p[i]!=magic[i]) return -1;
    /* version, header size, flags, manifest size, reserved, lengths, hashes */
    if (le32(p+8)!=1 || le32(p+12)!=128 || le32(p+16) || le32(p+24) || le32(p+28)) return -1;
    for (size_t i=112;i<128;i++) if (p[i]) return -1;
    uint32_t m=le32(p+20);
    uint64_t c=le64(p+32), e=le64(p+40);
    if (!m || m>FPK_MANIFEST_MAX || !c || c>FPK_COMPRESSED_MAX || !e || e>FPK_EXPANDED_MAX) return -1;
    out->manifest_bytes=m;
    out->compressed_bytes=c;
    out->expanded_bytes=e;
    out->container_bytes=FPK_HEADER_SIZE+(uint64_t)m+c;
    for (size_t i=0;i<32;i++) {
        out->manifest_sha256[i]=p[48+i];
        out->payload_sha256[i]=p[80+i];
    }
    return 0;
}
