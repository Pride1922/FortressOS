#ifndef FORTRESS_KERNEL_HOSTKEY_H
#define FORTRESS_KERNEL_HOSTKEY_H
#include "creds.h"
int64_t hostkey_control(uint64_t,uintptr_t,uint64_t,const creds_t *);
#endif
