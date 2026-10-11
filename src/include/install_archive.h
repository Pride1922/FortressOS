#ifndef FORTRESS_INSTALL_ARCHIVE_H
#define FORTRESS_INSTALL_ARCHIVE_H
#include "types.h"
#define INSTALL_ARCHIVE_MAX (8u*1024*1024)
/* Strict local USTAR input: <=510 entries, no aliases, duplicate names,
 * non-root entries, links, devices, old bootstrap credentials or hidden trailer.
 * Output is caller-owned, non-overlapping and zeroed before use. Credentials
 * must already be a valid hash-only Phase2 recovery record; no secret input.
 * Success returns exact length with two zero blocks. Failure returns -errno,
 * zero length and wipes the entire output capacity after disjoint valid buffer
 * admission. Invalid/overlapping buffers are untouched. No I/O/allocation. */
int install_archive_personalize(const void *,size_t,const void *,size_t,
                               void *,size_t,size_t *);
#endif
