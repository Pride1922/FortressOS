#ifndef FORTRESS_PKG_BOOT_CLI_H
#define FORTRESS_PKG_BOOT_CLI_H
#include "boot_info.h"
/* Boot-only after independent bootstrap/readiness verification, before root
 * preparation/publication. Exposes only immutable /bin/pkg as /run/pkg; no
 * enrollment, keys, backend authority or installed filesystem writes. */
int pkg_boot_cli_publish(const boot_info_t *);
#endif
