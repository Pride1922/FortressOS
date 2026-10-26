#include "mouse_dev.h"
#include "spinlock.h"
#include "string.h"
#include "poll.h"
#include "syscall_abi.h"

static mouse_packet_t s_mouse_fifo[MOUSE_DEV_FIFO_CAP];
static size_t         s_mouse_head = 0;
static size_t         s_mouse_tail = 0;
static size_t         s_mouse_count = 0;
static spinlock_t     s_mouse_lock = SPINLOCK_RANKED(1, "mouse_fifo");

static vfs_node_t     s_mouse_node;
static vfs_node_t     s_input_dir;
static vfs_node_t     s_input_mouse_node;

static int64_t mouse_vfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t len) {
    (void)node;
    (void)offset;
    if (!buf || len < 3) {
        return 0;
    }

    uint64_t rflags = spin_lock_irqsave(&s_mouse_lock);
    if (s_mouse_count == 0) {
        spin_unlock_irqrestore(&s_mouse_lock, rflags);
        return 0; /* Non-blocking: no data available */
    }

    mouse_packet_t pkt = s_mouse_fifo[s_mouse_head];
    s_mouse_head = (s_mouse_head + 1) % MOUSE_DEV_FIFO_CAP;
    s_mouse_count--;
    spin_unlock_irqrestore(&s_mouse_lock, rflags);

    uint8_t *out = (uint8_t *)buf;
    out[0] = pkt.buttons;
    out[1] = (uint8_t)pkt.dx;
    out[2] = (uint8_t)pkt.dy;
    if (len >= 4) {
        out[3] = (uint8_t)pkt.dz;
        return 4;
    }
    return 3;
}

void mouse_dev_push(uint8_t buttons, int8_t dx, int8_t dy, int8_t dz) {
    uint64_t rflags = spin_lock_irqsave(&s_mouse_lock);
    if (s_mouse_count == MOUSE_DEV_FIFO_CAP) {
        /* Drop oldest to keep queue fresh */
        s_mouse_head = (s_mouse_head + 1) % MOUSE_DEV_FIFO_CAP;
        s_mouse_count--;
    }
    s_mouse_fifo[s_mouse_tail] = (mouse_packet_t){
        .buttons = buttons,
        .dx      = dx,
        .dy      = dy,
        .dz      = dz,
    };
    s_mouse_tail = (s_mouse_tail + 1) % MOUSE_DEV_FIFO_CAP;
    s_mouse_count++;
    spin_unlock_irqrestore(&s_mouse_lock, rflags);

    poll_wake_notify();
}

bool mouse_dev_has_data(void) {
    uint64_t rflags = spin_lock_irqsave(&s_mouse_lock);
    bool has = (s_mouse_count > 0);
    spin_unlock_irqrestore(&s_mouse_lock, rflags);
    return has;
}

bool is_mouse_dev_node(const vfs_node_t *node) {
    return (node == &s_mouse_node || node == &s_input_mouse_node);
}

void mouse_dev_init(vfs_node_t *dev_dir) {
    if (!dev_dir) return;

    memset(&s_mouse_node, 0, sizeof(s_mouse_node));
    s_mouse_node.type = VFS_FILE;
    s_mouse_node.mode = VFS_S_IFCHR | 0660;
    s_mouse_node.is_stream = true;
    s_mouse_node.read = mouse_vfs_read;
    s_mouse_node.parent = dev_dir;
    memcpy(s_mouse_node.name, "mouse0", 7);
    memcpy(s_mouse_node.path, "/dev/mouse0", 12);

    /* Link /dev/mouse0 as child of /dev */
    s_mouse_node.next = dev_dir->children;
    dev_dir->children = &s_mouse_node;

    /* Create /dev/input directory */
    memset(&s_input_dir, 0, sizeof(s_input_dir));
    s_input_dir.type = VFS_DIRECTORY;
    s_input_dir.mode = VFS_S_IFDIR | 0755;
    s_input_dir.parent = dev_dir;
    memcpy(s_input_dir.name, "input", 6);
    memcpy(s_input_dir.path, "/dev/input", 11);
    s_input_dir.next = dev_dir->children;
    dev_dir->children = &s_input_dir;

    /* Create /dev/input/mouse */
    memset(&s_input_mouse_node, 0, sizeof(s_input_mouse_node));
    s_input_mouse_node.type = VFS_FILE;
    s_input_mouse_node.mode = VFS_S_IFCHR | 0660;
    s_input_mouse_node.is_stream = true;
    s_input_mouse_node.read = mouse_vfs_read;
    s_input_mouse_node.parent = &s_input_dir;
    memcpy(s_input_mouse_node.name, "mouse", 6);
    memcpy(s_input_mouse_node.path, "/dev/input/mouse", 17);
    s_input_mouse_node.next = NULL;
    s_input_dir.children = &s_input_mouse_node;
}
