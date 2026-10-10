#define main registry_fixture_main
#include "process_registry_audit_host.c"
#undef main
static unsigned checks;
void permission_capability_test(const creds_t *a,uint64_t cap) {(void)a;(void)cap;assert(false);}
void permission_signal_test(const creds_t *a,const creds_t *b,unsigned sig) {
    assert(held==1 && held_lock && creds_valid(a) && creds_valid(b));(void)sig;checks++;
}
int main(void) {
    creds_t a={.uid=1001,.euid=2002,.suid=1001,.umask=0022};
    creds_t b={.uid=3003,.euid=2002,.suid=4004,.umask=0022};
    signal_state_t sa,sb;
    assert(!process_record_begin(6000,0,false,0,0));assert(process_record_bind_creds(6000,&a));
    process_record_attach_signals(6000,&sa);process_record_commit(6000);
    assert(!process_record_begin(6001,6000,true,SPAWN_STAGED,0));assert(process_record_bind_creds(6001,&b));
    process_record_attach_signals(6001,&sb);
    unsigned before=wakes;
    assert(process_signal_send_creds(6000,6001,0)==SYSCALL_EPERM && !sb.pending_mask && wakes==before);
    assert(process_signal_send_creds(6000,6001,SIGINT)==SYSCALL_EPERM && !sb.pending_mask && wakes==before);
    assert(!process_signal_send_creds(6000,0,SIGINT) && (sa.pending_mask&SIGNAL_BIT(SIGINT)) && !sb.pending_mask);
    process_record_commit(6001);creds_t next=b;next.suid=a.uid;
    assert(process_record_publish_creds(6001,&b,&next));
    assert(!process_signal_send_creds(6000,6001,SIGINT) && (sb.pending_mask&SIGNAL_BIT(SIGINT)));
    next=b;next.suid=4004;next.uid=a.euid;assert(process_record_publish_creds(6001,&b,&next));
    assert(!process_signal_send_creds(6000,6001,0));
    next=b;next.uid=3003;assert(process_record_publish_creds(6001,&b,&next));
    assert(process_signal_send_creds(6000,6001,0)==SYSCALL_EPERM);
    next=a;next.cap_effective=CAP_KILL;assert(process_record_publish_creds(6000,&a,&next));
    assert(!process_signal_send_creds(6000,6001,0));
    unsigned previous=checks;assert(process_record_exit(6001,0));process_record_forget(6001);
    assert(process_signal_send_creds(6000,6001,0)==SYSCALL_ESRCH && checks==previous);
    assert(process_record_wait(6000,6001,0,NULL,false)==6001);
    process_record_exit(6000,0);process_record_forget(6000);
    puts("PASS actual signal enforcement: same-G current identities, staged/zero/group partial admission, real/effective to real/saved UID, CAP_KILL, detached target");
    return 0;
}
