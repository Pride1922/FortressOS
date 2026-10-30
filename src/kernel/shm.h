#ifndef FORTRESS_SHM_H
#define FORTRESS_SHM_H
#include "types.h"

/* Anonymous shared memory objects (SYS_SHM_CREATE/MAP/UNMAP).
 *
 * An object is a set of zeroed PMM frames behind an ordinary file
 * descriptor, so it is shared by fd inheritance or spawn DUP2 actions and
 * needs no new handle type. Frames are mapped as PTE_DEVICE leaves: an
 * address space never frees them, and syscalls refuse them as copy buffers
 * (a concurrent unmap can never pull memory out from under a kernel copy).
 *
 * Lifetime: one reference for the (single) file_t node plus one per live
 * mapping. Frames return to the PMM only when both the last descriptor is
 * closed and the last mapping is gone, and only after the unmapping TLB
 * shootdown (or address-space teardown) completed. */

#define SHM_MAX_BYTES        (32ULL * 1024 * 1024)
#define SHM_TOTAL_BYTES_MAX  (256ULL * 1024 * 1024)
#define SHM_MAX_MAPPINGS     8

int64_t shm_create(uint64_t bytes, uint32_t flags);
int64_t shm_map(int fd, uintptr_t va, uint32_t flags);
int64_t shm_unmap(uintptr_t va);
/* Called by vmm teardown after every leaf of the root is gone and no CPU
 * can use it, before the root frame returns to the PMM. */
void shm_space_destroyed(uintptr_t root_phys);

#endif
