/* Actual registry locking/publication with pthread adapters. No IRQ claim. */
#define main registry_regression_main
#include "process_registry_audit_host.c"
#undef main
static unsigned signal_checks;
void permission_capability_test(const creds_t *a,uint64_t cap) { (void)a;(void)cap;assert(false); }
void permission_signal_test(const creds_t *a,const creds_t *b,unsigned sig) {
    assert(held==1 && held_lock && a && b);
    assert(a->euid==1001 && !a->cap_effective && (b->euid==1001 || b->euid==2002 || b->euid==3003));
    assert(sig==0 || sig==SIGINT);signal_checks++;
}
int main(void) {
    creds_t a={.uid=1001,.euid=1001,.suid=1001,.umask=0022};
    creds_t b={.uid=2002,.euid=2002,.suid=2002,.umask=0022};
    signal_state_t sa,sb;
    assert(!process_record_begin(6000,0,false,0,0));assert(process_record_bind_creds(6000,&a));
    process_record_attach_signals(6000,&sa);process_record_commit(6000);
    assert(!process_record_begin(6001,6000,true,SPAWN_STAGED,0));assert(process_record_bind_creds(6001,&b));
    process_record_attach_signals(6001,&sb);
    creds_t value;assert(!process_record_creds(6001,&value));
    assert(!process_signal_send_creds(6000,6001,0) && signal_checks==1);
    assert(!process_signal_send_creds(6000,0,SIGINT) && signal_checks==3);
    assert(sb.pending_mask&SIGNAL_BIT(SIGINT)); /* Different UID, no caps: permissive. */
    process_record_commit(6001);creds_t next=b;next.uid=next.euid=next.suid=3003;
    assert(process_record_publish_creds(6001,&b,&next));
    assert(!process_signal_send_creds(6000,6001,0) && signal_checks==4);
    assert(process_record_exit(6001,0));process_record_forget(6001);
    assert(process_signal_send_creds(6000,6001,0)==SYSCALL_ESRCH && signal_checks==4);
    assert(process_record_wait(6000,6001,0,NULL,false)==6001);
    (void)process_record_exit(6000,0);process_record_forget(6000);
    puts("PASS Phase1 signal values: same-G actor/current target, staged/group/signal-0, publication/exit detach, unlocked wake");
    return 0;
}
