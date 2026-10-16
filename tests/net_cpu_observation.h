#ifndef NET_CPU_OBSERVATION_H
#define NET_CPU_OBSERVATION_H
/* Private diagnostic-image ABI. Never installed in the production kernel. */
#include "types.h"
#define NET_CPU_OBSERVATION_NR 0x7ff6
#define NET_CPU_OBSERVATION_VERSION 1
#define NET_CPU_OBSERVATION_SYSCALLS 80
typedef struct {
    uint64_t version;
    char name[32];
    uint64_t owner_cpus, owner_steps, boot_dhcp_cpus;
    uint64_t masks[NET_CPU_OBSERVATION_SYSCALLS];
} net_cpu_observation_t;
_Static_assert(sizeof(net_cpu_observation_t)==704,"private observation ABI");
#endif
