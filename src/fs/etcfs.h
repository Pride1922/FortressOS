#ifndef FORTRESS_ETCFS_H
#define FORTRESS_ETCFS_H

#include "vfs.h"

/*
 * ETCFS: In-Memory Writable Configuration Filesystem for Live Media.
 *
 * Provides a mutable RAM-backed filesystem rooted at /etc on live media.
 * Existing pristine TarFS files are read copy-on-write without consuming
 * dynamic memory until modified. New file creation, truncation, writes,
 * atomic replacement (SYS_RENAME_REPLACE), unlinks, and nested directories
 * are supported under a strict 16 MiB aggregate memory quota.
 *
 * Critical Security Invariant:
 * /etc/passwd, /etc/group, and /etc/shadow retain VFS_MNT_RDONLY and reject
 * mutation or deletion to maintain fail-closed user authentication under
 * userdb_io on live boot.
 */

#define ETCFS_MAX_BYTES (16 * 1024 * 1024ULL) /* 16 MiB memory ceiling */

int etcfs_init(vfs_node_t *root);
size_t etcfs_allocated_bytes(void);

#endif /* FORTRESS_ETCFS_H */
