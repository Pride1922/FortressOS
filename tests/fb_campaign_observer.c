/* Compiled only by the isolated campaign runner, never a production source. */
#include "thread.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "percpu.h"
#include "string.h"
#include "syscall_abi.h"
#include "fb_campaign.h"
extern bool fb_campaign_root_allocated(uintptr_t);
static uintptr_t reaping[64];
void fb_campaign_reap_begin(uintptr_t root) {
    for(unsigned i=0;i<64;i++) {
        uintptr_t empty=0;
        if(__atomic_compare_exchange_n(&reaping[i],&empty,root,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    }
    __builtin_trap();
}
void fb_campaign_reap_end(uintptr_t root) {
    for(unsigned i=0;i<64;i++)if(__atomic_load_n(&reaping[i],__ATOMIC_ACQUIRE)==root) {
        __atomic_store_n(&reaping[i],0,__ATOMIC_RELEASE);return;
    }
    __builtin_trap();
}
static bool root_reaping(uintptr_t root) {
    for(unsigned i=0;i<64;i++)if(__atomic_load_n(&reaping[i],__ATOMIC_ACQUIRE)==root)return true;
    return false;
}
static uintptr_t fault_root;
static uint64_t fault_preempts,fault_cpu;
void fb_campaign_note_fault(interrupt_frame_t *frame) {
    tcb_t *t=thread_current();
    uintptr_t address;
    __asm__ volatile("mov %%cr2,%0":"=r"(address));
    if(t && (frame->cs&3)==3 && frame->vector==14 && address==0x6000000000ULL) {
        fault_preempts=t->preempt_count;fault_cpu=cpu_current()->id;
        __atomic_store_n(&fault_root,t->cr3,__ATOMIC_RELEASE);
    }
}
static uintptr_t releasing;
static uint64_t remote_mask, remote_preempts[MAX_DETECTED_CPUS];
void fb_campaign_release_begin(uintptr_t root) {
    __atomic_store_n(&remote_mask,0,__ATOMIC_RELEASE);
    __atomic_store_n(&releasing,root,__ATOMIC_RELEASE);
}
void fb_campaign_release_end(void) { __atomic_store_n(&releasing,0,__ATOMIC_RELEASE); }
void fb_campaign_note_ipi(interrupt_frame_t *frame) {
    uintptr_t root=__atomic_load_n(&releasing,__ATOMIC_ACQUIRE), active;
    __asm__ volatile("mov %%cr3,%0":"=r"(active));
    tcb_t *t=thread_current();size_t cpu=cpu_current()->id;
    if(root && (active&PTE_ADDR_MASK)==root && (frame->cs&3)==3 && t && t->cr3==root && cpu<MAX_DETECTED_CPUS) {
        __atomic_store_n(&remote_preempts[cpu],t->preempt_count,__ATOMIC_RELEASE);
        __atomic_fetch_or(&remote_mask,1ULL<<cpu,__ATOMIC_RELEASE);
    }
}
int64_t fb_campaign_dispatch(uint64_t op,uintptr_t a,uint64_t bytes) {
    tcb_t *t=thread_current();
    if(!t || !t->is_user || strcmp(t->name,"/bin/fb-smp-probe"))return SYSCALL_EPERM;
    if(op==FB_TEST_CPU) {
        size_t cpu=cpu_current()->id;
        return t->cpu_affinity==(int)cpu && t->cpus_allowed==CPU_MASK_ONE(cpu) ? (int64_t)cpu:SYSCALL_EINVAL;
    }
    if(op==FB_TEST_BITMAP) {
        size_t needed=(pmm_get_total_pages()+7)/8;
        if(bytes<needed || bytes>PMM_BITMAP_CAPACITY_BYTES || !vmm_validate_user_range(t->pml4_virt,a,bytes,true))return SYSCALL_EFAULT;
        return pmm_snapshot((void *)a,bytes)?(int64_t)needed:SYSCALL_EIO;
    }
    if(op==FB_TEST_FAULT_PREEMPTS) {
        if(__atomic_load_n(&fault_root,__ATOMIC_ACQUIRE)!=a || fault_cpu!=1)return SYSCALL_EINVAL;
        return fault_preempts;
    }
    if(op==FB_TEST_ROOT)return t->cr3;
    if(op==FB_TEST_ROOT_LIVE)return vmm_space_lookup(a)!=NULL || fb_campaign_root_allocated(a) || root_reaping(a);
    if(op==FB_TEST_WARM_HEAP) {
        /* Heap backing is intentionally retained by the production allocator.
         * Precommit reusable capacity before the measured baseline. */
        void *warm=kmalloc_large(4*1024*1024);
        if(!warm)return SYSCALL_ENOMEM;
        kfree(warm);return 0;
    }
    if(op==FB_TEST_LEAK)return pmm_alloc_page(); // explicit negative-control fixture only
    if(op==FB_TEST_THREAD_LIVE)return thread_is_alive(a);
    if(op==FB_TEST_PREEMPTS)return t->preempt_count;
    if(op==FB_TEST_REMOTE_MASK)return __atomic_load_n(&remote_mask,__ATOMIC_ACQUIRE);
    if(op==FB_TEST_REMOTE_PREEMPTS && a<MAX_DETECTED_CPUS)return __atomic_load_n(&remote_preempts[a],__ATOMIC_ACQUIRE);
    return SYSCALL_EINVAL;
}
