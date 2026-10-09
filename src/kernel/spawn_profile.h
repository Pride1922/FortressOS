#ifndef FORTRESS_SPAWN_PROFILE_H
#define FORTRESS_SPAWN_PROFILE_H
#include "spawn_profile_abi.h"

static inline uint64_t spawn_profile_clock(const spawn_profile_t *profile) {
    if (!profile) return 0;
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline void spawn_profile_add(spawn_profile_t *profile, uint64_t *counter,
                                     uint64_t begin) {
    if (!profile) return;
    uint64_t end = spawn_profile_clock(profile);
    if (end < begin || UINT64_MAX - *counter < end - begin) {
        profile->valid = 0;
        return;
    }
    *counter += end - begin;
}
/* The macro avoids evaluating a counter through a NULL disabled profile. */
#define SPAWN_ADD(p, member, begin) do { \
    if (p) spawn_profile_add((p), &(p)->member, (begin)); \
} while (0)
#endif
