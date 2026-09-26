#ifndef FORTRESS_PIPE_H
#define FORTRESS_PIPE_H

#include "types.h"
#include "spinlock.h"
#include "vfs.h"

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
} pipe_t;

/* Transfers ownership of two anonymous nodes to the caller. Attach each to
 * exactly one file_t; dup/spawn share that file_t. Close both on rollback.
 * Transfers block through CPU-local scheduler wait channels. All peers must
 * remain on the same CPU until cross-CPU channel wakeup support is added. */
int pipe_create(vfs_node_t **out_read_node, vfs_node_t **out_write_node);
void pipe_close_endpoint(vfs_node_t *node);

#endif
