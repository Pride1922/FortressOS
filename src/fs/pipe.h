#ifndef FORTRESS_PIPE_H
#define FORTRESS_PIPE_H

#include "types.h"
#include "spinlock.h"
#include "vfs.h"
#include "pipe_profile_abi.h"

#define PIPE_CAPACITY (64 * 1024)
#define PIPE_BUF 4096

typedef struct pipe {
    spinlock_t lock;
    uintptr_t buffer_phys;
    uint8_t *buffer;
    size_t head, tail, count;
    _Atomic uint32_t readers, writers;
    _Atomic uint32_t data_bytes, space_bytes;
    _Atomic uint32_t active_endpoints;
    vfs_node_t *read_node, *write_node;
    bool profile_enabled;
    uint8_t profile_direction, profile_cpu;
    pipe_io_profile_t profile;
} pipe_t;

/* Transfers ownership of two anonymous nodes to the caller. Attach each to
 * exactly one file_t; dup/spawn share that file_t. Close both on rollback.
 * Transfers block on distinct data/space channels through the CPU-local
 * scheduler; wakeups scan all CPUs and notify remote runnable peers by IPI.
 * Transfers wake all peers in the affected direction; endpoint close wakes
 * both directions while retaining the endpoint's lifetime reference.
 * Writes larger than PIPE_BUF return the available positive prefix immediately;
 * they do not wait again after copying. No readers returns -VFS_EPIPE with all
 * locks released. SYS_WRITE publishes SIGPIPE to its user writer on that error;
 * direct kernel VFS callers retain the error-only contract. */
int pipe_create(vfs_node_t **out_read_node, vfs_node_t **out_write_node);
void pipe_close_endpoint(vfs_node_t *node);
int pipe_profile(vfs_node_t *node, uint64_t action, pipe_io_profile_t *out);

#endif
