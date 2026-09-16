#ifndef FORTRESS_POWER_H
#define FORTRESS_POWER_H

#include "types.h"

#define REBOOT_CMD_RESTART   1
#define REBOOT_CMD_POWEROFF  2

/* Initialize power management (discovers FADT and parses DSDT _S5 sleep values) */
void power_init(void);

/* Reboot the system via ACPI reset -> 8042 reset -> port 0xCF9 -> triple fault */
void power_reboot(void) __attribute__((noreturn));

/* Power off the system via ACPI S5 sleep state and emulator fallback ports */
void power_shutdown(void) __attribute__((noreturn));

#endif /* FORTRESS_POWER_H */
