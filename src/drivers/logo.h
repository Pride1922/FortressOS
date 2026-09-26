#ifndef FORTRESS_LOGO_H
#define FORTRESS_LOGO_H

#include "types.h"
#include "boot_info.h"

/* Renders the branded FortressOS boot logo and emblem on the linear framebuffer.
 * Safe for early boot: runs before PMM or heap initialization.
 */
void logo_render_boot(const boot_info_t *boot_info);
void logo_update_status(const char *status);

#endif /* FORTRESS_LOGO_H */
