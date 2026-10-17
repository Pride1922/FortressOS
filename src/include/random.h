#ifndef FORTRESS_RANDOM_H
#define FORTRESS_RANDOM_H
#include "types.h"
#include "boot_info.h"

/* Boot-only, before scheduler publication. No reinitialization on spawn/exec.
 * Ordinary Rank-1 exclusion; no allocation, logging, user copy or scheduling
 * under the lock. Hardware retries are finite, with no timer dependency. */
void random_init(const boot_info_t *boot, uint64_t epoch);
bool random_ready(void);
int64_t random_get(void *out, size_t length);
int64_t random_user(uintptr_t address, size_t length, uint64_t flags);
typedef struct {
    uint64_t bytes, reseeds, hardware_failures;
    bool ready;
} random_stats_t;
void random_stats(random_stats_t *out);
void random_wipe(void *p, size_t n);
#endif
