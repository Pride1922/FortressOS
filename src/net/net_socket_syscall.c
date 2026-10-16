#include "net_socket_syscall.h"
#include "net_socket.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
#include "thread.h"
#include "percpu.h"
#include "vmm.h"
#include "string.h"
#include "net_tcp_syscall.h"
#include "process_table.h"
#include "permissions.h"
#include "net_udp_owner.h"

static bool range(uintptr_t p, size_t n, bool write) {
    return !n || vmm_validate_user_range(vmm_get_active_pml4_virt(),p,n,write);
}
static bool fields(const net_sockaddr_in_t *a) {
    if (a->family!=NET_AF_INET) return false;
    for (unsigned i=0; i<8; ++i) if (a->reserved[i]) return false;
    return true;
}
static bool overlap(uintptr_t a, size_t an, uintptr_t b, size_t bn) {
    return an && bn && a<b+bn && b<a+an; /* Both ranges validated before arithmetic. */
}
static int64_t receive_ranges(interrupt_frame_t *f, bool first) {
    if (!range(f->rsi,f->rdx,true)) return SYSCALL_EFAULT;
    if (!f->r8) return 0;
    if (!range(f->r9,4,true)) return SYSCALL_EFAULT;
    if (first) {
        uint32_t capacity; memcpy(&capacity,(const void *)f->r9,4);
        if (capacity<sizeof(net_sockaddr_in_t)) return SYSCALL_EINVAL;
    }
    if (!range(f->r8,sizeof(net_sockaddr_in_t),true)) return SYSCALL_EFAULT;
    if (overlap(f->rsi,f->rdx,f->r8,16) || overlap(f->rsi,f->rdx,f->r9,4) ||
        overlap(f->r8,16,f->r9,4)) return SYSCALL_EINVAL;
    return 0;
}
static int64_t terminal(bool accepted_send) {
    handoff_token_t t=thread_current()->handoff.token;
    for (;;) {
        handoff_reply_t reply;
        int64_t rc=net_udp_collect(&reply);
        if (!rc && accepted_send) { net_udp_release(); return reply.status; }
        if (process_signal_interrupt()) {
            if (!accepted_send) { net_udp_abandon(); return SYSCALL_EINTR; }
            /* Resolve accepted side effects before returning EINTR. This is
             * owner acknowledgement only, never a wait for the remote peer. */
            handoff_cancel(t);
            sched_wait_until_ack(handoff_channel(t),handoff_ready,&t);
            continue;
        }
        if (!rc) { net_udp_release(); return reply.status; }
        if (rc<0) { net_udp_abandon(); return rc; }
        sched_wait_until(handoff_channel(t),handoff_ready,&t);
    }
}
int64_t net_socket_syscall(interrupt_frame_t *f) {
    tcb_t *caller=thread_current();
    if (!caller) return SYSCALL_EOPNOTSUPP;
    if (f->rax==SYS_SOCKET) {
        bool stream=f->rsi==NET_SOCK_STREAM || f->rsi==(NET_SOCK_STREAM|NET_SOCK_CLOEXEC);
        if (f->rdi!=NET_AF_INET || (!stream && f->rsi!=NET_SOCK_DGRAM &&
            f->rsi!=(NET_SOCK_DGRAM|NET_SOCK_CLOEXEC)) || (f->rdx && f->rdx!=(stream ? 6u : 17u))) return SYSCALL_EOPNOTSUPP;
        if (!net_socket_available()) return SYSCALL_EIO;
        unsigned i=0;
        for (; i<MAX_PROCESS_FDS; ++i) if (!caller->fd_table[i]) break;
        if (i==MAX_PROCESS_FDS) return SYSCALL_EMFILE;
        file_t *file=NULL; int64_t ret;
        {
            ret=net_udp_begin(stream ? NET_OWNER_TCP_CREATE : NET_OWNER_UDP_CREATE,NULL,NULL,0,false);
            if (ret) return ret;
            handoff_token_t t=caller->handoff.token;
            while (!handoff_ready(&t)) {
                if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
                sched_wait_until(handoff_channel(t),handoff_ready,&t);
            }
            if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
            handoff_reply_t reply; ret=net_udp_collect(&reply);
            if (!ret) ret=reply.status;
            if (!ret) file=net_udp_created();
            if (!ret && !file) ret=SYSCALL_EIO;
            /* File installation is caller-only and cannot sleep. The separate
             * operation reference survives rollback/death until owner ack. */
            if (!ret) {
                int fd=fd_alloc(caller,file);
                if (fd<0) { vfs_close(file); ret=SYSCALL_EMFILE; }
                else { if (f->rsi&NET_SOCK_CLOEXEC) caller->fd_flags[fd]=FD_FLAG_CLOEXEC; ret=fd; }
            }
            net_udp_release(); return ret;
        }

    }
    if (f->rdi>=MAX_PROCESS_FDS) return SYSCALL_EBADF;
    file_t *file=fd_get(caller,(int)f->rdi);
    if (!net_socket_file(file)) return SYSCALL_EBADF;
    if (f->rax>=SYS_CONNECT) return net_tcp_syscall(f);
    if (f->rax!=SYS_BIND && net_socket_stream(file)) return SYSCALL_EOPNOTSUPP;
    if (f->rax==SYS_BIND) {
        if (f->rdx!=sizeof(net_sockaddr_in_t)) return SYSCALL_EINVAL;
        if (!range(f->rsi,sizeof(net_sockaddr_in_t),false)) return SYSCALL_EFAULT;
        net_sockaddr_in_t a; memcpy(&a,(const void *)f->rsi,sizeof(a));
        if (!fields(&a) || !net_socket_address_snapshot(a.address,false)) return SYSCALL_EINVAL;
        if (a.port && ntohs(a.port)<1024) {
            creds_t actor;
            if (!process_record_creds(caller->tid,&actor)) return SYSCALL_ESRCH;
            if (permission_capability(&actor,CAP_NET_BIND)) return SYSCALL_EPERM;
        }
        int64_t ret=net_udp_begin(net_socket_stream(file) ? NET_OWNER_TCP_BIND : NET_OWNER_UDP_BIND,file,&a,0,false);
        return ret ? ret : terminal(false);
    }
    int64_t ret;
    bool receiving=f->rax==SYS_RECVFROM;
    if (!receiving) {
        if (f->r10 || f->rdx>NET_UDP_DATA_MAX || f->r9!=sizeof(net_sockaddr_in_t)) return SYSCALL_EINVAL;
        if (!range(f->r8,sizeof(net_sockaddr_in_t),false)) return SYSCALL_EFAULT;
        net_sockaddr_in_t a; memcpy(&a,(const void *)f->r8,sizeof(a));
        if (!range(f->rsi,f->rdx,false)) return SYSCALL_EFAULT;
        if (!fields(&a) || !a.port || !net_socket_address_snapshot(a.address,true)) return SYSCALL_EINVAL;
        ret=net_udp_begin(NET_OWNER_UDP_SEND,file,&a,f->rdx,false);
    } else {
        if ((f->r10 && f->r10!=NET_MSG_DONTWAIT) || f->rdx>NET_UDP_DATA_MAX ||
            (!!f->r8!=!!f->r9)) return SYSCALL_EINVAL;
        ret=receive_ranges(f,true); if (ret) return ret;
        ret=net_udp_begin(NET_OWNER_UDP_RECV,file,NULL,0,f->r10!=0);
    }
    if (ret) return ret;
    handoff_token_t t=caller->handoff.token;
    for (;;) {
        handoff_reply_t reply;
        if (!net_udp_collect(&reply)) { net_udp_release(); return reply.status; }
        if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
        sched_wait_until(handoff_channel(t),net_udp_ready,&t);
        if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
        ret=receiving ? receive_ranges(f,false) :
            (range(f->rsi,f->rdx,false) ? 0 : SYSCALL_EFAULT);
        if (ret) { net_udp_abandon(); return ret; }
        const uint8_t *data; net_sockaddr_in_t source;
        if (!net_udp_pin(&data,&ret,&source)) continue;
        /* No sleep/signal check while pinned. Remote owner cannot revoke or
         * reuse the slab until release, even if its one-second lease expires. */
        if (!receiving) { if (f->rdx) memcpy((void *)data,(const void *)f->rsi,f->rdx); }
        else {
            size_t n=(size_t)ret; if (n>f->rdx) n=f->rdx;
            if (n) memcpy((void *)f->rsi,data,n);
            if (f->r8) {
                uint32_t size=sizeof(source);
                memcpy((void *)f->r8,&source,sizeof(source)); memcpy((void *)f->r9,&size,4);
            }
            ret=(int64_t)n;
        }
        net_udp_unpin(true);
        int64_t done=terminal(true);
        return receiving && done>=0 ? ret : done;
    }
}
