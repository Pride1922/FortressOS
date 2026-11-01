#ifndef FORTRESS_KBD_DEV_H
#define FORTRESS_KBD_DEV_H

#include "types.h"
#include "vfs.h"
#include "input_event.h"

#define KBD_DEV_FIFO_CAP 128

/* Creates /dev/kbd0 and /dev/input/keyboard. Call after mouse_dev_init,
 * which creates /dev/input; dev_dir is /dev. Boot-time, single-threaded. */
void kbd_dev_init(vfs_node_t *dev_dir);

/* Appends one event; drops the oldest when full. Any context, including
 * IRQ handlers; takes only the rank-1 kbd_fifo lock (never nested) and then
 * calls poll_wake_notify() with no lock held. */
void kbd_dev_push(const key_event_t *ev);

bool kbd_dev_has_data(void);
bool is_kbd_dev_node(const vfs_node_t *node);

/* True while at least one open file references the device. Keyboard bytes
 * are then not fed to the console terminal (the reader owns the keyboard);
 * serial console input is unaffected. Lock-free acquire load. */
bool kbd_dev_grabbed(void);

/* Events dropped by saturation since boot (diagnostics). */
uint64_t kbd_dev_dropped(void);

#endif /* FORTRESS_KBD_DEV_H */
