#ifndef FORTRESS_VFS_H
#define FORTRESS_VFS_H

#include "types.h"

#define VFS_MAX_PATH     256
#define VFS_MAX_NAME     64
#define MAX_PROCESS_FDS  32

#define VFS_O_RDONLY  0
#define VFS_O_WRONLY  1
#define VFS_O_RDWR    2
#define VFS_O_ACCMODE 3
#define VFS_O_CREAT   0x40
#define VFS_O_TRUNC   0x200
#define VFS_O_APPEND  0x400

#define VFS_SUCCESS      0
#define VFS_EPERM        1
#define VFS_ENOENT       2
#define VFS_EIO          5
#define VFS_EBADF        9
#define VFS_ENOMEM       12
#define VFS_EEXIST       17
#define VFS_EINVAL       22
#define VFS_EFBIG        27
#define VFS_ENOSPC       28
#define VFS_EROFS        30
#define VFS_EOPNOTSUPP   95

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
    void *fs_private;
    struct vfs_node *(*lookup)(struct vfs_node *, const char *);
    int64_t (*read)(struct vfs_node *, uint64_t, void *, size_t);
    int64_t (*write)(struct vfs_node *, uint64_t, const void *, size_t);
    struct vfs_node *(*create)(struct vfs_node *dir, const char *name, vfs_node_type_t type);
    int (*truncate)(struct vfs_node *node, uint64_t new_size);
    int (*readdir)(struct vfs_node *, uint64_t, void *);
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
vfs_node_t *vfs_create(const char *path, vfs_node_type_t type);
int         vfs_truncate(vfs_node_t *node, uint64_t new_size);
file_t     *vfs_open(const char *path, int flags);
file_t     *vfs_open_ext(const char *path, int flags, int *err_out);
int64_t     vfs_read(file_t *file, void *buf, size_t count);
int64_t     vfs_write(file_t *file, const void *buf, size_t count);
int         vfs_close(file_t *file);
int         vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat);
int         vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out_dirent);

#endif /* FORTRESS_VFS_H */
