#ifndef FORTRESS_PIPE_PROFILE_ABI_H
#define FORTRESS_PIPE_PROFILE_ABI_H
#include "types.h"
/* SYS_SPAWN_PROFILE actions on a live pipe descriptor. Shared endpoint
 * counters, serialized by the existing pipe lock; no transfer timestamps.
 * Waits count predicate wait attempts, not scheduler context switches.
 * Bytes include benchmark headers and trailers. CPU masks use logical IDs. */
#define PIPE_PROFILE_READ 6
#define PIPE_PROFILE_ENABLE 7
#define PIPE_PROFILE_DISABLE 8
typedef struct {
    uint64_t fd, valid;
    uint64_t reads, writes, read_bytes, write_bytes, read_max, write_max;
    uint64_t read_small, read_1k, read_large, write_small, write_1k, write_large;
    uint64_t read_waits, write_waits, empty_drains, empty_fills, full_fills;
    uint64_t reader_cpus, writer_cpus, direction_changes, cpu_changes;
} pipe_io_profile_t;
_Static_assert(sizeof(pipe_io_profile_t) == 184, "pipe diagnostic ABI size");
#endif
