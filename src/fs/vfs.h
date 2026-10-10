#ifndef FORTRESS_VFS_H
#define FORTRESS_VFS_H

#include "types.h"
#include "creds.h"

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
#define VFS_O_CLOEXEC 0x80000

#define VFS_SUCCESS      0
#define VFS_EPERM        1
#define VFS_EACCES       13
#define VFS_ENOTDIR      20
#define VFS_ENOENT       2
#define VFS_EINTR        4
#define VFS_EIO          5
#define VFS_EBADF        9
#define VFS_EAGAIN       11
#define VFS_ENOMEM       12
#define VFS_EEXIST       17
#define VFS_EINVAL       22
#define VFS_EFBIG        27
#define VFS_ENOSPC       28
#define VFS_EROFS        30
#define VFS_EPIPE        32
#define VFS_ENOTEMPTY    39
#define VFS_EOPNOTSUPP   95

typedef enum {
    VFS_FILE = 1,
    VFS_DIRECTORY = 2,
    VFS_STREAM = 3
} vfs_node_type_t;

#define VFS_S_IFMT  0170000u
#define VFS_S_IFREG 0100000u
#define VFS_S_IFDIR 0040000u
#define VFS_S_IFCHR 0020000u
#define VFS_S_IFBLK 0060000u
#define VFS_S_IFIFO 0010000u
#define VFS_MNT_RDONLY 1u
#define VFS_MNT_NOSUID 2u
#define VFS_MNT_NODEV  4u
typedef struct { uint16_t mode; uint32_t uid, gid; } vfs_create_attrs_t;
/* Internal coherent snapshot. Existing vfs_stat_t / SYS_STAT stay 16 bytes. */
typedef struct { uint64_t size; uint32_t type, mode, uid, gid, mnt_flags, reserved; } vfs_metadata_t;

typedef struct vfs_node {
    char             name[VFS_MAX_NAME];
    char             path[VFS_MAX_PATH];
    vfs_node_type_t  type;
    uint64_t         size;
    uint32_t uid, gid;
    uint16_t mode;
    uint8_t mnt_flags;
    int (*metadata)(struct vfs_node *, vfs_metadata_t *);
    /* Owned creation with metadata staged in the same authoritative mutation. */
    struct vfs_node *(*create_attrs_ref)(struct vfs_node *, const char *, vfs_node_type_t,
                                        const vfs_create_attrs_t *, int *);
    bool             is_stream; /* No file offset/size semantics; callback owns EOF. */
    const void      *data;      /* Backing pointer in USTAR initramfs */
    struct vfs_node *parent;
    struct vfs_node *next;      /* Sibling in parent directory */
    struct vfs_node *children;  /* Child list for directory */
    void *fs_private;
    struct vfs_node *(*lookup)(struct vfs_node *, const char *);
    int64_t (*read)(struct vfs_node *, uint64_t, void *, size_t);
    int64_t (*write)(struct vfs_node *, uint64_t *, bool, const void *, size_t);
    struct vfs_node *(*create)(struct vfs_node *dir, const char *name, vfs_node_type_t type);
    int (*unlink)(struct vfs_node *dir, const char *name);
    int (*rename)(struct vfs_node *old_dir, const char *old_name, struct vfs_node *new_dir, const char *new_name);
    int (*truncate)(struct vfs_node *node, uint64_t new_size);
    int (*readdir)(struct vfs_node *, uint64_t, void *);
    int (*can_write)(struct vfs_node *node);
    /* Optional file_t lifetime pin; paired with close once per independent
     * open, not per dup. Filesystem-owned nodes survive namespace removal. */
    int (*open)(struct vfs_node *node);
    /* When true, unlink/rename callbacks own hierarchy changes and retain
     * detached nodes; VFS must neither free nor move them a second time. */
    bool owns_nodes;
    /* Write callback serializes the shared file_t offset and node size under
     * filesystem exclusion. VFS must pass the real offset, without an unlocked
     * snapshot or a second publication after the callback returns. */
    bool serializes_write_offset;
    bool rename_no_replace;
    /* Called once on final file_t release; may destroy anonymous nodes. */
    void (*close)(struct vfs_node *node);
    /* Optional owned-node API. lookup_ref/create_ref return one reference
     * acquired under filesystem exclusion; put releases it. Legacy lookup/
     * create callbacks must continue returning mount-lifetime stable nodes. */
    struct vfs_node *(*lookup_ref)(struct vfs_node *, const char *, int *);
    struct vfs_node *(*create_ref)(struct vfs_node *, const char *, vfs_node_type_t, int *);
    int (*get)(struct vfs_node *);
    void (*put)(struct vfs_node *);
    /* Actor adapters decide from authoritative values under filesystem
     * exclusion. Trusted callbacks above remain separate. No G on entry. */
    int (*permission_actor)(struct vfs_node *, unsigned, const creds_t *);
    struct vfs_node *(*lookup_actor)(struct vfs_node *, const char *, const creds_t *, int *);
    struct vfs_node *(*create_actor)(struct vfs_node *, const char *, vfs_node_type_t,
                                    uint32_t, const creds_t *, int *);
    int (*open_actor)(struct vfs_node *, unsigned, bool, const creds_t *);
    int (*unlink_actor)(struct vfs_node *, const char *, bool, const creds_t *);
    int (*rename_actor)(struct vfs_node *, const char *, struct vfs_node *, const char *, bool, const creds_t *);
    int (*setattr_actor)(struct vfs_node *, bool, uint32_t, creds_id_change_t,
                         creds_id_change_t, const creds_t *);
    int64_t (*write_actor)(struct vfs_node *, uint64_t *, bool, const void *, size_t, const creds_t *);
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
/* Owned reference, including errors from filesystem lookup. Pair success with
 * vfs_node_put. Non-managed filesystems retain their existing stable lifetime. */
vfs_node_t *vfs_lookup_ref(const char *path, int *err_out);
vfs_node_t *vfs_create_ref(const char *path, vfs_node_type_t type, int *err_out);
void        vfs_node_put(vfs_node_t *node);
vfs_node_t *vfs_create_node(const char *path, vfs_node_type_t type, uint64_t size, const void *data);
vfs_node_t *vfs_create(const char *path, vfs_node_type_t type);
vfs_node_t *vfs_create_ext(const char *path, vfs_node_type_t type, int *err_out);
void        vfs_set_last_create_error(int err);
int         vfs_get_last_create_error(void);
int         vfs_mkdir(const char *path, uint32_t mode);
int         vfs_unlink(const char *path);
int         vfs_rename(const char *oldpath, const char *newpath);
int         vfs_truncate(vfs_node_t *node, uint64_t new_size);
file_t     *vfs_open(const char *path, int flags);
file_t     *vfs_open_ext(const char *path, int flags, int *err_out);
int64_t     vfs_read(file_t *file, void *buf, size_t count);
int64_t     vfs_write(file_t *file, const void *buf, size_t count);
int         vfs_close(file_t *file);
int         vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat);
int         vfs_metadata(vfs_node_t *node, vfs_metadata_t *out);
vfs_node_t *vfs_create_attrs_ref(const char *path, vfs_node_type_t type,
                                const vfs_create_attrs_t *attrs, int *error);
int         vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *out_dirent);
/* Descriptor admission survives mode changes. Advances offset only on success;
 * retains filesystem errors. No fresh pathname/DAC decision. */
int         vfs_readdir_file(file_t *file, vfs_dirent_t *out_dirent);
vfs_node_t *vfs_get_terminal_node(void);
file_t     *vfs_open_terminal(int flags);

/* Actor admission dispatch. Mutable nodes decide under their filesystem lock;
 * immutable boot nodes use immutable published values. The value engine in
 * permission_values.h is pure. No process lock may be held across entry. */
#define VFS_MAY_EXEC  1u
#define VFS_MAY_WRITE 2u
#define VFS_MAY_READ  4u
int vfs_permission(const vfs_node_t *, unsigned mask, const creds_t *actor);
int vfs_may_delete(const vfs_node_t *, const vfs_node_t *, const creds_t *actor);
/* Caller owns one stable, canonical actor value, captured before VFS entry.
 * No process lock or other subsystem lock on entry. References remain owned;
 * no credentials or node pointers are stored for use after the operation. */
vfs_node_t *vfs_lookup_creds(const char *,const creds_t *,int *);
file_t *vfs_open_creds(const char *,int,const creds_t *,int *);
file_t *vfs_open_mode_creds(const char *,int,uint32_t,const creds_t *,int *);
file_t *vfs_open_exec_creds(const char *,const creds_t *,int *);
int vfs_mkdir_creds(const char *,uint32_t,const creds_t *);
int vfs_unlink_creds(const char *,const creds_t *);
int vfs_rename_creds(const char *,const char *,const creds_t *);
int vfs_readdir_creds(vfs_node_t *,uint64_t,vfs_dirent_t *,const creds_t *);
int vfs_chmod_creds(const char *, uint32_t, const creds_t *);
int vfs_fchmod_creds(file_t *, uint32_t, const creds_t *);
int vfs_chown_creds(const char *, creds_id_change_t, creds_id_change_t, const creds_t *);
int64_t vfs_write_creds(file_t *,const void *,size_t,const creds_t *);
/* Join preserves every component for admission. Canonicalization is for cwd
 * storage only, after the original path has passed the owned walk. */
int vfs_join_path(const char *,const char *,char *,size_t);
int vfs_canonical_path(const char *,char *,size_t);
/* Explicit trusted kernel entry points. Older unsuffixed names are retained
 * for source compatibility with host fixtures; user callers must use _creds.
 * Kernel bypass is selected by API, never fabricated root credentials. */
static inline vfs_node_t *vfs_lookup_kernel(const char *p) { return vfs_lookup(p); }
static inline vfs_node_t *vfs_lookup_ref_kernel(const char *p,int *e) { return vfs_lookup_ref(p,e); }
static inline file_t *vfs_open_kernel(const char *p,int f) { return vfs_open(p,f); }
static inline file_t *vfs_open_ext_kernel(const char *p,int f,int *e) { return vfs_open_ext(p,f,e); }
static inline int vfs_mkdir_kernel(const char *p,uint32_t m) { return vfs_mkdir(p,m); }
static inline int vfs_unlink_kernel(const char *p) { return vfs_unlink(p); }
static inline int vfs_rename_kernel(const char *p,const char *q) { return vfs_rename(p,q); }
static inline int vfs_readdir_kernel(vfs_node_t *n,uint64_t i,vfs_dirent_t *d) { return vfs_readdir(n,i,d); }
static inline vfs_node_t *vfs_create_kernel(const char *p,vfs_node_type_t t) { return vfs_create(p,t); }
static inline vfs_node_t *vfs_create_ext_kernel(const char *p,vfs_node_type_t t,int *e) { return vfs_create_ext(p,t,e); }
static inline vfs_node_t *vfs_create_ref_kernel(const char *p,vfs_node_type_t t,int *e) { return vfs_create_ref(p,t,e); }
static inline vfs_node_t *vfs_create_attrs_ref_kernel(const char *p,vfs_node_type_t t,const vfs_create_attrs_t *a,int *e) { return vfs_create_attrs_ref(p,t,a,e); }
static inline vfs_node_t *vfs_create_node_kernel(const char *p,vfs_node_type_t t,uint64_t z,const void *d) { return vfs_create_node(p,t,z,d); }
static inline int vfs_truncate_kernel(vfs_node_t *n,uint64_t z) { return vfs_truncate(n,z); }
static inline file_t *vfs_open_terminal_kernel(int f) { return vfs_open_terminal(f); }
#ifdef FORTRESS_PERMISSIONS_TRACE
/* Test-build hook receives borrowed values synchronously, outside process lock.
 * Normal builds contain no hook or permission logging. */
void vfs_permission_trace(const vfs_node_t *,unsigned,const creds_t *);
#endif
#ifdef TEST_PERMISSIONS_INTERLEAVING
/* Test only, before authoritative admission and with no filesystem lock held.
 * 1=create, 2=unlink, 3=rename, 4=open. Borrowed owned node; synchronous hook. */
void vfs_admission_interleave_test(const vfs_node_t *,unsigned);
#endif

#endif /* FORTRESS_VFS_H */
