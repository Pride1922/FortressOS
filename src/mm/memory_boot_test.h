#ifndef FORTRESS_MEMORY_BOOT_TEST_H
#define FORTRESS_MEMORY_BOOT_TEST_H

#include "boot_info.h"

/* Exact, bounded cmdline token: smp_memory_test=boot. No default activation.
 * BSP only, quiescent, with kernel-owned boot metadata. */
bool memory_boot_test_enabled(const boot_info_t *boot_info);
void memory_boot_test_before_vmm(void);
void memory_boot_test_after_vmm(void);

/* Exact, bounded cmdline token: smp_memory_test=stress. No default activation.
 * Multi-core concurrent stress test after AP scheduler startup. */
bool memory_stress_test_enabled(const boot_info_t *boot_info);
void memory_stress_test_run(size_t total_cpus);

/* Exact, bounded cmdline token: smp_memory_test=vmm_lifecycle. No default activation.
 * Multi-core address space lifetime, concurrent process spawn/exit, deferred reaping,
 * and table frame equality verification. */
bool memory_vmm_lifecycle_test_enabled(const boot_info_t *boot_info);
void memory_vmm_lifecycle_test_run(size_t total_cpus);

/* Explicit fw_cfg opt-in, exclusive QEMU USB fixture admitted by caller.
 * Synchronous VFS memory audit after production mount, before shell starts. */
void memory_storage_test_run(void);
/* Separate opt-in namespace churn diagnostic; does not change node lifetime. */
void memory_storage_churn_test_run(void);
/* Exclusive low-RAM QEMU boot diagnostic before AP/thread startup. */
void memory_pressure_test_run(void);
/* Representative process/allocation burst and recovery under bounded PMM pressure. */
bool memory_burst_test_enabled(const boot_info_t *boot_info);
void memory_burst_test_run(size_t total_cpus);

/* Concurrent process peaks and fragmented PMM headroom under bounded pressure. */
bool memory_cohort_test_enabled(const boot_info_t *boot_info);
void memory_cohort_test_run(size_t total_cpus);

/* Process launch allocation failure rollback verification. */
bool memory_rollback_test_enabled(const boot_info_t *boot_info);
void memory_rollback_test_run(size_t total_cpus);

/* Dedicated measurement-only profiling: kernel stack allocation & free telemetry. */
bool memory_kstack_profile_enabled(const boot_info_t *boot_info);
void memory_kstack_profile_run(size_t total_cpus);

/* Matching untracked gated control: kernel stack allocation & free. */
bool memory_kstack_control_enabled(const boot_info_t *boot_info);
void memory_kstack_control_run(size_t total_cpus);

#endif

