#ifndef USER_WGET_H
#define USER_WGET_H

#include "types.h"

int wget_run(const char *raw_url, const char *custom_out, bool quiet,
             uint32_t dns_override, bool has_dns_override);

#endif
