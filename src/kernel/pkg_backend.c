#include "pkg_backend.h"
#include "process_table.h"
#include "spinlock.h"
#include "vfs.h"
/* One nonblocking execution credit, held without a spinlock through I/O.
 * Owner/session/operation refs use the existing private rank-1 arena, disjoint
 * from process and filesystem exclusion. Teardown cancels through pw_detach;
 * the last pw_complete releases the retained writer and permits fresh reuse. */
static unsigned pb_busy;
static void *pb_bound_context;
static uint64_t pb_session;
bool pkg_backend_snapshot_valid(const pkg_transaction_status *s) {
    if(s->phase>PKG_TX_RETIRING || !s->sequence || !s->account_sequence || !s->credential_generation)return false;
    for(unsigned i=0;i<2;i++) {
        if(s->remaining[i]>UINT64_MAX-s->charged[i])return false;
        if(s->phase==PKG_TX_IDLE && (s->remaining[i]||s->charged[i]))return false;
    }
    return true;
}
int pb_control(const pb_request *q,pw_key owner,bool admitted,const pkg_transaction_backend *b,pb_result *out) {
    spin_debug_assert_unheld();
    if(!q || !out || q->size!=sizeof(*q) || q->version!=1 || q->reserved || q->reserved64 ||
       q->operation<PB_STATUS || q->operation>PB_RELEASE ||
       ((q->operation==PB_STATUS || q->operation==PB_ACQUIRE)?q->session!=0:q->session==0))return -VFS_EINVAL;
    if(!admitted || !b || !b->status || !b->recover)return -VFS_EOPNOTSUPP;
    if(q->operation==PB_ACQUIRE && !pw_authorized(owner))return -VFS_EACCES;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&pb_busy,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -VFS_EAGAIN;
    int r=0;uint64_t session=q->session;pw_key op={0};bool entered=false;
    pkg_transaction_status snapshot={0};
    if(q->operation==PB_RELEASE) {
        if(!pw_close(owner,session))r=-VFS_EBADF;
        goto done;
    }
    if(q->operation==PB_RECOVER) {
        if(session!=pb_session || b->context!=pb_bound_context){r=-VFS_EACCES;goto done;}
        /* pw_enter takes a current effective-UID/capability snapshot outside
         * arena exclusion, then pins the owner generation and operation. */
        if(!pw_enter(owner,session,&op)){r=-VFS_EACCES;goto done;}
        entered=true;
        r=b->recover(b->context,&snapshot);
    } else {
        r=b->status(b->context,&snapshot);
    }
    if(!r && (!pkg_backend_snapshot_valid(&snapshot) || (q->operation==PB_RECOVER && snapshot.phase!=PKG_TX_IDLE)))r=-VFS_EIO;
    /* True here means a validated enrolled recovery profile, not durable IDLE.
     * Acquisition authorizes recovery of a pending grant; it never clears it. */
    if(!r && q->operation==PB_ACQUIRE) {
        if(!pw_begin(owner,true,&session))r=-VFS_EACCES;
        else {pb_bound_context=b->context;pb_session=session;}
    }
done:
    if(!r && q->operation==PB_RELEASE){pb_bound_context=NULL;pb_session=0;}
    if(entered && !pw_complete(op))r=-VFS_EIO;
    if(!r) {
        pb_result value={.size=sizeof(value),.version=1,.phase=snapshot.phase,.session=q->operation==PB_RELEASE?0:session,
            .sequence=snapshot.sequence,.account_sequence=snapshot.account_sequence,.credential_generation=snapshot.credential_generation};
        for(unsigned i=0;i<2;i++){value.remaining[i]=snapshot.remaining[i];value.charged[i]=snapshot.charged[i];}
        *out=value;
    }
    __atomic_store_n(&pb_busy,0,__ATOMIC_RELEASE);return r;
}
