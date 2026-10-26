#ifndef FORTRESS_FB_H
#define FORTRESS_FB_H
#include "types.h"
#include "syscall_abi.h"
/* Caller holds its scheduler address-space lifetime pin. Root identity stays
 * unique until fb_space_destroyed runs before that root returns to PMM. */
int64_t fb_acquire(uintptr_t cr3);
int64_t fb_release(uintptr_t cr3);
int64_t fb_map(uintptr_t cr3, uint64_t *root, uintptr_t requested, fb_info_v1_t *out);
void fb_space_destroyed(uintptr_t cr3);
#endif
