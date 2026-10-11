#ifndef FORTRESS_MACHINE_CONFIG_H
#define FORTRESS_MACHINE_CONFIG_H
#include "account_abi.h"
typedef long (*machine_call_t)(long,uintptr_t,uintptr_t,uintptr_t);
static inline bool machine_installed(machine_call_t call) {
    account_request_t req={.size=sizeof(req),.version=1};
    return call(SYS_ACCOUNTCTL,ACCOUNT_STATUS,(uintptr_t)&req,sizeof(req))==0;
}
#endif
