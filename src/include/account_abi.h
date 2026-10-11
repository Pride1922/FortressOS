#ifndef FORTRESS_ACCOUNT_ABI_H
#define FORTRESS_ACCOUNT_ABI_H
#include "types.h"
#define SYS_ACCOUNTCTL 72
#define SYS_INSTALLSEED 73
#define ACCOUNT_SNAPSHOT 0u
#define ACCOUNT_BEGIN 1u
#define ACCOUNT_CHECK 2u
#define ACCOUNT_FINISH 3u
#define ACCOUNT_RECOVER 4u /* never admitted from Ring 3 */
#define ACCOUNT_UPDATE 5u /* complete copied set, kernel owns redo publication */
#define ACCOUNT_STATUS 6u /* installed namespace identity, even with auth off */
#define ACCOUNT_WITH_SHADOW 1u
typedef struct {
    uint32_t size,version;
    uint64_t generation,token,passwd,group,shadow;
    uint32_t passwd_bytes,group_bytes,shadow_bytes,flags;
} account_request_t;
_Static_assert(sizeof(account_request_t)==64,"account request ABI");
_Static_assert(offsetof(account_request_t,passwd)==24,"account buffers ABI");
_Static_assert(offsetof(account_request_t,flags)==60,"account flags ABI");
#endif
