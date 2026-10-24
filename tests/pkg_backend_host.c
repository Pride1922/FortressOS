#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "pkg_backend_candidate.h"
#include "process_table.h"
#include "vfs.h"
static creds_t credentials[2];
static unsigned reads,writes;
static bool fail,die;
static pw_key dying;
static int status(void *c,pkg_transaction_status *s){
    assert(c==&reads);reads++;
    *s=(pkg_transaction_status){.phase=PKG_TX_RETIRING,.sequence=1,.account_sequence=1,.credential_generation=1};return 0;
}
static int recover(void *c,pkg_transaction_status *s){
    assert(c==&reads);writes++;
    if(die){assert(pw_detach(dying));assert(pw_references(dying)==2);}
    *s=(pkg_transaction_status){.phase=PKG_TX_IDLE,.sequence=2,.account_sequence=2,.credential_generation=1};
    return fail?-VFS_EIO:0;
}
static pw_key birth(uint64_t pid,unsigned slot){
    assert(!process_record_begin(pid,0,false,0,0));creds_init_root(&credentials[slot]);
    assert(process_record_bind_creds(pid,&credentials[slot]));process_record_commit(pid);
    pw_key k;assert(pw_register(pid,&k));return k;
}
static void forget(uint64_t pid){(void)process_record_exit(pid,0);process_record_forget(pid);}
int main(void){
    pkg_transaction_backend b={&reads,status,recover};
    for(unsigned i=0;i<128;i++){
        pw_key a=birth(1000+2*i,0),child=birth(1001+2*i,1);pb_result result,old;
        memset(&result,0xa5,sizeof result);old=result;
        pb_request q={.size=32,.version=1,.operation=PB_ACQUIRE};
        unsigned before=reads;assert(pb_control(&q,a,false,&b,&result)==-VFS_EOPNOTSUPP&&reads==before&&!memcmp(&old,&result,sizeof result));
        q.reserved=1;assert(pb_control(&q,a,true,&b,&result)==-VFS_EINVAL&&reads==before);q.reserved=0;
        assert(!pb_control(&q,a,true,&b,&result)&&result.session&&result.phase==PKG_TX_RETIRING);uint64_t session=result.session;
        assert(pb_control(&q,child,true,&b,&result)==-VFS_EACCES);
        q.operation=PB_RECOVER;q.session=session;before=writes;
        pkg_transaction_backend foreign=b;foreign.context=&writes;
        assert(pb_control(&q,a,true,&foreign,&result)==-VFS_EACCES&&writes==before);
        assert(pb_control(&q,child,true,&b,&result)==-VFS_EACCES&&writes==before);
        creds_t saved=credentials[0],drop=saved;drop.cap_effective=0;
        assert(process_record_publish_creds(1000+2*i,&saved,&drop));
        assert(pb_control(&q,a,true,&b,&result)==-VFS_EACCES&&writes==before);
        assert(process_record_publish_creds(1000+2*i,&drop,&saved));
        fail=true;old=result;assert(pb_control(&q,a,true,&b,&result)==-VFS_EIO&&writes==before+1&&!memcmp(&old,&result,sizeof result));fail=false;
        /* An admitted step finishes after cancellation; the final operation
         * reference, not detach or output, permits owner-slot reuse. */
        die=true;dying=a;assert(!pb_control(&q,a,true,&b,&result)&&result.phase==PKG_TX_IDLE);die=false;
        assert(!pw_references(a)&&pb_control(&q,a,true,&b,&result)==-VFS_EACCES);
        q.operation=PB_ACQUIRE;q.session=0;assert(!pb_control(&q,child,true,&b,&result));uint64_t fresh=result.session;assert(fresh!=session);
        q.operation=PB_RELEASE;q.session=session;assert(pb_control(&q,a,true,&b,&result)==-VFS_EBADF);
        q.session=fresh;assert(!pb_control(&q,child,true,&b,&result));assert(pw_detach(child));
        forget(1000+2*i);forget(1001+2*i);
    }
    puts("PASS backend lifetimes=128 admission credential-drop child stale cancel final-operation-release reuse unconfirmed-failure");
    return 0;
}
