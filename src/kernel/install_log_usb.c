#include "install_log.h"
#ifdef INSTALLER_MULTI
#include "xhci.h"
static block_dev_t log_device;
#endif
void install_log_boot_usb(const boot_info_t *b) {
#ifdef INSTALLER_MULTI
    if (!install_log_requested(b)) {install_log_unavailable(INSTALL_LOG_NO_SELECTION);return;}
    usb_durability_mode_t mode=usb_get_durability_mode();
    if (!usb_is_initialized()) {install_log_unavailable(INSTALL_LOG_NO_USB);return;}
    if (mode==USB_DURABILITY_UNKNOWN) {install_log_unavailable(INSTALL_LOG_UNKNOWN_DURABILITY);return;}
    if (mode==USB_DURABILITY_READ_ONLY) {install_log_unavailable(INSTALL_LOG_READ_ONLY);return;}
    log_device=(block_dev_t){.sector_size=usb_get_sector_size(),.sector_count=usb_get_sector_count(),
        .read_sector=usb_block_read,.write_sector=usb_block_write,.flush=usb_block_flush};
    (void)install_log_bind(&log_device,b);
#else
    (void)b;
#endif
}
