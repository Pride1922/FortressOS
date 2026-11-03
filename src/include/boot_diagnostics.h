#ifndef FORTRESS_BOOT_DIAGNOSTICS_H
#define FORTRESS_BOOT_DIAGNOSTICS_H
#include "types.h"
/* Exact, bounded opt-in. Verbose output alone never enables fault injection.
 * This selects non-storage self-tests; raw-media gates remain independent. */
static inline bool boot_selftests_requested(const char *cmdline, size_t capacity) {
    if (!cmdline) return false;
    const char option[] = "boot_selftests=1";
    for (size_t i=0;i<capacity && cmdline[i];) {
        while (i<capacity && (cmdline[i]==' ' || cmdline[i]=='\t' || cmdline[i]=='\n')) i++;
        size_t start=i;
        while (i<capacity && cmdline[i] && cmdline[i]!=' ' && cmdline[i]!='\t' && cmdline[i]!='\n') i++;
        if (i==capacity) return false;
        if (i-start==sizeof(option)-1) {
            size_t j=0;while (j<sizeof(option)-1 && cmdline[start+j]==option[j]) j++;
            if (j==sizeof(option)-1) return true;
        }
    }
    return false;
}
#endif
