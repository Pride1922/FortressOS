#ifndef USER_TOOLS_BOOT_SNAPSHOT_H
#define USER_TOOLS_BOOT_SNAPSHOT_H

#include "types.h"
#include "syscall_abi.h"

#define BOOT_SNAPSHOT_DIR_PRIMARY   "/var/log"
#define BOOT_SNAPSHOT_FILE_PRIMARY  "/var/log/boot.log"
#define BOOT_SNAPSHOT_PREV_PRIMARY  "/var/log/boot.log.prev"

#define BOOT_SNAPSHOT_DIR_FALLBACK  "/run"
#define BOOT_SNAPSHOT_FILE_FALLBACK "/run/boot.log"
#define BOOT_SNAPSHOT_PREV_FALLBACK "/run/boot.log.prev"

int boot_snapshot_main(int argc, char **argv);

#endif /* USER_TOOLS_BOOT_SNAPSHOT_H */
