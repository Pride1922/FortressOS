/* Private copied-kernel translation unit; never compiled by the release tree. */
#include "thread.h"
#include "percpu.h"
#include "process_table.h"
#include "syscall.h"
#define assert(x) do { if(!(x))__builtin_trap(); } while(0)
#include "../../tests/pkg_writer_candidate.c"
#include "../../tests/pkg_mutation_candidate.c"
#include "../../tests/pkg_call_candidate.c"
static unsigned call_kernel_exits,call_kernel_reaps;
static uint64_t call_kernel_cpus;
void call_kernel_exit(void){
    spin_debug_assert_unheld();tcb_t *t=thread_current();
    if(t&&t->package_call){assert(pc_cancel_current(t)&&pc_end(t));__atomic_add_fetch(&call_kernel_exits,1,__ATOMIC_RELEASE);}
}
bool call_kernel_reap(tcb_t *t){assert(!t->package_call);return t->package_call_seen;}
void call_kernel_reaped(bool tracked){if(tracked)__atomic_add_fetch(&call_kernel_reaps,1,__ATOMIC_RELEASE);}
static bool call_kernel_start(bool account,bool pin){
    tcb_t *t=thread_current();assert(t&&!t->package_call);(void)pm_configure(1);pm_key key;
    if((account?pm_account(1,&key):pm_ordinary(1,&key)))return false;
    if(!pc_start(t,key,1,t)){assert(pm_finish(key));return false;}
    if(pin&&!pc_pin(t)){assert(pc_end(t));return false;}
    t->package_call_seen=true;__atomic_fetch_or(&call_kernel_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);return true;
}
int64_t call_kernel_test(uint64_t op,uint64_t caught){
    spin_debug_assert_unheld();tcb_t *t=thread_current();assert(t&&t->is_user);
    if(op==8){thread_yield();return 0;}
    if(op==0)return pc_count();
    if(op==4){unsigned n,p;bool a;pm_counts(&n,&p,&a);assert(!n&&!p&&!a&&!pc_count());return __atomic_load_n(&call_kernel_reaps,__ATOMIC_ACQUIRE);}
    if(op==6)return (int64_t)__atomic_load_n(&call_kernel_cpus,__ATOMIC_RELAXED);
    if(op==7)return __atomic_load_n(&call_kernel_exits,__ATOMIC_ACQUIRE);
    if(op==1||op==3)return call_kernel_start(op==3,op==1)?0:SYSCALL_EAGAIN;
    assert(op==2||op==5);assert(call_kernel_start(false,true));
    if(op==5){
        assert(pc_commit_begin(t));assert(!process_signal_send_kernel(t->tid,(int64_t)t->tid,SIGKILL));
        assert(process_signal_interrupt()&&pc_cancel_current(t)&&!pc_end(t));pc_commit_end(t);
        assert(!pc_commit_begin(t));return SYSCALL_EINTR;
    }
    unsigned attempts=0;
    while(!process_signal_interrupt()){
        assert(++attempts<1000000);thread_yield();
        __atomic_fetch_or(&call_kernel_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);
    }
    assert(pc_cancel_current(t)&&!pc_commit_begin(t));
    if(caught)assert(pc_end(t)); /* unwind before actual custom-handler delivery */
    return SYSCALL_EINTR; /* default termination uses actual process_exit hook */
}
