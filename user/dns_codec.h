#ifndef FORTRESS_USER_DNS_CODEC_H
#define FORTRESS_USER_DNS_CODEC_H
#include "dns.h"
/* Private workspace is addressed through character storage, never type-punned.
 * Wire, query and visited-name regions are retained across transactions. */
#define DNS_WIRE 32u
#define DNS_QUERY 4128u
#define DNS_CURRENT 4400u
#define DNS_OWNER 4656u
#define DNS_TARGET 4912u
#define DNS_VISITED 5168u
#define DNS_STAGE 7472u
#define DNS_RRS 7776u
#define DNS_PRIVATE_END (DNS_RRS+64u*16u)
#define DNS_TRUNCATED 100
#define DNS_UNMATCHED 101
_Static_assert(DNS_PRIVATE_END<=sizeof(dns_context_t),"DNS workspace fits");
void dns_copy(void *dst, const void *src, size_t n);
void dns_zero(void *dst, size_t n);
size_t dns_length(const char *s);
bool dns_equal(const char *a, const char *b);
int dns_normalize(const char *name, size_t n, char out[256]);
int dns_encode(const char *name, uint16_t id, uint8_t *wire, size_t *len);
/* DNS_BUSY is private continuation: CNAME progressed but another query needed.
 * hop/visited budget is resolution-wide. Result storage is character-addressed. */
int dns_decode(uint8_t *workspace, size_t len, uint16_t id,
    unsigned *hops, unsigned *visited);
bool dns_ipv4(const char *name, size_t len, uint32_t *ip);
void dns_format_ipv4(uint32_t ip, char out[16]);
#endif
