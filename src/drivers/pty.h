#ifndef FORTRESS_PTY_H
#define FORTRESS_PTY_H

#include "types.h"
#include "vfs.h"
#include "spinlock.h"
#include "terminal.h"
#include "process_table.h"

#define PTY_IN_CAPACITY   4096
#define PTY_OUT_CAPACITY  16384

typedef struct {
    spinlock_t lock; /* Rank 2 Resource Lock */
    
    /* Input ring buffer: Master write -> Slave read (4 KiB) */
    uint8_t in_buf[PTY_IN_CAPACITY];
    size_t  in_head;
    size_t  in_tail;
    size_t  in_count;
    uint32_t in_data_bytes;
    uint32_t in_space_bytes;

    /* Output ring buffer: Slave write -> Master read (16 KiB) */
    uint8_t out_buf[PTY_OUT_CAPACITY];
    size_t  out_head;
    size_t  out_tail;
    size_t  out_count;
    uint32_t out_data_bytes;
    uint32_t out_space_bytes;

    /* Terminal settings & dimensions */
    terminal_attrs_t attrs;
    uint16_t cols;
    uint16_t rows;

    /* Session & Job Control */
    uint64_t controlling_sid;
    uint64_t fg_pgid;
    process_group_ref_t foreground;

    /* Endpoints & Ref counting */
    vfs_node_t *master_node;
    vfs_node_t *slave_node;
    bool master_open;
    bool slave_open;
    uint32_t active_endpoints;
} pty_t;

int pty_create(vfs_node_t **out_master, vfs_node_t **out_slave);
void pty_close_endpoint(vfs_node_t *node);

bool is_pty_master(const vfs_node_t *node);
bool is_pty_slave(const vfs_node_t *node);

int64_t pty_master_read(vfs_node_t *node, uint64_t flags, void *buf, size_t count);
int64_t pty_master_write(vfs_node_t *node, uint64_t *flags, bool append, const void *buf, size_t count);
int64_t pty_slave_read(vfs_node_t *node, uint64_t flags, void *buf, size_t count);
int64_t pty_slave_write(vfs_node_t *node, uint64_t *flags, bool append, const void *buf, size_t count);
int64_t pty_slave_input_read(vfs_node_t *node, void *buf, size_t count, int64_t timeout_ms);

int64_t pty_set_session(vfs_node_t *master_node, uint64_t child_pid);
int64_t pty_tcgetpgrp(vfs_node_t *slave_node, int64_t *out_pgid);
int64_t pty_tcsetpgrp(vfs_node_t *slave_node, uint64_t pgid);
int64_t pty_set_winsize(vfs_node_t *node, uint16_t cols, uint16_t rows);
int64_t pty_get_winsize(vfs_node_t *node, uint16_t *cols, uint16_t *rows);
int64_t pty_set_termattr(vfs_node_t *node, const terminal_attrs_t *attrs);
int64_t pty_get_termattr(vfs_node_t *node, terminal_attrs_t *attrs);

#endif /* FORTRESS_PTY_H */
