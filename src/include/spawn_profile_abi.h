#ifndef FORTRESS_SPAWN_PROFILE_ABI_H
#define FORTRESS_SPAWN_PROFILE_ABI_H
#include "types.h"

/* Self-only diagnostics. Elapsed TSC cycles include blocking/preemption.
 * ELF subphases are contained in SP_ELF and must not be added to it. */
enum spawn_phase { SP_FILE, SP_REAP, SP_ELF, SP_USTACK, SP_KSTACK,
                   SP_TCB, SP_FDS, SP_PUBLISH, SP_CLEANUP, SP_COUNT };
enum spawn_elf_phase { SE_SPACE, SE_ALLOC, SE_MAP, SE_COPY, SE_COUNT };
#define SPAWN_PROFILE_READ 0
#define SPAWN_PROFILE_ENABLE 1 /* Reset calling task's counters and enable. */
#define SPAWN_PROFILE_DISABLE 2 /* Snapshot then disable, preserving counters. */
typedef struct {
    uint64_t total, calls, failures, valid;
    uint64_t phase[SP_COUNT];
    uint64_t elf[SE_COUNT];
    uint64_t queued_cycles, first_run_cycles, birth_valid;
} spawn_profile_t;
_Static_assert(sizeof(spawn_profile_t) == 160, "spawn diagnostic ABI size");
#endif
