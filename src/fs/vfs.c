#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "serial.h"
#include "console.h"
#include "input.h"
#include "terminal.h"
#include "thread.h"
#include "syscall_abi.h"
#include "runfs.h"
#include "devfs.h"
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
#include "permission_values.h"
#endif

static vfs_node_t *g_vfs_root = NULL;

static int64_t terminal_read(vfs_node_t *node, uint64_t offset, void *buf, size_t count) {
    (void)node; (void)offset;
    if (!buf || count == 0) return 0;
    int64_t result = input_read(buf, count);
    /* input_read uses syscall errors; VFS callbacks use VFS errors. */
    if (result == SYSCALL_EINTR) return -VFS_EINTR;
    if (result == SYSCALL_EBADF) return -VFS_EBADF;
    if (result == SYSCALL_EIO || result == SYSCALL_ENOTTY) return -VFS_EIO;
    if (result == SYSCALL_EOPNOTSUPP) return -VFS_EOPNOTSUPP;
    return result;
}

static int64_t terminal_write(vfs_node_t *node, uint64_t *offset, bool append, const void *buf, size_t count) {
    (void)node; (void)offset; (void)append;
    if (!buf || count == 0) return 0;
    console_inc_generation();
    const char *ptr = (const char *)buf;
    tcb_t *owner = thread_current();
    unsigned mode = owner ? owner->terminal_mode : TERM_MIRROR;
    if (mode != TERM_SERIAL && !console_is_quiet()) console_terminal_write(ptr, count);
    if (mode != TERM_LOCAL) {
        for (size_t i = 0; i < count; i++) serial_raw_putc(ptr[i]);
    }
    return (int64_t)count;
}

static int dummy_truncate(vfs_node_t *node, uint64_t size) {
    (void)node; (void)size;
    return 0;
}

static vfs_node_t g_terminal_node = {
    .name = "tty",
    .is_stream = true,
    .path = "/dev/tty",
    .type = VFS_STREAM,
    .size = 0,
    .read = terminal_read,
    .write = terminal_write,
    .truncate = dummy_truncate,
    .close = NULL,
};

static int64_t null_read(vfs_node_t *node, uint64_t offset, void *buf, size_t count) {
    (void)node; (void)offset; (void)buf; (void)count;
    return 0; /* EOF */
}

static int64_t null_write(vfs_node_t *node, uint64_t *offset, bool append, const void *buf, size_t count) {
    (void)node; (void)offset; (void)append; (void)buf;
    return (int64_t)count; /* Discard bytes */
}

static vfs_node_t g_null_node = {
    .name = "null",
    .is_stream = true,
    .path = "/dev/null",
    .type = VFS_STREAM,
    .size = 0,
    .read = null_read,
    .write = null_write,
    .truncate = dummy_truncate,
    .close = NULL,
};

vfs_node_t *vfs_get_terminal_node(void) {
    return &g_terminal_node;
}

file_t *vfs_open_terminal(int flags) {
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) return NULL;
    file->node = &g_terminal_node;
    file->offset = 0;
    file->flags = flags;
    file->ref_count = 1;
    return file;
}

void vfs_init(void) {
    if (g_vfs_root) return;

    g_vfs_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!g_vfs_root) {
        serial_puts("[FATAL] VFS: Failed to allocate root node\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    memset(g_vfs_root, 0, sizeof(vfs_node_t));
    g_vfs_root->close = NULL;
    g_vfs_root->name[0] = '/';
    g_vfs_root->name[1] = '\0';
    g_vfs_root->path[0] = '/';
    g_vfs_root->path[1] = '\0';
    g_vfs_root->type    = VFS_DIRECTORY;
    g_vfs_root->mode=VFS_S_IFDIR|0755;
    g_vfs_root->mnt_flags=VFS_MNT_RDONLY;

    runfs_init(g_vfs_root);
    devfs_init(g_vfs_root,&g_terminal_node,&g_null_node);

    serial_puts("[ OK ] VFS root (/) initialized\n");
}

static void normalize_path(const char *in_path, char *out_path, size_t out_max) {
    if (!in_path || !out_path || out_max < 2) return;

    size_t in_len = strlen(in_path);
    size_t out_idx = 0;

    /* Ensure leading slash */
    if (in_path[0] != '/') {
        out_path[out_idx++] = '/';
    }

    /* Skip leading "./" if present */
    size_t start = 0;
    if (in_len >= 2 && in_path[0] == '.' && in_path[1] == '/') {
        start = 1;
    }

    for (size_t i = start; i < in_len && out_idx + 1 < out_max; i++) {
        /* Avoid duplicate slashes */
        if (in_path[i] == '/' && out_idx > 0 && out_path[out_idx - 1] == '/') {
            continue;
        }
        out_path[out_idx++] = in_path[i];
    }

    /* Remove trailing slash unless root */
    if (out_idx > 1 && out_path[out_idx - 1] == '/') {
        out_idx--;
    }

    out_path[out_idx] = '\0';
}

int vfs_join_path(const char *cwd,const char *input,char *out,size_t cap) {
    if (!input || !*input || !out || cap<2) return -VFS_EINVAL;
    if (*input=='/') {
        size_t n=strlen(input);
        if (n>=cap || n>=VFS_MAX_PATH) return -VFS_EINVAL;
        memcpy(out,input,n+1);return 0;
    }
    if (!cwd || !*cwd) cwd="/";
    size_t a=strlen(cwd),b=strlen(input);bool slash=cwd[a-1]!='/';
    if (a+b+slash>=cap || a+b+slash>=VFS_MAX_PATH) return -VFS_EINVAL;
    memcpy(out,cwd,a);if (slash) out[a++]='/';memcpy(out+a,input,b+1);return 0;
}

int vfs_canonical_path(const char *input,char *out,size_t cap) {
    if (!input || *input!='/' || !out || cap<2) return -VFS_EINVAL;
    size_t length=1;out[0]='/';out[1]=0;
    const char *p=input;
    while (*p) {
        while (*p=='/') p++;
        const char *start=p;while (*p && *p!='/') p++;
        size_t n=p-start;if (!n) break;
        if (n==1 && start[0]=='.') continue;
        if (n==2 && start[0]=='.' && start[1]=='.') {
            if (length>1) { while (length>1 && out[length-1]!='/') length--;if (length>1) length--; }
            out[length]=0;continue;
        }
        size_t slash=length>1;
        if (length+slash+n>=cap) return -VFS_EINVAL;
        if (slash) out[length++]='/';
        memcpy(out+length,start,n);length+=n;out[length]=0;
    }
    return 0;
}

vfs_node_t *vfs_lookup(const char *path) {
    if (!g_vfs_root || !path) return NULL;
    if (strlen(path) >= VFS_MAX_PATH) return NULL;

    char norm[VFS_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));

    if (strcmp(norm, "/") == 0) {
        return g_vfs_root;
    }

    /* Tokenize path by '/' */
    vfs_node_t *curr = g_vfs_root;
    const char *p = norm + 1; /* Skip leading '/' */

    while (*p) {
        char comp[VFS_MAX_NAME];
        size_t c_idx = 0;
        while (*p && *p != '/' && c_idx + 1 < sizeof(comp)) {
            comp[c_idx++] = *p++;
        }
        comp[c_idx] = '\0';
        if (*p && *p != '/') return NULL;
        if (*p == '/') p++;

        if (curr->type != VFS_DIRECTORY) return NULL;
        if (curr->lookup) {
            curr = curr->lookup(curr, comp);
            if (!curr) return NULL;
            continue;
        }

        /* Find child in curr->children */
        vfs_node_t *child = curr->children;
        vfs_node_t *found = NULL;
        while (child) {
            if (strcmp(child->name, comp) == 0) {
                found = child;
                break;
            }
            child = child->next;
        }

        if (!found) {
            return NULL; /* Path component not found */
        }

        curr = found;
    }

    return curr;
}

void vfs_node_put(vfs_node_t *node) {
    if (node && node->put) node->put(node);
}

static vfs_node_t *lookup_common(const char *path, const creds_t *actor, int *err_out) {
    int error = -VFS_EINVAL;
    vfs_node_t *curr = NULL;
    if (!g_vfs_root || !path || strlen(path) >= VFS_MAX_PATH) goto done;
    char norm[VFS_MAX_PATH];
    /* Preserve dot components and terminal slash until each is admitted. */
    if (vfs_join_path("/",path,norm,sizeof(norm))) goto done;
    curr = g_vfs_root;
    if (curr != g_vfs_root || !strcmp(norm, "/")) { error = 0; goto done; }
    const char *p = norm + 1;
    while (*p) {
        while (*p=='/') p++;
        if (!*p) break;
        char component[VFS_MAX_NAME];size_t length = 0;
        while (*p && *p != '/' && length + 1 < sizeof(component)) component[length++] = *p++;
        component[length] = 0;
        if (*p && *p != '/') { error = -VFS_EINVAL; goto fail; }
        if (*p == '/') p++;
        if (curr->type != VFS_DIRECTORY) { error = -VFS_ENOTDIR; goto fail; }
        if (actor) {
            error=vfs_permission(curr,VFS_MAY_EXEC,actor);
            if (error) goto fail;
        }
        vfs_node_t *next = NULL;
        error = -VFS_ENOENT;
        if (actor && curr->lookup_actor) next=curr->lookup_actor(curr,component,actor,&error);
        else if (curr->lookup_ref) next = curr->lookup_ref(curr, component, &error);
        else {
            if (!strcmp(component,".")) next=curr;
            else if (!strcmp(component,"..")) next=curr->parent ? curr->parent : curr;
            else if (curr->lookup) next = curr->lookup(curr, component);
            else for (vfs_node_t *child = curr->children; child; child = child->next)
                if (!strcmp(child->name, component)) { next = child; break; }
            if (next && next->get) {
                error = next->get(next);
                if (error) next = NULL;
            }
        }
        if (!next) goto fail;
        vfs_node_put(curr);curr = next;
    }
    if (path[strlen(path)-1]=='/' && curr->type!=VFS_DIRECTORY) { error=-VFS_ENOTDIR;goto fail; }
    error = 0;
    goto done;
fail:
    vfs_node_put(curr);curr = NULL;
done:
    if (err_out) *err_out = error;
    return curr;
}

vfs_node_t *vfs_create_node(const char *path, vfs_node_type_t type, uint64_t size, const void *data) {
    if (!g_vfs_root || !path) return NULL;

    char norm[VFS_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));

    if (strcmp(norm, "/") == 0) {
        return g_vfs_root;
    }

    /* Check for duplicate path ambiguity */
    if (vfs_lookup(norm) != NULL) {
        serial_puts("[WARN] VFS: Duplicate path rejected: ");
        serial_puts(norm);
        serial_puts("\n");
        return NULL;
    }

    /* Find or create parent directory */
    vfs_node_t *curr = g_vfs_root;
    const char *p = norm + 1;

    while (*p) {
        char comp[VFS_MAX_NAME];
        size_t c_idx = 0;
        while (*p && *p != '/' && c_idx + 1 < sizeof(comp)) {
            comp[c_idx++] = *p++;
        }
        comp[c_idx] = '\0';
        bool is_last = (*p == '\0');
        if (*p == '/') p++;

        if (is_last) {
            /* Create target node */
            vfs_node_t *new_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
            if (!new_node) return NULL;

            memset(new_node, 0, sizeof(vfs_node_t));
            new_node->close = NULL;
            memcpy(new_node->name, comp, strlen(comp) + 1);
            memcpy(new_node->path, norm, strlen(norm) + 1);
            new_node->type   = type;
            new_node->mode=(type==VFS_DIRECTORY ? VFS_S_IFDIR|0755 : VFS_S_IFREG|0644);
            new_node->mnt_flags=VFS_MNT_RDONLY;
            new_node->size   = size;
            new_node->data   = data;
            new_node->parent = curr;

            /* Prepend to parent's children */
            new_node->next = curr->children;
            curr->children = new_node;
            return new_node;
        }

        /* Intermediate directory */
        vfs_node_t *child = curr->children;
        vfs_node_t *found = NULL;
        while (child) {
            if (strcmp(child->name, comp) == 0) {
                found = child;
                break;
            }
            child = child->next;
        }

        if (!found) {
            /* Auto-create parent directory node */
            vfs_node_t *dir_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
            if (!dir_node) return NULL;

            memset(dir_node, 0, sizeof(vfs_node_t));
            dir_node->close = NULL;
            memcpy(dir_node->name, comp, strlen(comp) + 1);
            /* Build path */
            size_t curr_len = strlen(curr->path);
            memcpy(dir_node->path, curr->path, curr_len);
            if (curr_len > 1) dir_node->path[curr_len++] = '/';
            memcpy(dir_node->path + curr_len, comp, strlen(comp) + 1);

            dir_node->type   = VFS_DIRECTORY;
            dir_node->mode=VFS_S_IFDIR|0755;
            dir_node->mnt_flags=VFS_MNT_RDONLY;
            dir_node->parent = curr;
            dir_node->next   = curr->children;
            curr->children   = dir_node;
            found = dir_node;
        }

        curr = found;
    }

    return curr;
}

#if defined(FORTRESS_PERMISSIONS_TRACE) && !defined(TEST_PERMISSIONS_WIRING)
void vfs_permission_trace(const vfs_node_t *node,unsigned mask,const creds_t *actor) {
    (void)node; /* Mutable path/metadata is not trace authority. */
    serial_puts("PERM TRACE mask=");serial_print_dec(mask);
    serial_puts(" euid=");serial_print_dec(actor->euid);
    serial_puts(" egid=");serial_print_dec(actor->egid);
    serial_puts(" caps=");serial_print_hex(actor->cap_effective);serial_puts("\n");
}
#endif
int vfs_permission(const vfs_node_t *node,unsigned mask,const creds_t *actor) {
    /* Deliberately permissive: no mutable metadata read, I/O or locks. */
#ifdef FORTRESS_PERMISSIONS_TRACE
    vfs_permission_trace(node,mask,actor);
#else
    (void)node;(void)mask;(void)actor;
#endif
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (!node || !actor) return -VFS_EINVAL;
    if (node->permission_actor) return node->permission_actor((vfs_node_t *)node,mask,actor);
    /* Only immutable boot nodes may use their published fields directly. */
    if (node->owns_nodes) return -VFS_EOPNOTSUPP;
    vfs_metadata_t value={.mode=node->mode,.uid=node->uid,.gid=node->gid,.mnt_flags=node->mnt_flags};
    unsigned type=value.mode & VFS_S_IFMT;
    if ((type==VFS_S_IFCHR || type==VFS_S_IFBLK) && (value.mnt_flags & VFS_MNT_NODEV))
        return -VFS_EACCES;
    int r=permission_access_value(&value,mask,actor);
    if (!r && type==VFS_S_IFBLK && !(actor->cap_effective & CAP_SYS_RAWIO)) r=-VFS_EPERM;
    return r;
#else
    return 0;
#endif
}
int vfs_may_delete(const vfs_node_t *dir,const vfs_node_t *victim,const creds_t *actor) {
    (void)victim; /* Sticky/owner decision and authoritative adapters: Phase 2. */
    return vfs_permission(dir,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
}
vfs_node_t *vfs_lookup_ref(const char *path,int *error) { return lookup_common(path,NULL,error); }
vfs_node_t *vfs_lookup_creds(const char *path,const creds_t *actor,int *error) {
    if (!actor) { if (error) *error=-VFS_EINVAL;return NULL; }
    return lookup_common(path,actor,error);
}

static int g_last_create_error = -VFS_EIO;
void vfs_set_last_create_error(int err) {
    g_last_create_error = err;
}
int vfs_get_last_create_error(void) {
    return g_last_create_error;
}

static vfs_node_t *vfs_create_common(const char *path, vfs_node_type_t type, int *err_out, bool owned, const vfs_create_attrs_t *attrs, const creds_t *actor) {
    if (err_out) *err_out = -VFS_EINVAL;
    if (!path || strlen(path) >= VFS_MAX_PATH) return NULL;
    if (type==VFS_FILE && *path && path[strlen(path)-1]=='/') {
        if (err_out) *err_out=-VFS_ENOTDIR;
        return NULL;
    }
    char norm[VFS_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));
    if (!strcmp(norm, "/")) return NULL;

    const char *last_slash = NULL;
    for (const char *p = norm; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash) return NULL;

    char dir_path[VFS_MAX_PATH];
    size_t dir_len = (size_t)(last_slash - norm);
    if (dir_len == 0) {
        dir_path[0] = '/';
        dir_path[1] = '\0';
    } else {
        memcpy(dir_path, norm, dir_len);
        dir_path[dir_len] = '\0';
    }
    const char *name = last_slash + 1;
    if (!*name || strlen(name) >= VFS_MAX_NAME) return NULL;

    int lookup_error = 0;
    vfs_node_t *dir = lookup_common(dir_path, actor, &lookup_error);
    if (!dir) {
        if (err_out) *err_out = lookup_error;
        return NULL;
    }
    if (dir->type != VFS_DIRECTORY) {
        if (err_out) *err_out = -VFS_ENOTDIR; /* ENOTDIR */
        vfs_node_put(dir);
        return NULL;
    }
    if (actor && dir->create_actor) {
#ifdef TEST_PERMISSIONS_INTERLEAVING
        vfs_admission_interleave_test(dir,1);
#endif
        int r=0;vfs_node_t *res=dir->create_actor(dir,name,type,
            attrs ? attrs->mode : type==VFS_DIRECTORY ? 0755u : 0644u,actor,&r);
        vfs_node_put(dir);if (err_out) *err_out=r;return res;
    }
    if (actor) {
        int r=vfs_permission(dir,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if (r) { vfs_node_put(dir);if (err_out) *err_out=r;return NULL; }
    }
    if (!dir->create && !dir->create_attrs_ref) {
        if (err_out) *err_out = -VFS_EROFS;
        vfs_node_put(dir);
        return NULL;
    }
    if (attrs && !dir->create_attrs_ref) {
        if (err_out) *err_out=-VFS_EOPNOTSUPP;
        vfs_node_put(dir);return NULL;
    }
    int create_error = 0;
    vfs_node_t *res = attrs && dir->create_attrs_ref ? dir->create_attrs_ref(dir,name,type,attrs,&create_error) : owned && dir->create_ref ? dir->create_ref(dir, name, type, &create_error) : dir->create(dir, name, type);
    if (!res && !create_error) create_error = vfs_get_last_create_error();
    vfs_node_put(dir);
    if (!res) {
        if (err_out) *err_out = create_error;
        return NULL;
    }
    if (err_out) *err_out = VFS_SUCCESS;
    return res;
}

vfs_node_t *vfs_create_ext(const char *path, vfs_node_type_t type, int *err_out) {
    return vfs_create_common(path, type, err_out, false, NULL, NULL);
}

vfs_node_t *vfs_create_ref(const char *path, vfs_node_type_t type, int *err_out) {
    return vfs_create_common(path, type, err_out, true, NULL, NULL);
}

vfs_node_t *vfs_create(const char *path, vfs_node_type_t type) {
    return vfs_create_ext(path, type, NULL);
}

vfs_node_t *vfs_create_attrs_ref(const char *path,vfs_node_type_t type,const vfs_create_attrs_t *attrs,int *error) {
    if (!attrs || (attrs->mode & ~07777u) || (type!=VFS_FILE && type!=VFS_DIRECTORY)) {
        if (error) *error=-VFS_EINVAL;
        return NULL;
    }
    return vfs_create_common(path,type,error,true,attrs,NULL);
}

int vfs_mkdir(const char *path, uint32_t mode) {
    int err = 0;
    vfs_create_attrs_t attrs={.mode=mode & 07777u};
    vfs_node_t *node = vfs_create_attrs_ref(path, VFS_DIRECTORY, &attrs, &err);
    if (!node) return err ? err : -VFS_EIO;
    vfs_node_put(node);
    return VFS_SUCCESS;
}

static int unlink_common(const char *path,const creds_t *actor) {
    if (!path || strlen(path) >= VFS_MAX_PATH) return -VFS_EINVAL;
    char norm[VFS_MAX_PATH];
    normalize_path(path, norm, sizeof(norm));
    if (!strcmp(norm, "/")) return -VFS_EPERM;

    const char *last_slash = NULL;
    for (const char *p = norm; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash) return -VFS_EINVAL;

    char dir_path[VFS_MAX_PATH];
    size_t dir_len = (size_t)(last_slash - norm);
    if (dir_len == 0) {
        dir_path[0] = '/';
        dir_path[1] = '\0';
    } else {
        memcpy(dir_path, norm, dir_len);
        dir_path[dir_len] = '\0';
    }
    const char *name = last_slash + 1;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return -VFS_EINVAL;

    int lookup_error = 0;
    vfs_node_t *dir = lookup_common(dir_path, actor, &lookup_error);
    if (!dir) return lookup_error;
    if (dir->type != VFS_DIRECTORY) { vfs_node_put(dir);return -VFS_ENOTDIR; }
    if (actor && dir->unlink_actor) {
#ifdef TEST_PERMISSIONS_INTERLEAVING
        vfs_admission_interleave_test(dir,2);
#endif
        int r=dir->unlink_actor(dir,name,path[strlen(path)-1]=='/',actor);vfs_node_put(dir);return r;
    }
    if (actor) {
        vfs_node_t *victim=lookup_common(norm,actor,&lookup_error);
        if (!victim) { vfs_node_put(dir);return lookup_error; }
        int r=vfs_may_delete(dir,victim,actor);
        vfs_node_put(victim);
        if (r) { vfs_node_put(dir);return r; }
    }
    if (dir->create_ref) {
        int result=dir->unlink ? dir->unlink(dir,name) : -VFS_EROFS;
        vfs_node_put(dir);return result;
    }
    vfs_node_put(dir); /* Legacy filesystem nodes retain their stable lifetime. */

    vfs_node_t *target = vfs_lookup(norm);
    if (!target) return -VFS_ENOENT;

    if (!dir->unlink) return -VFS_EROFS;

    int res = dir->unlink(dir, name);
    if (res != 0) return res;

    if (target->owns_nodes) return VFS_SUCCESS;

    /* Unlink succeeded in filesystem; detach target from VFS child tree */
    vfs_node_t **curr = &dir->children;
    while (*curr) {
        if (*curr == target) {
            *curr = target->next;
            break;
        }
        curr = &(*curr)->next;
    }
    if (!target->owns_nodes) kfree(target);
    return VFS_SUCCESS;
}

static int rename_common(const char *oldpath, const char *newpath,const creds_t *actor) {
    if (!oldpath || !newpath) return -VFS_EINVAL;
    if (strlen(oldpath) >= VFS_MAX_PATH || strlen(newpath) >= VFS_MAX_PATH) return -VFS_EINVAL;

    char norm_old[VFS_MAX_PATH], norm_new[VFS_MAX_PATH];
    normalize_path(oldpath, norm_old, sizeof(norm_old));
    normalize_path(newpath, norm_new, sizeof(norm_new));

    if (!strcmp(norm_old, "/") || !strcmp(norm_new, "/")) return -VFS_EPERM;
    bool same_path = !strcmp(norm_old, norm_new);

    /* Check prefix: cannot move a directory inside itself */
    size_t old_len = strlen(norm_old);
    if (!actor && !same_path && !strncmp(norm_new, norm_old, old_len) && (norm_new[old_len] == '/' || norm_new[old_len] == '\0')) {
        return -VFS_EINVAL;
    }

    const char *old_slash = NULL;
    for (const char *p = norm_old; *p; p++) if (*p == '/') old_slash = p;
    char old_dir_path[VFS_MAX_PATH];
    size_t old_dir_len = (size_t)(old_slash - norm_old);
    if (old_dir_len == 0) { old_dir_path[0] = '/'; old_dir_path[1] = '\0'; }
    else { memcpy(old_dir_path, norm_old, old_dir_len); old_dir_path[old_dir_len] = '\0'; }
    const char *old_name = old_slash + 1;

    const char *new_slash = NULL;
    for (const char *p = norm_new; *p; p++) if (*p == '/') new_slash = p;
    char new_dir_path[VFS_MAX_PATH];
    size_t new_dir_len = (size_t)(new_slash - norm_new);
    if (new_dir_len == 0) { new_dir_path[0] = '/'; new_dir_path[1] = '\0'; }
    else { memcpy(new_dir_path, norm_new, new_dir_len); new_dir_path[new_dir_len] = '\0'; }
    const char *new_name = new_slash + 1;

    if (!*old_name || !*new_name) return -VFS_EINVAL;
    if (strlen(new_name) >= VFS_MAX_NAME) return -VFS_EINVAL;

    int old_error=0,new_error=0;
    vfs_node_t *old_dir = lookup_common(old_dir_path,actor,&old_error);
    vfs_node_t *new_dir = lookup_common(new_dir_path,actor,&new_error);
    if (!old_dir || !new_dir) {
        vfs_node_put(old_dir);vfs_node_put(new_dir);return !old_dir ? old_error : new_error;
    }
    if (old_dir->type!=VFS_DIRECTORY || new_dir->type!=VFS_DIRECTORY) {
        vfs_node_put(new_dir);vfs_node_put(old_dir);return -VFS_ENOTDIR;
    }
    if (actor && old_dir->rename_actor) {
#ifdef TEST_PERMISSIONS_INTERLEAVING
        vfs_admission_interleave_test(old_dir,3);
#endif
        bool require_directory=oldpath[strlen(oldpath)-1]=='/' || newpath[strlen(newpath)-1]=='/';
        int r=old_dir->rename_actor(old_dir,old_name,new_dir,new_name,require_directory,actor);
        vfs_node_put(new_dir);vfs_node_put(old_dir);return r;
    }
    if (actor) {
        int r=0;
        vfs_node_t *victim=lookup_common(norm_old,actor,&r);
        if (victim) {
            r=vfs_may_delete(old_dir,victim,actor);
            if (!r && victim->type==VFS_DIRECTORY && old_dir!=new_dir)
                r=vfs_permission(victim,VFS_MAY_WRITE,actor);
            vfs_node_put(victim);
            if (!r) r=vfs_permission(new_dir,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
            if (!r) {
                vfs_node_t *dest=lookup_common(norm_new,actor,&r);
                if (dest) { r=vfs_may_delete(new_dir,dest,actor);vfs_node_put(dest); }
                else if (r==-VFS_ENOENT) r=0;
            }
        }
        if (r) { vfs_node_put(new_dir);vfs_node_put(old_dir);return r; }
    }
    /* A no-op still resolves/admit the source. Missing names and a file with
     * a trailing slash cannot report success. No mutation callback runs. */
    if (same_path) {
        int result = 0;
        vfs_node_t *target = lookup_common(norm_old, actor, &result);
        if (target) {
            size_t a = strlen(oldpath), b = strlen(newpath);
            if (target->type != VFS_DIRECTORY &&
                ((a > 1 && oldpath[a-1] == '/') || (b > 1 && newpath[b-1] == '/')))
                result = -VFS_ENOTDIR;
            vfs_node_put(target);
        }
        vfs_node_put(new_dir); vfs_node_put(old_dir);
        return result;
    }
    if (old_dir->create_ref || new_dir->create_ref) {
        int result=-VFS_EROFS;
        if (old_dir->type!=VFS_DIRECTORY || new_dir->type!=VFS_DIRECTORY) result=-VFS_ENOTDIR;
        else if (old_dir->rename && old_dir->rename==new_dir->rename) {
            vfs_node_t *target=lookup_common(norm_old,actor,&result);
            if (target) {
                size_t a=strlen(oldpath),b=strlen(newpath);
                if (target->type!=VFS_DIRECTORY && ((a>1 && oldpath[a-1]=='/') || (b>1 && newpath[b-1]=='/'))) result=-VFS_ENOTDIR;
                else result=old_dir->rename(old_dir,old_name,new_dir,new_name);
                vfs_node_put(target);
            }
        }
        vfs_node_put(new_dir);vfs_node_put(old_dir);return result;
    }
    vfs_node_put(new_dir);vfs_node_put(old_dir);
    if (old_dir->type != VFS_DIRECTORY || new_dir->type != VFS_DIRECTORY) return -VFS_ENOTDIR; /* ENOTDIR */

    size_t oldpath_len = strlen(oldpath);
    size_t newpath_len = strlen(newpath);
    bool old_has_slash = (oldpath_len > 1 && oldpath[oldpath_len - 1] == '/');
    bool new_has_slash = (newpath_len > 1 && newpath[newpath_len - 1] == '/');

    vfs_node_t *target = vfs_lookup(norm_old);
    if (!target) return -VFS_ENOENT;
    if (target->type != VFS_DIRECTORY && (old_has_slash || new_has_slash)) {
        return -VFS_ENOTDIR; /* ENOTDIR */
    }

    if (!old_dir->rename || old_dir->rename != new_dir->rename) return -VFS_EROFS;

    /* If destination already exists in VFS, verify types and unlink/replace */
    vfs_node_t *dest = vfs_lookup(norm_new);
    if (dest) {
        if (dest == target) return VFS_SUCCESS;
        if (old_dir->rename_no_replace) return -VFS_EEXIST;
        if (dest->type == VFS_DIRECTORY && target->type != VFS_DIRECTORY) return -7; /* EISDIR */
        if (dest->type != VFS_DIRECTORY && target->type == VFS_DIRECTORY) return -VFS_ENOTDIR; /* ENOTDIR */
        int ures = unlink_common(norm_new,actor);
        if (ures != 0) return ures;
    }

    int res = old_dir->rename(old_dir, old_name, new_dir, new_name);
    if (res != 0) return res;

    if (target->owns_nodes) return VFS_SUCCESS;

    /* Move target in VFS hierarchy */
    if (old_dir != new_dir) {
        vfs_node_t **curr = &old_dir->children;
        while (*curr) {
            if (*curr == target) {
                *curr = target->next;
                break;
            }
            curr = &(*curr)->next;
        }
        target->next = new_dir->children;
        new_dir->children = target;
        target->parent = new_dir;
    }

    memcpy(target->name, new_name, strlen(new_name) + 1);
    memcpy(target->path, norm_new, strlen(norm_new) + 1);

    return VFS_SUCCESS;
}

int vfs_truncate(vfs_node_t *node, uint64_t new_size) {
    if (!node) return -1;
    if (node->type == VFS_DIRECTORY) return -7; /* EISDIR */
    if (node->truncate) {
        return node->truncate(node, new_size);
    }
    return -30; /* -EROFS */
}

static file_t *open_common(const char *path, int flags, const creds_t *actor, bool executable, uint32_t mode, int *err_out) {
    if (err_out) *err_out = -VFS_EINVAL;
    if (!path) return NULL;

    int access_mode = flags & VFS_O_ACCMODE;
    if (access_mode > 2) {
        return NULL;
    }
    if (flags & ~(VFS_O_RDONLY | VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND | VFS_O_CLOEXEC)) {
        return NULL;
    }
    /* This ABI requires a writable handle for truncation. Reject before
     * lookup/allocation/creation, rather than silently ignoring O_TRUNC. */
    if ((flags & VFS_O_TRUNC) && access_mode == VFS_O_RDONLY) return NULL;

    /* Pre-reserve the file_t descriptor structure before any fallible creation or truncation */
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        if (err_out) *err_out = -VFS_ENOMEM;
        return NULL;
    }

    int lookup_error = 0;
    bool created=false;
    vfs_node_t *node = lookup_common(path, actor, &lookup_error);
    if (!node) {
        if ((flags & VFS_O_CREAT) && lookup_error == -VFS_ENOENT) {
            int create_err = 0;
            if (actor) {
                vfs_create_attrs_t attrs={.mode=mode & 07777u,.uid=actor->euid,.gid=actor->egid};
                node=vfs_create_common(path,VFS_FILE,&create_err,true,&attrs,actor);
            } else node = vfs_create_ref(path, VFS_FILE, &create_err);
            bool own_creation=node!=NULL;
            if (!node && create_err==-VFS_EEXIST) {
                node=lookup_common(path,actor,&create_err);
            }
            if (!node) {
                kfree(file);
                if (err_out) *err_out = create_err ? create_err : -VFS_EIO;
                return NULL;
            }
            created=own_creation;
        } else {
            kfree(file);
            if (err_out) *err_out = lookup_error;
            return NULL;
        }
    }

    if (node->type == VFS_DIRECTORY && access_mode != VFS_O_RDONLY) {
        vfs_node_put(node);
        kfree(file);
        if (err_out) *err_out = -7; /* EISDIR */
        return NULL;
    }

    bool actor_open=actor && node->open_actor;
    if (actor_open) {
#ifdef TEST_PERMISSIONS_INTERLEAVING
        vfs_admission_interleave_test(node,4);
#endif
        unsigned mask=created ? 0 : executable ? VFS_MAY_EXEC :
            access_mode==VFS_O_RDONLY ? VFS_MAY_READ :
            access_mode==VFS_O_WRONLY ? VFS_MAY_WRITE : VFS_MAY_READ|VFS_MAY_WRITE;
        int r=node->open_actor(node,mask,(flags & VFS_O_TRUNC)!=0,actor);
        if (r) { vfs_node_put(node);kfree(file);if (err_out) *err_out=r;return NULL; }
    } else if (actor) {
        unsigned mask=executable ? VFS_MAY_EXEC :
            access_mode==VFS_O_RDONLY ? VFS_MAY_READ :
            access_mode==VFS_O_WRONLY ? VFS_MAY_WRITE : VFS_MAY_READ|VFS_MAY_WRITE;
        if (flags & VFS_O_TRUNC) mask|=VFS_MAY_WRITE;
        int r=vfs_permission(node,mask,actor);
        if (r) { vfs_node_put(node);kfree(file);if (err_out) *err_out=r;return NULL; }
    }
    /* If writing or truncating is requested, ensure node is writable */
    if (!actor_open && (access_mode != VFS_O_RDONLY || (flags & VFS_O_TRUNC))) {
        if (!node->write || !node->truncate) {
            vfs_node_put(node);
            kfree(file);
            if (err_out) *err_out = -VFS_EROFS;
            return NULL;
        }
        if (node->can_write) {
            int can_err = node->can_write(node);
            if (can_err < 0) {
                vfs_node_put(node);
                kfree(file);
                if (err_out) *err_out = can_err;
                return NULL;
            }
        }
    }

    if (!actor_open && node->open) {
        int pin_err = node->open(node);
        if (pin_err < 0) {
            vfs_node_put(node);
            kfree(file);
            if (err_out) *err_out = pin_err;
            return NULL;
        }
    }
    if (!actor_open && (flags & VFS_O_TRUNC) && access_mode != VFS_O_RDONLY) {
        int trunc_res = vfs_truncate(node, 0);
        if (trunc_res < 0) {
            if (node->open && node->close) node->close(node);
            vfs_node_put(node);
            kfree(file);
            if (err_out) *err_out = trunc_res;
            return NULL;
        }
    }

    file->node      = node;
    file->offset    = (flags & VFS_O_APPEND) ? node->size : 0;
    file->flags     = flags;
    file->ref_count = 1;

    if (err_out) *err_out = VFS_SUCCESS;
    return file;
}

file_t *vfs_open_ext(const char *path,int flags,int *error) { return open_common(path,flags,NULL,false,0644,error); }
file_t *vfs_open_creds(const char *path,int flags,const creds_t *actor,int *error) {
    if (!actor) { if (error) *error=-VFS_EINVAL;return NULL; }
    return open_common(path,flags,actor,false,0644,error);
}
file_t *vfs_open_mode_creds(const char *path,int flags,uint32_t mode,const creds_t *actor,int *error) {
    if (!actor) { if (error) *error=-VFS_EINVAL;return NULL; }
    return open_common(path,flags,actor,false,mode,error);
}
file_t *vfs_open_exec_creds(const char *path,const creds_t *actor,int *error) {
    if (!actor) { if (error) *error=-VFS_EINVAL;return NULL; }
    return open_common(path,VFS_O_RDONLY,actor,true,0644,error);
}
int vfs_mkdir_creds(const char *path,uint32_t mode,const creds_t *actor) {
    if (!actor) return -VFS_EINVAL;
    vfs_create_attrs_t attrs={.mode=mode & 07777u,.uid=actor->euid,.gid=actor->egid};
    int r=0;vfs_node_t *n=vfs_create_common(path,VFS_DIRECTORY,&r,true,&attrs,actor);
    if (!n) return r ? r : -VFS_EIO;
    vfs_node_put(n);return 0;
}
int vfs_unlink(const char *path) { return unlink_common(path,NULL); }
int vfs_unlink_creds(const char *path,const creds_t *actor) { return actor ? unlink_common(path,actor) : -VFS_EINVAL; }
int vfs_rename(const char *a,const char *b) { return rename_common(a,b,NULL); }
int vfs_rename_creds(const char *a,const char *b,const creds_t *actor) { return actor ? rename_common(a,b,actor) : -VFS_EINVAL; }
static int vfs_immutable_setattr(vfs_node_t *n,bool chown,uint32_t mode,
                                 creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (n->owns_nodes) return -VFS_EOPNOTSUPP;
    vfs_metadata_t old={.mode=n->mode,.uid=n->uid,.gid=n->gid,.mnt_flags=n->mnt_flags},next;
    int r=chown ? permission_chown_value(&old,uid,gid,actor,&next) :
        permission_chmod_value(&old,mode,actor,&next);
    return r ? r : -VFS_EOPNOTSUPP;
#else
    (void)n;(void)chown;(void)mode;(void)uid;(void)gid;(void)actor;
    return -VFS_EOPNOTSUPP;
#endif
}
int vfs_fchmod_creds(file_t *file,uint32_t mode,const creds_t *actor) {
    if (!file || !file->node) return -VFS_EBADF;
    if (!actor) return -VFS_EINVAL;
    creds_id_change_t keep={.keep=true};
    if (!file->node->setattr_actor) return vfs_immutable_setattr(file->node,false,mode,keep,keep,actor);
    return file->node->setattr_actor(file->node,false,mode,keep,keep,actor);
}
static int vfs_setattr_path(const char *path,bool chown,uint32_t mode,
                             creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
    if (!actor) return -VFS_EINVAL;
    int r=0;vfs_node_t *n=lookup_common(path,actor,&r);if (!n) return r;
    r=n->setattr_actor ? n->setattr_actor(n,chown,mode,uid,gid,actor) :
        vfs_immutable_setattr(n,chown,mode,uid,gid,actor);
    vfs_node_put(n);return r;
}
int vfs_chmod_creds(const char *p,uint32_t mode,const creds_t *actor) {
    creds_id_change_t keep={.keep=true};return vfs_setattr_path(p,false,mode,keep,keep,actor);
}
int vfs_chown_creds(const char *p,creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
    return vfs_setattr_path(p,true,0,uid,gid,actor);
}
int vfs_readdir_creds(vfs_node_t *dir,uint64_t index,vfs_dirent_t *out,const creds_t *actor) {
    if (!actor) return -VFS_EINVAL;
    if (!dir || dir->type!=VFS_DIRECTORY || !out) return -VFS_EINVAL;
    int r=vfs_permission(dir,VFS_MAY_READ,actor);
    return r ? r : vfs_readdir(dir,index,out);
}

file_t *vfs_open(const char *path, int flags) {
    return vfs_open_ext(path, flags, NULL);
}

int64_t vfs_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->node || (!buf && count > 0)) {
        return -1;
    }

    int acc = file->flags & VFS_O_ACCMODE;
    if (acc != VFS_O_RDONLY && acc != VFS_O_RDWR) {
        return -9; /* EBADF - not open for reading */
    }

    if (file->node->type == VFS_DIRECTORY) {
        return -7; /* EISDIR */
    }

    if (count == 0) {
        return 0;
    }

    if (file->node->is_stream) {
        return file->node->read ? file->node->read(file->node, 0, buf, count) : -VFS_EBADF;
    }

    if (file->node->serializes_write_offset && file->node->read) {
        int64_t n=file->node->read(file->node,file->offset,buf,count);
        if (n>0) file->offset+=(uint64_t)n;
        return n;
    }

    if (file->offset >= file->node->size) {
        return 0; /* EOF */
    }

    uint64_t available = file->node->size - file->offset;
    size_t to_read = count;
    if (to_read > available) {
        to_read = (size_t)available;
    }

    if (file->node->read) {
        int64_t result = file->node->read(file->node, file->offset, buf, to_read);
        if (result > 0) file->offset += (uint64_t)result;
        return result;
    }
    if (file->node->data) {
        const uint8_t *src = (const uint8_t *)file->node->data + file->offset;
        memcpy(buf, src, to_read);
    } else {
        memset(buf, 0, to_read);
    }

    file->offset += to_read;
    return (int64_t)to_read;
}

int64_t vfs_write(file_t *file, const void *buf, size_t count) {
    if (!file || !file->node || (!buf && count > 0)) {
        return -1;
    }

    int acc = file->flags & VFS_O_ACCMODE;
    if (acc != VFS_O_WRONLY && acc != VFS_O_RDWR) {
        return -9; /* EBADF - not open for writing */
    }

    if (file->node->type == VFS_DIRECTORY) {
        return -7; /* EISDIR */
    }

    if (count == 0) {
        return 0;
    }

    if (file->node->write) {
        if (file->node->is_stream) {
            uint64_t stream_off = 0;
            return file->node->write(file->node, &stream_off, false, buf, count);
        }
        bool append = (file->flags & VFS_O_APPEND) != 0;
        if (file->node->serializes_write_offset) {
            return file->node->write(file->node, &file->offset, append, buf, count);
        }
        uint64_t write_off = file->offset;
        int64_t result = file->node->write(file->node, &write_off, append, buf, count);
        if (result > 0) {
            file->offset = write_off;
            if (!file->node->owns_nodes && file->offset > file->node->size) {
                file->node->size = file->offset;
            }
        }
        return result;
    }

    return -30; /* -EROFS */
}
int64_t vfs_write_creds(file_t *file,const void *buf,size_t count,const creds_t *actor) {
    if (!actor) return -VFS_EINVAL;
    if (!file || !file->node) return -VFS_EBADF;
    int acc=file->flags & VFS_O_ACCMODE;
    if (acc!=VFS_O_WRONLY && acc!=VFS_O_RDWR) return -VFS_EBADF;
    if (file->node->type==VFS_DIRECTORY) return -7;
    if (!buf && count) return -VFS_EINVAL;
    if (!count) return 0;
    if (file->node->write_actor)
        return file->node->write_actor(file->node,&file->offset,(file->flags & VFS_O_APPEND)!=0,buf,count,actor);
    return vfs_write(file,buf,count);
}

int vfs_close(file_t *file) {
    if (!file) {
        return -1;
    }

    if (__atomic_sub_fetch(&file->ref_count, 1, __ATOMIC_ACQ_REL) <= 0) {
        void (*put)(vfs_node_t *)=file->node ? file->node->put : NULL;
        if (file->node && file->node->close) file->node->close(file->node);
        if (put) put(file->node);
        kfree(file);
    }

    return 0;
}

int vfs_metadata(vfs_node_t *node,vfs_metadata_t *out) {
    if (!node || !out) return -VFS_EINVAL;
    if (node->metadata) return node->metadata(node,out);
    *out=(vfs_metadata_t){.size=node->size,.type=node->type,.mode=node->mode,
        .uid=node->uid,.gid=node->gid,.mnt_flags=node->mnt_flags};
    return 0;
}

int vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat) {
    if (!node || !out_stat) {
        return -1;
    }

    vfs_metadata_t value;
    int r=vfs_metadata(node,&value);if (r) return r;
    out_stat->size=value.size;
    out_stat->type=value.type;
    out_stat->mode=value.mode;
    return 0;
}

int vfs_readdir(vfs_node_t *dir_node, uint64_t index, vfs_dirent_t *out_dent) {
    if (!dir_node || dir_node->type != VFS_DIRECTORY || !out_dent) {
        return -1;
    }
    if (dir_node->readdir) return dir_node->readdir(dir_node, index, out_dent);

    vfs_node_t *child = dir_node->children;
    uint64_t curr_idx = 0;

    while (child && curr_idx < index) {
        child = child->next;
        curr_idx++;
    }

    if (!child) {
        return 0; /* End of directory */
    }

    memcpy(out_dent->name, child->name, strlen(child->name) + 1);
    out_dent->type = (uint32_t)child->type;
    out_dent->size = child->size;
    return 1; /* Entry read */
}

int vfs_readdir_file(file_t *file, vfs_dirent_t *out) {
    if (!file || !file->node) return -VFS_EBADF;
    int access = file->flags & VFS_O_ACCMODE;
    if (access != VFS_O_RDONLY && access != VFS_O_RDWR) return -VFS_EBADF;
    if (file->node->type != VFS_DIRECTORY) return -VFS_ENOTDIR;
    if (!out) return -VFS_EINVAL;
    /* Open admitted READ. Metadata changes do not revoke descriptor rights.
     * The filesystem callback still owns lifetime and coherent enumeration. */
    int result = vfs_readdir(file->node, file->offset, out);
    if (result == 1) file->offset++;
    return result;
}
