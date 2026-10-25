/* Private Ring 3 frontend: callbacks own one request, never retry mutations. */
#include "pkg_exec_cli.h"
#include "pkg_exec_wire.h"
#include "user/pkg/mutation_cli.h"
#include "user/tools/common.h"
typedef struct {uint64_t session;} cli_owner;
static int control(unsigned operation,uint64_t session,pb_result *out) {
    pb_request q={.size=sizeof q,.version=1,.operation=operation,.session=session};
    return (int)tool_syscall(200,(uintptr_t)&q,(uintptr_t)out,sizeof q);
}
static int snapshot(const pb_result *r,uint64_t session,pkg_transaction_status *out) {
    if(r->size!=sizeof *r||r->version!=1||r->reserved||r->session!=session||
       r->phase>PKG_TX_RETIRING||!r->sequence||!r->account_sequence||!r->credential_generation)return -1;
    *out=(pkg_transaction_status){.phase=r->phase,.sequence=r->sequence,
        .account_sequence=r->account_sequence,.credential_generation=r->credential_generation};
    for(unsigned i=0;i<2;i++) {
        if(r->remaining[i]>UINT64_MAX-r->charged[i]||
           (r->phase==PKG_TX_IDLE&&(r->remaining[i]||r->charged[i])))return -1;
        out->remaining[i]=r->remaining[i];out->charged[i]=r->charged[i];
    }
    return 0;
}
static int acquire(cli_owner *owner) {
    if(owner->session)return 0;
    pb_result r={0};pkg_transaction_status status;
    if(control(PB_ACQUIRE,0,&r))return -1;
    /* Retain a returned handle for cleanup even if its snapshot is rejected. */
    owner->session=r.session;
    return !r.session?-1:snapshot(&r,r.session,&status);
}
static int status(void *context,pkg_transaction_status *out) {
    (void)context;pb_result r={0};
    return control(PB_STATUS,0,&r)?-1:snapshot(&r,0,out);
}
static int recover(void *context,pkg_transaction_status *out) {
    cli_owner *owner=context;pb_result r={0};
    if(acquire(owner)||control(PB_RECOVER,owner->session,&r))return -1;
    return snapshot(&r,owner->session,out);
}
static int mutate(void *context,bool remove,const char *name,size_t n) {
    cli_owner *owner=context;
    if(!n||n>32||acquire(owner))return -1;
    mx_request q={.size=sizeof q,.version=1,.operation=remove?2:1,.session=owner->session};
    for(size_t i=0;i<n;i++)q.package[i]=name[i];
    pb_result r={0};pkg_transaction_status result;
    if(tool_syscall(203,(uintptr_t)&q,(uintptr_t)&r,sizeof q))return -1;
    return snapshot(&r,owner->session,&result)||result.phase!=PKG_TX_IDLE?-1:0;
}
int pkg_private_cli(int argc,char **argv) {
    cli_owner owner={0};
    pkg_mutation_backend backend={.transactions={&owner,status,recover},.mutate=mutate};
    int result=pkg_mutation_cli(argc,argv,NULL,&backend);
    if(owner.session) {
        pb_result released={0};
        if(control(PB_RELEASE,owner.session,&released)||released.size!=sizeof released||
           released.version!=1||released.reserved||released.session)
            return tool_error("pkg","session release not confirmed; inspect status before retrying",NULL);
    }
    return result;
}
