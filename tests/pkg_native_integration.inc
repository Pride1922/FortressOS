/* Private copied-kernel only. No public root or authentication publication. */
#include "accounts.h"
int jn_accounts_prepare(ext4_mount_t *);
int64_t jn_accounts_control(unsigned,uintptr_t,size_t,const creds_t *,uint64_t);
static unsigned jn_wait,jn_ready,jn_seen,jn_steps;
static uint64_t jn_paused_pid;
static bool jn_ack_loss,jn_flush_ack_loss;
static unsigned jn_account_state; /* 0 closed, 1 preparing, 2 ready; immutable afterwards. */
int jn_publish_gate(ext4_mount_t *);
/* Mount identity is retained for the boot, not selected by an account request. */
int jn_root_ready(ext4_mount_t *fs){
    if(!jb_engine || fs!=jb_engine->fs || !__atomic_load_n(&jb_admitted,__ATOMIC_ACQUIRE))return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=jb_validate(jb_engine);
    if(!r && pn_view.action!=PP_DONE)r=-VFS_EAGAIN;
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
int jn_account_operation(unsigned op){
    int r=jn_root_ready(jb_engine?jb_engine->fs:NULL);if(r)return r;
    if(__atomic_load_n(&jn_account_state,__ATOMIC_ACQUIRE)!=2)return -VFS_EAGAIN;
    return op==ACCOUNT_BEGIN || op==ACCOUNT_UPDATE || op==ACCOUNT_FINISH?-VFS_EROFS:0;
}
static int jn_attach_root(void){
    ext4_mount_t *fs=jb_engine->fs;if(fs->nodes)return -VFS_EEXIST;
    e4_node_t *node=kcalloc(1,sizeof *node);if(!node)return -VFS_ENOMEM;
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t root;int r=jb_validate(jb_engine);
    if(!r)r=e4_inode(fs,2,&root);
    if(!r){fs->trusted_root=true;memcpy(node->node.name,"/",2);memcpy(node->node.path,"/",2);e4_setup(node,&root);fs->cached[fs->nodes++]=node;}
    spin_unlock_irqrestore(&e4_lock,flags);if(r){kfree(node);return r;}
    ext4_account_namespace_report_t report;return ext4_root_account_namespace_admit(fs,&report);
}
static bool jn_released(void *unused){(void)unused;return !__atomic_load_n(&jn_wait,__ATOMIC_ACQUIRE);}
static bool jn_boundary(unsigned completed){
    spin_debug_assert_unheld();unsigned mode=__atomic_load_n(&jn_wait,__ATOMIC_ACQUIRE);
    if(mode && (mode==1 || completed)){
        if(!jn_seen){jn_seen=1;__atomic_store_n(&jn_paused_pid,thread_current()->tid,__ATOMIC_RELEASE);__atomic_add_fetch(&jn_ready,1,__ATOMIC_RELEASE);}
        sched_wait_until(&jn_wait,jn_released,NULL);
    }
    return process_signal_interrupt();
}
int64_t jn_test(uint64_t op){
    if(op==10 || op==11){jn_seen=0;__atomic_store_n(&jn_wait,op==10?1:2,__ATOMIC_RELEASE);return 0;}
    if(op==12)return __atomic_load_n(&jn_ready,__ATOMIC_ACQUIRE);
    if(op==15)return __atomic_load_n(&jn_steps,__ATOMIC_ACQUIRE);
    if(op==17){jn_ack_loss=true;return 0;}
    if(op==18){jn_flush_ack_loss=true;return 0;}
    if(op==20){
        int r=jn_root_ready(jb_engine->fs);if(r)return r;
        unsigned expected=0;
        if(!__atomic_compare_exchange_n(&jn_account_state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return expected==2?0:-VFS_EAGAIN;
        r=jn_accounts_prepare(jb_engine->fs);if(!r)r=jn_root_ready(jb_engine->fs);
        __atomic_store_n(&jn_account_state,r?0:2,__ATOMIC_RELEASE);return r;
    }
    if(op==21)return jn_root_ready(jb_engine->fs);
    if(op==22){if(!jn_publish_gate(jb_engine->fs))return 0;return ext4_publish_root(jb_engine->fs);}
    if(op==24){
        uint64_t pid=__atomic_load_n(&jn_paused_pid,__ATOMIC_ACQUIRE);creds_t old,next;
        if(!pid||!process_record_creds(pid,&old))return -VFS_EINVAL;
        next=old;next.cap_effective&=~CAP_SYS_ADMIN;
        if(!process_record_publish_creds(pid,&old,&next))return -VFS_EAGAIN;
        __atomic_store_n(&jn_wait,0,__ATOMIC_RELEASE);sched_wake_all(&jn_wait);return 0;
    }
    return SYSCALL_EINVAL;
}
int64_t jn_account_transport(unsigned op,uintptr_t address,size_t length){
    tcb_t *t=thread_current();creds_t actor;if(!t||!process_record_creds(t->tid,&actor))return SYSCALL_EPERM;
    return jn_accounts_control(op,address,length,&actor,t->tid);
}
