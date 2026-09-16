#ifndef FORTRESS_VFS_H
#define FORTRESS_VFS_H

#include "types.h"

#define VFS_MAX_PATH     256
#define VFS_MAX_NAME     64
#define MAX_PROCESS_FDS  32

typedef enum {
    VFS_FILE = 1,
    VFS_DIRECTORY = 2
} vfs_node_type_t;

typedef struct vfs_node {
    char             name[VFS_MAX_NAME];
    char             path[VFS_MAX_PATH];
    vfs_node_type_t  type;
    uint64_t         size;
    const void      *data;      /* Backing pointer in USTAR initramfs */
    struct vfs_node *parent;
    struct vfs_node *next;      /* Sibling in parent directory */
    struct vfs_node *children;  /* Child list for directory */
} vfs_node_t;

typedef struct file {
    vfs_node_t *node;
    uint64_t    offset;
    int         flags;
    int         ref_count;
} file_t;

typedef struct {
    uint64_t size;
    uint32_t type;
    uint32_t mode;
} vfs_stat_t;

typedef struct {
    char     name[VFS_MAX_NAME];
    uint32_t type;
    uint64_t size;
} vfs_dirent_t;

void        vfs_init(void);
vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_create_node(const char *path, vfs_node_type_t type, uint64_t size, const void *data);
file_t     *vfs_open(const char *path, int flags);
int64_t     vfs_read(file_t *file, void *buf, size_t count);
int         vfs_close(file_t *file);
int         vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat);
int         vfs_readdir(vfs_node_t *dir_node, uint64_t index, vfs_dirent_t *out_dent);

#endif /* FORTRESS_VFS_H */
