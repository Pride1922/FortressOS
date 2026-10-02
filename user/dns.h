#ifndef FORTRESS_USER_DNS_H
#define FORTRESS_USER_DNS_H
#include "types.h"
#define DNS_CONTEXT_WORDS 2048u
#define DNS_NAME_CAP 256u
#define DNS_ADDRESS_MAX 8u
#define DNS_RESULT_NUMERIC 1u
#define DNS_RESULT_TCP 2u
enum { DNS_OK, DNS_INVALID, DNS_BUSY, DNS_TIMEOUT, DNS_INTERRUPTED,
    DNS_NXDOMAIN, DNS_NO_DATA, DNS_SERVER_FAILURE, DNS_MALFORMED, DNS_LIMIT,
    DNS_IO, DNS_UNSUPPORTED, DNS_TCP_QUIET };
typedef struct { uint64_t private_words[DNS_CONTEXT_WORDS]; } dns_context_t;
typedef struct { uint32_t server_ipv4, reserved; uint64_t deadline_ticks; } dns_options_t;
typedef struct {
    char canonical_name[DNS_NAME_CAP];
    uint32_t addresses[DNS_ADDRESS_MAX];
    uint32_t address_count, flags;
} dns_result_t;
_Static_assert(sizeof(dns_context_t)==16384 && _Alignof(dns_context_t)==8,"DNS context");
_Static_assert(sizeof(dns_options_t)==16 && _Alignof(dns_options_t)==8,"DNS options");
_Static_assert(offsetof(dns_options_t,server_ipv4)==0 && offsetof(dns_options_t,reserved)==4,"DNS option offsets");
_Static_assert(offsetof(dns_options_t,deadline_ticks)==8,"DNS deadline offset");
_Static_assert(sizeof(dns_result_t)==296 && _Alignof(dns_result_t)==4,"DNS result");
_Static_assert(offsetof(dns_result_t,addresses)==256 && offsetof(dns_result_t,address_count)==288 && offsetof(dns_result_t,flags)==292,"DNS result offsets");
void dns_context_init(dns_context_t *ctx);
int dns_resolve_ipv4(dns_context_t *ctx, const dns_options_t *options,
    const char *name, size_t name_length, dns_result_t *result);
int64_t dns_last_syscall_error(const dns_context_t *ctx);
const char *dns_status_name(int status);
#endif
