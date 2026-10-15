#ifndef FORTRESS_INSTALL_LOG_H
#define FORTRESS_INSTALL_LOG_H
#include "block.h"
#include "boot_info.h"
#include "install_abi.h"
#define INSTALL_LOG_BYTES 4096u
#define INSTALL_LOG_VOLUME_LBA (514u*2048u)
#define INSTALL_LOG_VOLUME_SECTORS 16384u
#define INSTALL_LOG_DATA_LBA 161u
/* BSP only, unlocked, serialized by the installer control gate. No general FAT
 * writer or published block device. Binding is explicit boot configuration plus
 * the trusted boot-source GUID and exact immutable metadata SHA-256. */
bool install_log_bind(block_dev_t *,const boot_info_t *);
void install_log_boot_usb(const boot_info_t *);
bool install_log_requested(const boot_info_t *);
/* Fixed boot rejection reasons only; never console text or credentials. */
typedef enum { INSTALL_LOG_NO_SELECTION, INSTALL_LOG_NO_USB,
    INSTALL_LOG_UNKNOWN_DURABILITY, INSTALL_LOG_READ_ONLY } install_log_unavailable_t;
void install_log_unavailable(install_log_unavailable_t);
int install_log_event(const install_log_event_v1_t *);
void install_log_target(const install_identity_v1_t *,const char *username);
void install_log_stage(unsigned);
void install_log_result(unsigned,int,bool);
const char *install_log_snapshot(size_t *);
bool install_log_usb_ok(void);
#endif
