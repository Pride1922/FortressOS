#include "etcfs.h"
#include "spinlock.h"
#include "string.h"
#include "heap.h"
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
#include "permission_values.h"
#endif
#include "serial.h"

/*
 * ETCFS: Ephemeral in-memory writable configuration tree.
 * Enforces a strict 16 MiB aggregate memory ceiling across all dynamic buffers.
 * Protects /etc/passwd, /etc/group, and /etc/shadow as immutable VFS_MNT_RDONLY.
 */

typedef struct etc_file_entry {
    uint8_t *buffer;
    size_t   capacity;
    uint32_t refs;
    bool     removed;
    bool     is_dynamic_node;
} etc_file_entry_t;

static spinlock_t s_etc_lock = SPINLOCK_RANKED(1, "etcfs");
static size_t s_etcfs_allocated_bytes = 0;

size_t etcfs_allocated_bytes(void) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    size_t bytes = s_etcfs_allocated_bytes;
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return bytes;
}

static inline etc_file_entry_t *etc_entry(vfs_node_t *node) {
    return node ? (etc_file_entry_t *)node->fs_private : NULL;
}

static bool etc_is_protected_account(const char *path) {
    if (!path) return false;
    return !strcmp(path, "/etc/passwd") ||
           !strcmp(path, "/etc/group") ||
           !strcmp(path, "/etc/shadow");
}

static void etc_collect(vfs_node_t *node) {
    etc_file_entry_t *entry = etc_entry(node);
    if (!entry) return;
    if (entry->removed && !entry->refs) {
        if (entry->buffer) {
            kfree(entry->buffer);
            s_etcfs_allocated_bytes -= entry->capacity;
            entry->buffer = NULL;
        }
        bool is_dyn = entry->is_dynamic_node;
        kfree(entry);
        node->fs_private = NULL;
        if (is_dyn) {
            kfree(node);
        }
    }
}

static int etc_get(vfs_node_t *node) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *entry = etc_entry(node);
    int r = 0;
    if (entry) {
        if (entry->removed || entry->refs == UINT32_MAX) r = -VFS_EIO;
        else entry->refs++;
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

static void etc_put(vfs_node_t *node) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *entry = etc_entry(node);
    if (entry) {
        if (entry->refs) entry->refs--;
        etc_collect(node);
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
}

static vfs_metadata_t etc_value_locked(vfs_node_t *n) {
    return (vfs_metadata_t){
        .size = n->size,
        .type = n->type,
        .mode = n->mode,
        .uid = n->uid,
        .gid = n->gid,
        .mnt_flags = n->mnt_flags
    };
}

static int etc_metadata(vfs_node_t *node, vfs_metadata_t *out) {
    if (!node || !out) return -VFS_EINVAL;
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    *out = etc_value_locked(node);
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return 0;
}

static int etc_access_locked(vfs_node_t *n, unsigned mask, const creds_t *actor) {
    etc_file_entry_t *e = etc_entry(n);
    if (e && e->removed) return -VFS_ENOENT;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (actor) {
        vfs_metadata_t val = etc_value_locked(n);
        return permission_access_value(&val, mask, actor);
    }
#else
    (void)mask; (void)actor;
#endif
    return 0;
}

static int etc_permission_actor(vfs_node_t *node, unsigned mask, const creds_t *actor) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = etc_access_locked(node, mask, actor);
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

static vfs_node_t *etc_find_locked(vfs_node_t *dir, const char *name) {
    for (vfs_node_t *n = dir->children; n; n = n->next) {
        etc_file_entry_t *e = etc_entry(n);
        if ((!e || !e->removed) && !strcmp(n->name, name)) {
            return n;
        }
    }
    return NULL;
}

static vfs_node_t *etc_lookup_common(vfs_node_t *dir, const char *name, int *error, bool owned) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *de = etc_entry(dir);
    vfs_node_t *n = (de && de->removed) ? NULL :
                    !strcmp(name, ".") ? dir :
                    !strcmp(name, "..") ? dir->parent :
                    etc_find_locked(dir, name);
    int r = -VFS_ENOENT;
    if (n) {
        etc_file_entry_t *ne = etc_entry(n);
        if (ne && ne->removed) {
            n = NULL;
            r = -VFS_ENOENT;
        } else if (owned && ne && ne->refs == UINT32_MAX) {
            n = NULL;
            r = -VFS_EIO;
        } else {
            if (owned && ne) ne->refs++;
            r = 0;
        }
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    if (error) *error = r;
    return n;
}

static vfs_node_t *etc_lookup(vfs_node_t *dir, const char *name) {
    return etc_lookup_common(dir, name, NULL, false);
}

static vfs_node_t *etc_lookup_ref(vfs_node_t *dir, const char *name, int *error) {
    return etc_lookup_common(dir, name, error, true);
}

static vfs_node_t *etc_lookup_actor(vfs_node_t *dir, const char *name, const creds_t *actor, int *error) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = etc_access_locked(dir, VFS_MAY_EXEC, actor);
    vfs_node_t *n = NULL;
    if (!r) {
        if (!strcmp(name, ".")) n = dir;
        else if (!strcmp(name, "..")) n = dir->parent;
        else n = etc_find_locked(dir, name);

        if (!n) r = -VFS_ENOENT;
        else {
            etc_file_entry_t *ne = etc_entry(n);
            if (ne && ne->removed) { n = NULL; r = -VFS_ENOENT; }
            else if (ne) {
                if (ne->refs == UINT32_MAX) { n = NULL; r = -VFS_EIO; }
                else ne->refs++;
            }
        }
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    if (error) *error = r;
    return r ? NULL : n;
}

static int64_t etc_read_file(vfs_node_t *node, uint64_t off, void *buf, size_t len) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    if (off >= node->size) len = 0;
    else if (len > node->size - off) len = (size_t)(node->size - off);

    if (len) {
        etc_file_entry_t *entry = etc_entry(node);
        const void *src = (entry && entry->buffer) ? entry->buffer : node->data;
        if (src) memcpy(buf, (const uint8_t *)src + off, len);
        else memset(buf, 0, len);
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return (int64_t)len;
}

static int64_t etc_read_offset(vfs_node_t *node, uint64_t *off, void *buf, size_t len) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    uint64_t pos = *off;
    if (pos >= node->size) len = 0;
    else if (len > node->size - pos) len = (size_t)(node->size - pos);

    if (len) {
        etc_file_entry_t *entry = etc_entry(node);
        const void *src = (entry && entry->buffer) ? entry->buffer : node->data;
        if (src) memcpy(buf, (const uint8_t *)src + pos, len);
        else memset(buf, 0, len);
        *off = pos + len;
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return (int64_t)len;
}

static int etc_ensure_capacity_locked(vfs_node_t *node, size_t needed) {
    if (etc_is_protected_account(node->path)) return -VFS_EROFS;

    etc_file_entry_t *entry = etc_entry(node);
    if (!entry) {
        entry = (etc_file_entry_t *)kmalloc(sizeof(etc_file_entry_t));
        if (!entry) return -VFS_ENOMEM;
        memset(entry, 0, sizeof(*entry));
        node->fs_private = entry;
    }

    if (needed <= entry->capacity && entry->buffer) {
        return 0;
    }

    /* Grow buffer: align up to 256 bytes */
    size_t new_cap = (needed + 255) & ~255ULL;
    if (new_cap < 512) new_cap = 512;
    size_t delta = new_cap - entry->capacity;
    if (s_etcfs_allocated_bytes + delta > ETCFS_MAX_BYTES) {
        return -VFS_ENOSPC;
    }

    uint8_t *new_buf = (uint8_t *)kmalloc(new_cap);
    if (!new_buf) return -VFS_ENOMEM;
    memset(new_buf, 0, new_cap);

    if (node->size > 0) {
        const void *src = entry->buffer ? entry->buffer : node->data;
        if (src) {
            size_t copy_len = node->size < new_cap ? (size_t)node->size : new_cap;
            memcpy(new_buf, src, copy_len);
        }
    }

    if (entry->buffer) {
        kfree(entry->buffer);
    }

    entry->buffer = new_buf;
    entry->capacity = new_cap;
    node->data = new_buf;
    s_etcfs_allocated_bytes += delta;
    return 0;
}

static int64_t etc_write_file(vfs_node_t *node, uint64_t *offset, bool append, const void *buf, size_t len) {
    if (etc_is_protected_account(node->path)) return -VFS_EROFS;
    if (!len) return 0;

    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    uint64_t off = append ? node->size : *offset;
    size_t needed = (size_t)(off + len);

    int err = etc_ensure_capacity_locked(node, needed);
    if (err) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return err;
    }

    etc_file_entry_t *entry = etc_entry(node);
    memcpy(entry->buffer + off, buf, len);
    *offset = off + len;
    if (off + len > node->size) {
        node->size = off + len;
    }

    spin_unlock_irqrestore(&s_etc_lock, irq);
    return (int64_t)len;
}

static int64_t etc_write_actor(vfs_node_t *node, uint64_t *offset, bool append, const void *buf, size_t len, const creds_t *actor) {
    if (etc_is_protected_account(node->path)) return -VFS_EROFS;
    if (!len) return 0;

    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *entry = etc_entry(node);
    if (entry && entry->removed) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return -VFS_ENOENT;
    }

    uint64_t off = append ? node->size : *offset;
    size_t needed = (size_t)(off + len);

    int err = etc_ensure_capacity_locked(node, needed);
    if (err) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return err;
    }

    entry = etc_entry(node);
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    vfs_metadata_t val = etc_value_locked(node);
    uint16_t new_mode = actor ? permission_content_mode(&val, actor) : node->mode;
#else
    uint16_t new_mode = node->mode;
    (void)actor;
#endif

    memcpy(entry->buffer + off, buf, len);
    *offset = off + len;
    if (off + len > node->size) {
        node->size = off + len;
    }
    node->mode = new_mode;

    spin_unlock_irqrestore(&s_etc_lock, irq);
    return (int64_t)len;
}

static int etc_truncate_file(vfs_node_t *node, uint64_t size) {
    if (etc_is_protected_account(node->path)) return -VFS_EROFS;

    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *entry = etc_entry(node);

    if (size == 0) {
        if (entry && entry->buffer) {
            kfree(entry->buffer);
            s_etcfs_allocated_bytes -= entry->capacity;
            entry->buffer = NULL;
            entry->capacity = 0;
        }
        node->size = 0;
        node->data = NULL;
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return 0;
    }

    int err = etc_ensure_capacity_locked(node, (size_t)size);
    if (!err) {
        entry = etc_entry(node);
        if (size > node->size) {
            memset(entry->buffer + node->size, 0, (size_t)(size - node->size));
        }
        node->size = size;
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return err;
}

static int etc_open_actor(vfs_node_t *node, unsigned mask, bool truncate, const creds_t *actor) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = etc_access_locked(node, mask, actor);
    if (!r && truncate) {
        if (node->type != VFS_FILE) {
            r = -VFS_EINVAL;
        } else if (etc_is_protected_account(node->path)) {
            r = -VFS_EROFS;
        } else {
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
            vfs_metadata_t val = etc_value_locked(node);
            uint16_t mode = actor ? permission_content_mode(&val, actor) : node->mode;
#else
            uint16_t mode = node->mode;
            (void)actor;
#endif
            etc_file_entry_t *entry = etc_entry(node);
            if (entry && entry->buffer) {
                kfree(entry->buffer);
                s_etcfs_allocated_bytes -= entry->capacity;
                entry->buffer = NULL;
                entry->capacity = 0;
            }
            node->size = 0;
            node->data = NULL;
            node->mode = mode;
        }
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

static int etc_setattr_actor(vfs_node_t *n, bool chown, uint32_t mode,
                             creds_id_change_t uid, creds_id_change_t gid, const creds_t *actor) {
    if (etc_is_protected_account(n->path)) return -VFS_EPERM;
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *entry = etc_entry(n);
    int r = -VFS_ENOENT;
    if (!entry || !entry->removed) {
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
        vfs_metadata_t old = etc_value_locked(n), next;
        r = chown ? permission_chown_value(&old, uid, gid, actor, &next) :
                    permission_chmod_value(&old, mode, actor, &next);
        if (!r) {
            n->mode = next.mode;
            n->uid = next.uid;
            n->gid = next.gid;
        }
#else
        (void)actor;
        if (chown) {
            if (!uid.keep) n->uid = uid.value;
            if (!gid.keep) n->gid = gid.value;
        } else {
            n->mode = (n->mode & VFS_S_IFMT) | (mode & 07777u);
        }
        r = 0;
#endif
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

static int etc_readdir_node(vfs_node_t *dir, uint64_t index, void *output) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    vfs_node_t *n = dir->children;
    while (n) {
        etc_file_entry_t *e = etc_entry(n);
        if (!e || !e->removed) {
            if (index == 0) break;
            index--;
        }
        n = n->next;
    }
    if (n) {
        vfs_dirent_t *out = (vfs_dirent_t *)output;
        memset(out, 0, sizeof(*out));
        memcpy(out->name, n->name, strlen(n->name) + 1);
        out->type = n->type;
        out->size = n->size;
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return n ? 1 : 0;
}

static int etc_unlink_common(vfs_node_t *dir, const char *name, bool require_directory, const creds_t *actor) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = actor ? etc_access_locked(dir, VFS_MAY_WRITE | VFS_MAY_EXEC, actor) : 0;
    if (r) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return r;
    }

    vfs_node_t **link = &dir->children;
    while (*link) {
        etc_file_entry_t *e = etc_entry(*link);
        if ((!e || !e->removed) && !strcmp((*link)->name, name)) break;
        link = &(*link)->next;
    }

    if (!*link) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return -VFS_ENOENT;
    }

    vfs_node_t *victim = *link;
    if (etc_is_protected_account(victim->path)) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return -VFS_EPERM;
    }

    if (require_directory && victim->type != VFS_DIRECTORY) {
        spin_unlock_irqrestore(&s_etc_lock, irq);
        return -VFS_ENOTDIR;
    }

    if (victim->type == VFS_DIRECTORY && victim->children) {
        /* Check if any non-removed children exist */
        bool has_children = false;
        for (vfs_node_t *c = victim->children; c; c = c->next) {
            etc_file_entry_t *ce = etc_entry(c);
            if (!ce || !ce->removed) { has_children = true; break; }
        }
        if (has_children) {
            spin_unlock_irqrestore(&s_etc_lock, irq);
            return -VFS_ENOTEMPTY;
        }
    }

#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (actor) {
        vfs_metadata_t d = etc_value_locked(dir), v = etc_value_locked(victim);
        r = permission_delete_value(&d, &v, actor);
        if (r) {
            spin_unlock_irqrestore(&s_etc_lock, irq);
            return r;
        }
    }
#else
    (void)actor;
#endif

    *link = victim->next;
    victim->next = NULL;

    etc_file_entry_t *ve = etc_entry(victim);
    if (ve) {
        ve->removed = true;
        etc_collect(victim);
    }

    spin_unlock_irqrestore(&s_etc_lock, irq);
    return 0;
}

static int etc_unlink_node(vfs_node_t *dir, const char *name) {
    return etc_unlink_common(dir, name, false, NULL);
}

static int etc_unlink_actor(vfs_node_t *dir, const char *name, bool require_directory, const creds_t *actor) {
    return etc_unlink_common(dir, name, require_directory, actor);
}

static int etc_exec_snapshot(vfs_node_t *node, const creds_t *actor, vfs_metadata_t *out, void **bytes) {
    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = actor ? etc_access_locked(node, VFS_MAY_EXEC, actor) : 0;
    void *copy = NULL;
    if (!r && (node->type != VFS_FILE || !node->size)) r = -VFS_EINVAL;
    if (!r) {
        copy = kmalloc((size_t)node->size);
        if (!copy) r = -VFS_ENOMEM;
        else {
            etc_file_entry_t *entry = etc_entry(node);
            const void *src = (entry && entry->buffer) ? entry->buffer : node->data;
            if (src) memcpy(copy, src, (size_t)node->size);
            else memset(copy, 0, (size_t)node->size);
            *out = etc_value_locked(node);
            *bytes = copy;
        }
    }
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

/* Forward declarations for setup */
static void etc_setup_directory(vfs_node_t *dir);
static void etc_setup_file(vfs_node_t *file);

static vfs_node_t *etc_create_common(vfs_node_t *dir, const char *name, vfs_node_type_t type,
                                     const vfs_create_attrs_t *a, int *error, bool owned, const creds_t *actor) {
    int r = -VFS_EINVAL;
    vfs_node_t *out = NULL;

    if (!name || !*name || strlen(name) >= VFS_MAX_NAME || !strcmp(name, ".") || !strcmp(name, "..") ||
        (type != VFS_FILE && type != VFS_DIRECTORY) || (a && (a->mode & ~07777u))) {
        goto done;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') goto done;
    }
    if (strlen(dir->path) + strlen(name) + 2 > VFS_MAX_PATH) {
        goto done;
    }

    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    etc_file_entry_t *de = etc_entry(dir);
    if (de && de->removed) {
        r = -VFS_ENOENT;
        spin_unlock_irqrestore(&s_etc_lock, irq);
        goto done;
    }

    if (actor) {
        r = etc_access_locked(dir, VFS_MAY_WRITE | VFS_MAY_EXEC, actor);
        if (r) {
            spin_unlock_irqrestore(&s_etc_lock, irq);
            goto done;
        }
    }

    if (etc_find_locked(dir, name)) {
        r = -VFS_EEXIST;
        spin_unlock_irqrestore(&s_etc_lock, irq);
        goto done;
    }

    vfs_create_attrs_t attrs = a ? *a : (vfs_create_attrs_t){
        .mode = type == VFS_DIRECTORY ? 0755u : 0644u,
        .uid = actor ? actor->euid : 0,
        .gid = actor ? actor->egid : 0
    };

#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (actor) {
        vfs_metadata_t dval = etc_value_locked(dir);
        uint32_t req_mode = a ? a->mode : (type == VFS_DIRECTORY ? 0755u : 0644u);
        r = permission_create_value(&dval, type, req_mode, actor, &attrs);
        if (r) {
            spin_unlock_irqrestore(&s_etc_lock, irq);
            goto done;
        }
    }
#else
    (void)actor;
#endif

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    etc_file_entry_t *entry = (etc_file_entry_t *)kmalloc(sizeof(etc_file_entry_t));
    if (!node || !entry) {
        if (node) kfree(node);
        if (entry) kfree(entry);
        r = -VFS_ENOMEM;
        spin_unlock_irqrestore(&s_etc_lock, irq);
        goto done;
    }

    memset(node, 0, sizeof(*node));
    memset(entry, 0, sizeof(*entry));
    entry->is_dynamic_node = true;
    entry->refs = owned ? 1 : 0;

    node->type = type;
    node->parent = dir;
    node->mode = (type == VFS_DIRECTORY ? VFS_S_IFDIR : VFS_S_IFREG) | (attrs.mode & 07777u);
    node->uid = attrs.uid;
    node->gid = attrs.gid;
    node->fs_private = entry;
    node->owns_nodes = true;
    node->serializes_write_offset = true;

    memcpy(node->name, name, strlen(name) + 1);
    size_t dlen = strlen(dir->path);
    memcpy(node->path, dir->path, dlen);
    if (dlen > 1) node->path[dlen++] = '/';
    memcpy(node->path + dlen, name, strlen(name) + 1);

    if (type == VFS_DIRECTORY) {
        etc_setup_directory(node);
    } else {
        etc_setup_file(node);
    }

    node->next = dir->children;
    dir->children = node;
    out = node;
    r = 0;

    spin_unlock_irqrestore(&s_etc_lock, irq);

done:
    if (error) *error = r;
    if (!out) vfs_set_last_create_error(r);
    return out;
}

static vfs_node_t *etc_create(vfs_node_t *d, const char *n, vfs_node_type_t t) {
    return etc_create_common(d, n, t, NULL, NULL, false, NULL);
}

static vfs_node_t *etc_create_ref(vfs_node_t *d, const char *n, vfs_node_type_t t, int *e) {
    return etc_create_common(d, n, t, NULL, e, true, NULL);
}

static vfs_node_t *etc_create_attrs(vfs_node_t *d, const char *n, vfs_node_type_t t, const vfs_create_attrs_t *a, int *e) {
    return etc_create_common(d, n, t, a, e, true, NULL);
}

static vfs_node_t *etc_create_actor(vfs_node_t *dir, const char *name, vfs_node_type_t type,
                                    uint32_t mode, const creds_t *actor, int *error) {
    vfs_create_attrs_t a = {.mode = (uint16_t)(mode & 07777u), .uid = actor ? actor->euid : 0, .gid = actor ? actor->egid : 0};
    return etc_create_common(dir, name, type, &a, error, true, actor);
}

static void etc_update_paths(vfs_node_t *node, const char *parent_path) {
    size_t plen = strlen(parent_path);
    memcpy(node->path, parent_path, plen);
    if (plen > 1) node->path[plen++] = '/';
    size_t nlen = strlen(node->name);
    memcpy(node->path + plen, node->name, nlen + 1);
    for (vfs_node_t *c = node->children; c; c = c->next) {
        etc_update_paths(c, node->path);
    }
}

static int etc_rename_actor(vfs_node_t *old_dir, const char *name,
                            vfs_node_t *dest_dir, const char *next,
                            bool require_directory, const creds_t *actor) {
    if (!*name || !*next || !strcmp(name, ".") || !strcmp(name, "..") ||
        !strcmp(next, ".") || !strcmp(next, "..") || strlen(next) >= VFS_MAX_NAME) {
        return -VFS_EINVAL;
    }
    for (const char *p = next; *p; p++) if (*p == '/') return -VFS_EINVAL;

    uint64_t irq = spin_lock_irqsave(&s_etc_lock);
    int r = actor ? etc_access_locked(old_dir, VFS_MAY_WRITE | VFS_MAY_EXEC, actor) : 0;
    if (!r && actor) r = etc_access_locked(dest_dir, VFS_MAY_WRITE | VFS_MAY_EXEC, actor);

    vfs_node_t *source = etc_find_locked(old_dir, name);
    vfs_node_t *victim = etc_find_locked(dest_dir, next);

    if (!r && !source) r = -VFS_ENOENT;
    if (!r && (etc_is_protected_account(source->path) ||
               (victim && etc_is_protected_account(victim->path)))) {
        r = -VFS_EPERM;
    }
    if (!r && require_directory && source->type != VFS_DIRECTORY) r = -VFS_ENOTDIR;

#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (!r && actor) {
        vfs_metadata_t d = etc_value_locked(old_dir), v = etc_value_locked(source);
        r = permission_delete_value(&d, &v, actor);
    }
    if (!r && victim && actor) {
        vfs_metadata_t d = etc_value_locked(dest_dir), v = etc_value_locked(victim);
        r = permission_delete_value(&d, &v, actor);
    }
#else
    (void)actor;
#endif

    if (!r && source == victim) goto done;

    if (!r && source->type == VFS_DIRECTORY && old_dir != dest_dir && actor) {
        r = etc_access_locked(source, VFS_MAY_WRITE, actor);
    }
    if (!r && victim && source->type != victim->type) {
        r = victim->type == VFS_DIRECTORY ? -7 : -VFS_ENOTDIR;
    }
    if (!r && victim && victim->type == VFS_DIRECTORY && victim->children) {
        bool has_children = false;
        for (vfs_node_t *c = victim->children; c; c = c->next) {
            etc_file_entry_t *ce = etc_entry(c);
            if (!ce || !ce->removed) { has_children = true; break; }
        }
        if (has_children) r = -VFS_ENOTEMPTY;
    }
    if (!r) {
        for (vfs_node_t *p = dest_dir; p; p = p->parent) {
            if (p == source) { r = -VFS_EINVAL; break; }
        }
    }

    if (!r) {
        /* Unlink victim if present */
        if (victim) {
            vfs_node_t **link = &dest_dir->children;
            while (*link && *link != victim) link = &(*link)->next;
            if (*link) {
                *link = victim->next;
                victim->next = NULL;
                etc_file_entry_t *ve = etc_entry(victim);
                if (ve) {
                    ve->removed = true;
                    etc_collect(victim);
                }
            }
        }

        /* Move source from old_dir to dest_dir */
        vfs_node_t **link = &old_dir->children;
        while (*link && *link != source) link = &(*link)->next;
        if (*link) *link = source->next;

        source->parent = dest_dir;
        source->next = dest_dir->children;
        dest_dir->children = source;
        memcpy(source->name, next, strlen(next) + 1);

        etc_update_paths(source, dest_dir->path);
    }

done:
    spin_unlock_irqrestore(&s_etc_lock, irq);
    return r;
}

static int etc_rename(vfs_node_t *old_dir, const char *old_name, vfs_node_t *new_dir, const char *new_name) {
    return etc_rename_actor(old_dir, old_name, new_dir, new_name, false, NULL);
}

static int etc_replace(vfs_node_t *old_dir, const char *old_name,
                       vfs_node_t *new_dir, const char *new_name,
                       const creds_t *actor) {
    return etc_rename_actor(old_dir, old_name, new_dir, new_name, false, actor);
}

static void etc_setup_directory(vfs_node_t *dir) {
    dir->mnt_flags = 0; /* writable */
    dir->lookup = etc_lookup;
    dir->lookup_ref = etc_lookup_ref;
    dir->lookup_actor = etc_lookup_actor;
    dir->create = etc_create;
    dir->create_ref = etc_create_ref;
    dir->create_attrs_ref = etc_create_attrs;
    dir->create_actor = etc_create_actor;
    dir->unlink = etc_unlink_node;
    dir->unlink_actor = etc_unlink_actor;
    dir->rename = etc_rename;
    dir->rename_actor = etc_rename_actor;
    dir->replace = etc_replace;
    dir->readdir = etc_readdir_node;
    dir->metadata = etc_metadata;
    dir->get = etc_get;
    dir->put = etc_put;
    dir->permission_actor = etc_permission_actor;
    dir->open_actor = etc_open_actor;
    dir->setattr_actor = etc_setattr_actor;
    dir->owns_nodes = true;
}

static void etc_setup_file(vfs_node_t *file) {
    if (etc_is_protected_account(file->path)) {
        file->mnt_flags = VFS_MNT_RDONLY;
        file->write = NULL;
        file->write_actor = NULL;
        file->truncate = NULL;
        file->read = etc_read_file;
        file->read_offset = etc_read_offset;
        file->metadata = etc_metadata;
        file->get = etc_get;
        file->put = etc_put;
        file->permission_actor = etc_permission_actor;
        file->open_actor = etc_open_actor;
        file->exec_snapshot = etc_exec_snapshot;
        file->owns_nodes = true;
        return;
    }

    file->mnt_flags = 0; /* writable */
    file->read = etc_read_file;
    file->read_offset = etc_read_offset;
    file->write = etc_write_file;
    file->write_actor = etc_write_actor;
    file->truncate = etc_truncate_file;
    file->metadata = etc_metadata;
    file->get = etc_get;
    file->put = etc_put;
    file->permission_actor = etc_permission_actor;
    file->open_actor = etc_open_actor;
    file->setattr_actor = etc_setattr_actor;
    file->exec_snapshot = etc_exec_snapshot;
    file->owns_nodes = true;
    file->serializes_write_offset = true;
}

static void etc_convert_tree(vfs_node_t *dir) {
    if (!dir) return;

    if (!dir->fs_private) {
        etc_file_entry_t *entry = (etc_file_entry_t *)kmalloc(sizeof(etc_file_entry_t));
        if (entry) {
            memset(entry, 0, sizeof(*entry));
            dir->fs_private = entry;
        }
    }
    etc_setup_directory(dir);

    for (vfs_node_t *c = dir->children; c; c = c->next) {
        if (!c->fs_private) {
            etc_file_entry_t *entry = (etc_file_entry_t *)kmalloc(sizeof(etc_file_entry_t));
            if (entry) {
                memset(entry, 0, sizeof(*entry));
                c->fs_private = entry;
            }
        }
        if (c->type == VFS_DIRECTORY) {
            etc_convert_tree(c);
        } else if (c->type == VFS_FILE) {
            etc_setup_file(c);
        }
    }
}

int etcfs_init(vfs_node_t *root) {
    if (!root) root = vfs_lookup_kernel("/");
    if (!root) return -VFS_ENOENT;

    vfs_node_t *etc = NULL;
    for (vfs_node_t *n = root->children; n; n = n->next) {
        if (!strcmp(n->name, "etc")) {
            etc = n;
            break;
        }
    }
    if (!etc) return 0; /* TarFS archive without /etc is allowed */

    etc_convert_tree(etc);
    serial_puts("[ OK ] Live /etc initialized as mutable RAM-backed filesystem (16 MiB quota)\n");
    return 0;
}
