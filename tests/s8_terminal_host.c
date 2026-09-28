/* Actual input/terminal and group implementation. Hardware, IRQ exclusion and
 * scheduling are adapters; this test makes no IRQ/SMP/context-switch claim. */
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#define INPUT_HOST_TEST
#include "process_table.c"
#include "input.c"
unsigned input_test_cpu;
static bool locked, device_irq;
static tcb_t tasks[12], *current;
static vfs_node_t tty, other;
static file_t tty_file={.node=&tty}, other_file={.node=&other};
static unsigned stops, wakes;
static void (*wait_hook)(void);
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!locked && !device_irq); assert(!pthread_mutex_lock(&l->mutex)); locked=true; return 0;
}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {
    (void)f; assert(locked); locked=false; assert(!pthread_mutex_unlock(&l->mutex));
}
tcb_t *thread_current(void) { return current; }
file_t *fd_get(tcb_t *t,int fd) { return fd>=0 && fd<32 ? t->fd_table[fd] : NULL; }
vfs_node_t *vfs_get_terminal_node(void) { return &tty; }
void serial_puts(const char *s) { (void)s; assert(!locked && !device_irq); }
void thread_yield(void) { assert(!locked && !device_irq); }
void sched_wake_all(const void *channel) { (void)channel; assert(!locked && !device_irq); ++wakes; }
void sched_wait_until(const void *channel,bool (*ready)(void *),void *arg) {
    (void)channel; assert(!locked && !device_irq);
    if (wait_hook) { void (*hook)(void)=wait_hook; wait_hook=NULL; hook(); }
    assert(ready(arg));
}
bool process_signal_interrupt(void) {
    assert(!locked && !device_irq);
    if ((current->signals.pending_mask & SIGNAL_BIT(SIGTTIN)) &&
        !(current->signals.blocked_mask & SIGNAL_BIT(SIGTTIN)) &&
        !(current->signals.ignored_mask & SIGNAL_BIT(SIGTTIN)) &&
        !current->signals.action_handlers[SIGTTIN]) {
        assert(process_signal_take_control(current->tid)==SIGTTIN); ++stops;
        tcb_t *reader=current; current=&tasks[0];
        assert(!input_tcsetpgrp(31,reader->pgid));
        assert(!process_signal_send(current->tid,reader->tid,SIGCONT));
        assert(process_record_resume(reader->tid)); current=reader;
    }
    return signal_state_ready(&current->signals);
}
static void bytes(const char *p,size_t n) {
    device_irq=true; ingress(p,n); device_irq=false;
}
static void setup(unsigned i) {
    tcb_t *t=&tasks[i]; t->tid=10+i; t->sid=10; t->pgid=10+i; t->is_user=true;
    t->fd_table[0]=t->fd_table[31]=&tty_file; t->fd_table[1]=&other_file;
    assert(!process_record_begin(t->tid,i ? 10 : 0,i!=0,i ? SPAWN_SETPGROUP : 0,0));
    process_record_attach_signals(t->tid,&t->signals);
}
static void lose_foreground(void) {
    current=&tasks[0]; assert(!input_tcsetpgrp(31,10)); current=&tasks[1];
}
int main(void) {
    (void)signal_worker; /* worker loop itself is exercised by QEMU */
    for (unsigned i=0;i<12;++i) setup(i);
    current=&tasks[0]; g_input_ready=true;
    assert(!input_terminal_bootstrap(10));
    assert(input_tcgetpgrp(31)==10 && g_terminal.attrs.input_flags==TERM_ISIG);
    assert(input_tcgetpgrp(32)==SYSCALL_EBADF);
    assert(input_tcgetpgrp(UINT64_MAX)==SYSCALL_EBADF);
    assert(input_tcgetpgrp(1)==SYSCALL_ENOTTY);
    assert(input_tcsetpgrp(31,999)==SYSCALL_ESRCH);
    assert(input_tcsetpgrp(31,0)==SYSCALL_EINVAL);
    tasks[0].sid=999; assert(input_tcgetpgrp(31)==SYSCALL_ENOTTY); tasks[0].sid=10;
    input_test_cpu=1;
    assert(input_tcsetpgrp(31,11)==SYSCALL_EOPNOTSUPP);
    assert(input_control_check()==SYSCALL_EOPNOTSUPP); input_test_cpu=0;
    signal_action_t ignore={.handler=SIG_IGN};
    assert(!process_signal_action(10,SIGTTOU,&ignore,NULL));
    /* FD 31 works with stdin redirected; a different node is never a tty. */
    current->fd_table[0]=&other_file; assert(!input_tcsetpgrp(31,11));
    assert(input_tcgetpgrp(0)==SYSCALL_ENOTTY);
    assert(!input_tcsetpgrp(31,10)); current->fd_table[0]=&tty_file;
    char c=0; bytes("a",1); assert(input_read_timeout(&c,1,0)==1 && c=='a');
    current=&tasks[1];
    uint64_t mask=SIGNAL_BIT(SIGTTIN);
    assert(!process_signal_mask(11,SIG_BLOCK,&mask,NULL));
    bytes("b",1); assert(input_read_timeout(&c,1,0)==SYSCALL_EIO && g_input.count==1);
    assert(!process_signal_mask(11,SIG_UNBLOCK,&mask,NULL));
    assert(!process_signal_action(11,SIGTTIN,&ignore,NULL));
    assert(input_read_timeout(&c,1,0)==SYSCALL_EIO && g_input.count==1);
    signal_action_t normal={0}; assert(!process_signal_action(11,SIGTTIN,&normal,NULL));
    assert(input_read_timeout(&c,1,0)==1 && c=='b' && stops==1);
    /* Ownership changes inside the wait adapter; post-wake check cannot steal. */
    assert(!process_signal_action(11,SIGTTIN,&ignore,NULL));
    bytes("c",1); wait_hook=lose_foreground;
    assert(input_read_timeout(&c,1,0)==SYSCALL_EIO && g_input.count==1);
    /* Caught background mutation returns EINTR and leaves state unchanged. */
    signal_action_t caught={.handler=0x400000};
    assert(!process_signal_action(11,SIGTTOU,&caught,NULL));
    assert(input_tcsetpgrp(31,11)==SYSCALL_EINTR && g_terminal.fg_pgid==10);
    assert(process_signal_take(11)==SIGTTOU);
    mask=SIGNAL_BIT(SIGTTOU); assert(!process_signal_mask(11,SIG_BLOCK,&mask,NULL));
    assert(!input_tcsetpgrp(31,11));
    terminal_attrs_t attrs;
    assert(!input_termattr(31,TERM_GET,&attrs)); attrs.version=2;
    assert(input_termattr(31,TERM_SET,&attrs)==SYSCALL_EINVAL); attrs.version=1;
    attrs.input_flags=0; assert(!input_termattr(31,TERM_SET,&attrs));
    g_input.count=g_input.head=0; bytes("\3",1); bytes("\32",1);
    assert(input_read_timeout(&c,1,0)==1 && c==3);
    assert(input_read_timeout(&c,1,0)==1 && c==26);
    attrs.input_flags=TERM_ISIG; assert(!input_termattr(31,TERM_SET,&attrs));
    bytes("queued",6); bytes("\3",1);
    assert(!g_input.count && g_terminal.event_count==1);
    bytes("\3",1); assert(g_terminal.attrs.signal_coalesced==1);
    current=&tasks[0]; assert(!input_tcsetpgrp(31,10));
    assert(drain_event()); assert(process_signal_take(11)==SIGINT);
    assert(!process_signal_take(10)); /* handoff did not redirect the old event */
    /* Atomic keyboard sequence rejected whole at capacity. */
    for (unsigned i=0;i<INPUT_CAPACITY-1;++i) bytes("x",1);
    bytes("\033[A",3); assert(g_input.count==INPUT_CAPACITY-1 && input_dropped()==3);
    assert(input_read_timeout(&c,1,0)==INPUT_LOST);
    serial_ingress('\r'); serial_ingress('\n');
    assert(input_read_timeout(&c,1,0)==1 && c=='\n' && !g_input.count);
    for (unsigned i=1;i<=8;++i) {
        assert(!input_tcsetpgrp(31,10+i)); bytes("\3",1); bytes("\32",1);
    }
    assert(g_terminal.event_count==TERMINAL_EVENTS);
    assert(!input_tcsetpgrp(31,19)); bytes("\3",1);
    assert(g_terminal.attrs.signal_dropped==1);
    worker_started=true; unsigned before=wakes; input_timer_tick(100); assert(wakes>before);
    while (drain_event()) {}
    assert(!g_terminal.event_count);
    /* Release every queue/foreground owner; no retained group slots leak. */
    assert(process_group_release_ref(&g_terminal.foreground));
    for (unsigned i=1;i<12;++i) process_record_abort(10+i);
    process_record_abort(10);
    for (unsigned i=0;i<PROCESS_GROUP_CAPACITY;++i) assert(!groups[i].refs && !groups[i].members);
    puts("PASS S8 terminal host: access, wake recheck, fd/session, attributes, ingress, handoff, overflow, references");
}
