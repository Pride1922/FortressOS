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

file_t *vfs_open(const char *path, int flags) {
    if (flags != 0) return NULL; /* All current filesystems are read-only. */
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        return NULL;
    }

    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        return NULL;
    }

    file->node      = node;
    file->offset    = 0;
    file->flags     = flags;
    file->ref_count = 1;

    return file;
}

int64_t vfs_read(file_t *file, void *buf, size_t count) {
    if (!file || !file->node || !buf) {
        return -1;
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
