#include "kbd_dev.h"
#include "spinlock.h"
#include "string.h"
#include "poll.h"

/* Bounded static FIFO of structured key events, modeled on mouse_dev.c.
 * s_kbd_lock is rank 1 and never nests with any other lock: producers (the
 * i8042 IRQ path and the USB HID worker via input.c) take it alone, and
 * poll_wake_notify() runs after it is released. */
static key_event_t s_kbd_fifo[KBD_DEV_FIFO_CAP];
static size_t      s_kbd_head = 0;
static size_t      s_kbd_count = 0;
static uint64_t    s_kbd_dropped = 0;
static spinlock_t  s_kbd_lock = SPINLOCK_RANKED(1, "kbd_fifo");
/* Independent opens (not dups) currently alive; see kbd_dev_grabbed(). */
static uint32_t    s_kbd_open_count = 0;

static vfs_node_t  s_kbd_node;
static vfs_node_t  s_input_kbd_node;

#define KBD_READ_BATCH 16

/* Non-blocking: returns whole records only, 0 when empty. Records are popped
 * under the lock into a bounded local batch and copied out after unlocking,
 * so no user memory is touched with interrupts disabled. */
static int64_t kbd_vfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t len) {
    (void)node;
    (void)offset;
    if (!buf || len < sizeof(key_event_t)) return -VFS_EINVAL;
    size_t want = len / sizeof(key_event_t);
    if (want > KBD_READ_BATCH) want = KBD_READ_BATCH;

    key_event_t batch[KBD_READ_BATCH];
    size_t got = 0;
    uint64_t rflags = spin_lock_irqsave(&s_kbd_lock);
    while (got < want && s_kbd_count) {
        batch[got++] = s_kbd_fifo[s_kbd_head];
        s_kbd_head = (s_kbd_head + 1) % KBD_DEV_FIFO_CAP;
        s_kbd_count--;
    }
    spin_unlock_irqrestore(&s_kbd_lock, rflags);

    memcpy(buf, batch, got * sizeof(key_event_t));
    return (int64_t)(got * sizeof(key_event_t));
}

static int kbd_vfs_open(vfs_node_t *node) {
    (void)node;
    uint64_t rflags = spin_lock_irqsave(&s_kbd_lock);
    if (__atomic_fetch_add(&s_kbd_open_count, 1, __ATOMIC_ACQ_REL) == 0) {
        /* Discard keys typed into the shell before the grab began. */
        s_kbd_head = 0;
        s_kbd_count = 0;
    }
    spin_unlock_irqrestore(&s_kbd_lock, rflags);
    return 0;
}

static void kbd_vfs_close(vfs_node_t *node) {
    (void)node;
    /* Paired with kbd_vfs_open once per independent open; never underflow. */
    uint32_t cur = __atomic_load_n(&s_kbd_open_count, __ATOMIC_ACQUIRE);
    while (cur && !__atomic_compare_exchange_n(&s_kbd_open_count, &cur, cur - 1, false,
                                               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
}

void kbd_dev_push(const key_event_t *ev) {
    if (!ev || !ev->keycode) return;
    uint64_t rflags = spin_lock_irqsave(&s_kbd_lock);
    if (s_kbd_count == KBD_DEV_FIFO_CAP) {
        /* Drop oldest to keep queue fresh */
        s_kbd_head = (s_kbd_head + 1) % KBD_DEV_FIFO_CAP;
        s_kbd_count--;
        s_kbd_dropped++;
    }
    key_event_t rec = *ev;
    rec.reserved = 0;
    s_kbd_fifo[(s_kbd_head + s_kbd_count) % KBD_DEV_FIFO_CAP] = rec;
    s_kbd_count++;
    spin_unlock_irqrestore(&s_kbd_lock, rflags);

    poll_wake_notify();
}

bool kbd_dev_has_data(void) {
    uint64_t rflags = spin_lock_irqsave(&s_kbd_lock);
    bool has = (s_kbd_count > 0);
    spin_unlock_irqrestore(&s_kbd_lock, rflags);
    return has;
}

uint64_t kbd_dev_dropped(void) {
    uint64_t rflags = spin_lock_irqsave(&s_kbd_lock);
    uint64_t dropped = s_kbd_dropped;
    spin_unlock_irqrestore(&s_kbd_lock, rflags);
    return dropped;
}

bool kbd_dev_grabbed(void) {
    return __atomic_load_n(&s_kbd_open_count, __ATOMIC_ACQUIRE) != 0;
}

bool is_kbd_dev_node(const vfs_node_t *node) {
    return node && (node == &s_kbd_node || node == &s_input_kbd_node);
}

static void kbd_node_setup(vfs_node_t *n, vfs_node_t *parent, const char *name, const char *path) {
    memset(n, 0, sizeof(*n));
    n->type = VFS_FILE;
    n->mode = VFS_S_IFCHR | 0660;
    n->is_stream = true;
    n->read = kbd_vfs_read;
    n->open = kbd_vfs_open;
    n->close = kbd_vfs_close;
    n->parent = parent;
    memcpy(n->name, name, strlen(name) + 1);
    memcpy(n->path, path, strlen(path) + 1);
    n->next = parent->children;
    parent->children = n;
}

void kbd_dev_init(vfs_node_t *dev_dir) {
    if (!dev_dir) return;
    kbd_node_setup(&s_kbd_node, dev_dir, "kbd0", "/dev/kbd0");
    for (vfs_node_t *c = dev_dir->children; c; c = c->next) {
        if (c->type == VFS_DIRECTORY && !strcmp(c->name, "input")) {
            kbd_node_setup(&s_input_kbd_node, c, "keyboard", "/dev/input/keyboard");
            break;
        }
    }
}
