#ifndef FORTRESS_ENTRY_SECURITY_H
#define FORTRESS_ENTRY_SECURITY_H
#include "types.h"
#include "syscall_abi.h"
/* Loader-owned, bounded initial envp followed by auxv, terminated by AT_NULL. */
static inline bool user_entry_secure(const char *const *envp) {
    if (!envp) return false;
    unsigned n=0;
    while (n<MAX_SPAWN_ENVP && envp[n]) n++;
    const uintptr_t *aux=(const uintptr_t *)(envp+n+1);
    return aux[0]==23 && aux[1]!=0;
}
#endif
