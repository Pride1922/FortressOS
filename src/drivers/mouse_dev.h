#ifndef FORTRESS_MOUSE_DEV_H
#define FORTRESS_MOUSE_DEV_H

#include "types.h"
#include "vfs.h"

#define MOUSE_DEV_FIFO_CAP 64

/* Mouse report packet: standard 4-byte PS/2 / USB boot mouse format */
typedef struct {
    uint8_t buttons; /* bit 0 = Left, bit 1 = Right, bit 2 = Middle */
    int8_t  dx;      /* Relative X motion (-128..127) */
    int8_t  dy;      /* Relative Y motion (-128..127) */
    int8_t  dz;      /* Relative Z / wheel motion (-128..127) */
} mouse_packet_t;

/* Initialize mouse character device */
void mouse_dev_init(vfs_node_t *dev_dir);

/* Push a mouse event into the circular FIFO (called from USB HID driver / IRQ) */
void mouse_dev_push(uint8_t buttons, int8_t dx, int8_t dy, int8_t dz);

/* Check if mouse packets are ready for reading */
bool mouse_dev_has_data(void);

/* Check if a given VFS node is the mouse character device */
bool is_mouse_dev_node(const vfs_node_t *node);

#endif /* FORTRESS_MOUSE_DEV_H */
