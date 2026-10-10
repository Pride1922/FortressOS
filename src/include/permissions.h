#ifndef FORTRESS_PERMISSIONS_H
#define FORTRESS_PERMISSIONS_H
#include "creds.h"
#ifdef TEST_PERMISSIONS_VALUES
void permission_capability_test(const creds_t *,uint64_t);
void permission_signal_test(const creds_t *,const creds_t *,unsigned);
#endif
/* Value-only capability and signal gates. Borrow coherent, canonical values;
 * callers own actual admission/publication exclusion. No lock or I/O here.
 * Legacy host wiring fixtures compile without the production enforcement flag. */
static inline int permission_capability(const creds_t *actor,uint64_t capability) {
#ifdef TEST_PERMISSIONS_VALUES
    permission_capability_test(actor,capability);
#endif
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    return actor && creds_valid(actor) && !(capability & ~CAP_ALL) &&
        (actor->cap_effective & capability)==capability ? 0 : -1;
#else
    (void)actor;(void)capability;return 0;
#endif
}
static inline int permission_signal(const creds_t *actor,const creds_t *target,unsigned signal) {
#ifdef TEST_PERMISSIONS_VALUES
    permission_signal_test(actor,target,signal);
#endif
    (void)signal;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (!actor || !target || !creds_valid(actor) || !creds_valid(target)) return -1;
    return (actor->cap_effective & CAP_KILL) || actor->uid==target->uid ||
        actor->uid==target->suid || actor->euid==target->uid || actor->euid==target->suid ? 0 : -1;
#else
    (void)actor;(void)target;return 0;
#endif
}
#endif
