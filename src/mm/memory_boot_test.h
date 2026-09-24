#ifndef FORTRESS_MEMORY_BOOT_TEST_H
#define FORTRESS_MEMORY_BOOT_TEST_H

#include "boot_info.h"

/* Exact, bounded cmdline token: smp_memory_test=boot. No default activation.
 * BSP only, quiescent, with kernel-owned boot metadata. */
bool memory_boot_test_enabled(const boot_info_t *boot_info);
void memory_boot_test_before_vmm(void);
void memory_boot_test_after_vmm(void);

#endif
