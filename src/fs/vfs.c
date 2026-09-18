#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

static vfs_node_t *g_vfs_root = NULL;

void vfs_init(void) {
    if (g_vfs_root) return;

    g_vfs_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!g_vfs_root) {
        serial_puts("[FATAL] VFS: Failed to allocate root node\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    memset(g_vfs_root, 0, sizeof(vfs_node_t));
    g_vfs_root->name[0] = '/';
    g_vfs_root->name[1] = '\0';
    g_vfs_root->path[0] = '/';
    g_vfs_root->path[1] = '\0';
    g_vfs_root->type    = VFS_DIRECTORY;

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
            memcpy(new_node->name, comp, strlen(comp) + 1);
            memcpy(new_node->path, norm, strlen(norm) + 1);
            new_node->type   = type;
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
            memcpy(dir_node->name, comp, strlen(comp) + 1);
            /* Build path */
            size_t curr_len = strlen(curr->path);
            memcpy(dir_node->path, curr->path, curr_len);
            if (curr_len > 1) dir_node->path[curr_len++] = '/';
            memcpy(dir_node->path + curr_len, comp, strlen(comp) + 1);

            dir_node->type   = VFS_DIRECTORY;
            dir_node->parent = curr;
            dir_node->next   = curr->children;
            curr->children   = dir_node;
            found = dir_node;
        }

        curr = found;
    }

    return curr;
}

static int g_last_create_error = -VFS_EIO;
void vfs_set_last_create_error(int err) {
    g_last_create_error = err;
}
int vfs_get_last_create_error(void) {
    return g_last_create_error;
}

vfs_node_t *vfs_create_ext(const char *path, vfs_node_type_t type, int *err_out) {
    if (err_out) *err_out = -VFS_EINVAL;
    if (!path || strlen(path) >= VFS_MAX_PATH) return NULL;
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

    vfs_node_t *dir = vfs_lookup(dir_path);
    if (!dir) {
        if (err_out) *err_out = -VFS_ENOENT;
        return NULL;
    }
    if (dir->type != VFS_DIRECTORY) {
        if (err_out) *err_out = -8; /* ENOTDIR */
        return NULL;
    }
    if (!dir->create) {
        if (err_out) *err_out = -VFS_EROFS;
        return NULL;
    }
    vfs_node_t *res = dir->create(dir, name, type);
    if (!res) {
        if (err_out) *err_out = vfs_get_last_create_error();
        return NULL;
    }
    if (err_out) *err_out = VFS_SUCCESS;
    return res;
}

vfs_node_t *vfs_create(const char *path, vfs_node_type_t type) {
    return vfs_create_ext(path, type, NULL);
}

int vfs_mkdir(const char *path, uint32_t mode) {
    (void)mode;
    int err = 0;
    vfs_node_t *node = vfs_create_ext(path, VFS_DIRECTORY, &err);
    if (!node) return err ? err : -VFS_EIO;
    return VFS_SUCCESS;
}

int vfs_unlink(const char *path) {
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

    vfs_node_t *dir = vfs_lookup(dir_path);
    if (!dir) return -VFS_ENOENT;
    if (dir->type != VFS_DIRECTORY) return -8; /* ENOTDIR */

    vfs_node_t *target = vfs_lookup(norm);
    if (!target) return -VFS_ENOENT;

    if (!dir->unlink) return -VFS_EROFS;

    int res = dir->unlink(dir, name);
    if (res != 0) return res;

    /* Unlink succeeded in filesystem; detach target from VFS child tree */
    vfs_node_t **curr = &dir->children;
    while (*curr) {
        if (*curr == target) {
            *curr = target->next;
            break;
        }
        curr = &(*curr)->next;
    }
    kfree(target);
    return VFS_SUCCESS;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -VFS_EINVAL;
    if (strlen(oldpath) >= VFS_MAX_PATH || strlen(newpath) >= VFS_MAX_PATH) return -VFS_EINVAL;

    char norm_old[VFS_MAX_PATH], norm_new[VFS_MAX_PATH];
    normalize_path(oldpath, norm_old, sizeof(norm_old));
    normalize_path(newpath, norm_new, sizeof(norm_new));

    if (!strcmp(norm_old, "/") || !strcmp(norm_new, "/")) return -VFS_EPERM;
    if (!strcmp(norm_old, norm_new)) return VFS_SUCCESS;

    /* Check prefix: cannot move a directory inside itself */
    size_t old_len = strlen(norm_old);
    if (!strncmp(norm_new, norm_old, old_len) && (norm_new[old_len] == '/' || norm_new[old_len] == '\0')) {
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

    vfs_node_t *old_dir = vfs_lookup(old_dir_path);
    vfs_node_t *new_dir = vfs_lookup(new_dir_path);
    if (!old_dir || !new_dir) return -VFS_ENOENT;
    if (old_dir->type != VFS_DIRECTORY || new_dir->type != VFS_DIRECTORY) return -8; /* ENOTDIR */

    size_t oldpath_len = strlen(oldpath);
    size_t newpath_len = strlen(newpath);
    bool old_has_slash = (oldpath_len > 1 && oldpath[oldpath_len - 1] == '/');
    bool new_has_slash = (newpath_len > 1 && newpath[newpath_len - 1] == '/');

    vfs_node_t *target = vfs_lookup(norm_old);
    if (!target) return -VFS_ENOENT;
    if (target->type != VFS_DIRECTORY && (old_has_slash || new_has_slash)) {
        return -8; /* ENOTDIR */
    }

    if (!old_dir->rename || old_dir->rename != new_dir->rename) return -VFS_EROFS;

    /* If destination already exists in VFS, verify types and unlink/replace */
    vfs_node_t *dest = vfs_lookup(norm_new);
    if (dest) {
        if (dest == target) return VFS_SUCCESS;
        if (dest->type == VFS_DIRECTORY && target->type != VFS_DIRECTORY) return -7; /* EISDIR */
        if (dest->type != VFS_DIRECTORY && target->type == VFS_DIRECTORY) return -8; /* ENOTDIR */
        int ures = vfs_unlink(norm_new);
        if (ures != 0) return ures;
    }

    int res = old_dir->rename(old_dir, old_name, new_dir, new_name);
    if (res != 0) return res;

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

file_t *vfs_open_ext(const char *path, int flags, int *err_out) {
    if (err_out) *err_out = -VFS_EINVAL;
    if (!path) return NULL;

    int access_mode = flags & VFS_O_ACCMODE;
    if (access_mode > 2) {
        return NULL;
    }
    if (flags & ~(VFS_O_RDONLY | VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) {
        return NULL;
    }
    if (flags & VFS_O_APPEND) {
        /* O_APPEND is unsupported in Phase 9D bounded phase to prevent seek/write races */
        if (err_out) *err_out = -VFS_EINVAL;
        return NULL;
    }

    /* Pre-reserve the file_t descriptor structure before any fallible creation or truncation */
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        if (err_out) *err_out = -VFS_ENOMEM;
        return NULL;
    }

    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        if (flags & VFS_O_CREAT) {
            int create_err = 0;
            node = vfs_create_ext(path, VFS_FILE, &create_err);
            if (!node) {
                kfree(file);
                if (err_out) *err_out = create_err ? create_err : -VFS_EIO;
                return NULL;
            }
        } else {
            kfree(file);
            if (err_out) *err_out = -VFS_ENOENT;
            return NULL;
        }
    }

    if (node->type == VFS_DIRECTORY && access_mode != VFS_O_RDONLY) {
        kfree(file);
        if (err_out) *err_out = -7; /* EISDIR */
        return NULL;
    }

    /* If writing or truncating is requested, ensure node is writable */
    if (access_mode != VFS_O_RDONLY || (flags & VFS_O_TRUNC)) {
        if (!node->write || !node->truncate) {
            kfree(file);
            if (err_out) *err_out = -VFS_EROFS;
            return NULL;
        }
    }

    if ((flags & VFS_O_TRUNC) && access_mode != VFS_O_RDONLY) {
        int trunc_res = vfs_truncate(node, 0);
        if (trunc_res < 0) {
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
        int64_t result = file->node->write(file->node, file->offset, buf, count);
        if (result > 0) {
            file->offset += (uint64_t)result;
            if (file->offset > file->node->size) {
                file->node->size = file->offset;
            }
        }
        return result;
    }

    return -30; /* -EROFS */
}

int vfs_close(file_t *file) {
    if (!file) {
        return -1;
    }

    file->ref_count--;
    if (file->ref_count <= 0) {
        kfree(file);
    }

    return 0;
}

int vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat) {
    if (!node || !out_stat) {
        return -1;
    }

    out_stat->size = node->size;
    out_stat->type = (uint32_t)node->type;
    out_stat->mode = (node->type == VFS_DIRECTORY) ? 0555 : 0444;
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
