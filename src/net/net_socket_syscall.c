#include "net_socket_syscall.h"
#include "net_socket.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
#include "thread.h"
#include "percpu.h"
#include "vmm.h"
#include "string.h"
#include "net_tcp_syscall.h"

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
int64_t net_socket_syscall(interrupt_frame_t *f) {
    tcb_t *caller=thread_current();
    if (cpu_current()->id || !caller || caller->cpu_affinity!=0) return SYSCALL_EOPNOTSUPP;
    if (f->rax==SYS_SOCKET) {
        bool stream=f->rsi==NET_SOCK_STREAM || f->rsi==(NET_SOCK_STREAM|NET_SOCK_CLOEXEC);
        if (f->rdi!=NET_AF_INET || (!stream && f->rsi!=NET_SOCK_DGRAM &&
            f->rsi!=(NET_SOCK_DGRAM|NET_SOCK_CLOEXEC)) || (f->rdx && f->rdx!=(stream ? 6u : 17u))) return SYSCALL_EOPNOTSUPP;
        if (!net_socket_available()) return SYSCALL_EIO;
        unsigned i=0;
        for (; i<MAX_PROCESS_FDS; ++i) if (!caller->fd_table[i]) break;
        if (i==MAX_PROCESS_FDS) return SYSCALL_EMFILE;
        file_t *file; int64_t ret=stream ? net_socket_create_stream(&file) : net_socket_create(&file);
        if (ret) return ret;
        int fd=fd_alloc(caller,file);
        if (fd<0) { vfs_close(file); return SYSCALL_EMFILE; }
        if (f->rsi&NET_SOCK_CLOEXEC) caller->fd_flags[fd]=FD_FLAG_CLOEXEC;
        return fd;
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
        if (!fields(&a) || (a.address && a.address!=net_ipv4_local())) return SYSCALL_EINVAL;
        return net_socket_bind(file,&a);
    }
    net_socket_wait_t wait;
    int64_t ret;
    bool receiving=f->rax==SYS_RECVFROM;
    if (!receiving) {
        if (f->r10 || f->rdx>NET_UDP_DATA_MAX || f->r9!=sizeof(net_sockaddr_in_t)) return SYSCALL_EINVAL;
        if (!range(f->r8,sizeof(net_sockaddr_in_t),false)) return SYSCALL_EFAULT;
        net_sockaddr_in_t a; memcpy(&a,(const void *)f->r8,sizeof(a));
        if (!range(f->rsi,f->rdx,false)) return SYSCALL_EFAULT;
        if (!fields(&a) || !a.port || !net_ipv4_unicast(a.address) || a.address==net_ipv4_local()) return SYSCALL_EINVAL;
        ret=net_socket_send(file,&a,(const void *)f->rsi,f->rdx,&wait);
    } else {
        if ((f->r10 && f->r10!=NET_MSG_DONTWAIT) || f->rdx>NET_UDP_DATA_MAX ||
            (!!f->r8!=!!f->r9)) return SYSCALL_EINVAL;
        ret=receive_ranges(f,true); if (ret) return ret;
        ret=net_socket_receive(file,f->r10!=0,&wait);
    }
    if (ret) return ret;
    sched_wait_until(net_socket_channel(&wait),net_socket_ready,&wait);
    if (process_signal_interrupt()) { net_socket_finish(&wait,false); return SYSCALL_EINTR; }
    if (!net_socket_live(&wait)) return SYSCALL_EINTR;
    if (receiving) {
        ret=receive_ranges(f,false);
        if (ret) { net_socket_finish(&wait,false); return ret; }
    }
    const uint8_t *data=NULL; net_sockaddr_in_t source;
    ret=net_socket_result(&wait,&data,&source);
    if (ret>=0 && receiving) {
        size_t n=(size_t)ret; if (n>f->rdx) n=f->rdx;
        /* Own continuation with IF clear: no worker expiry or signal handler
         * between staging/copy/commit; all user copies occur without locks. */
        if (n) memcpy((void *)f->rsi,data,n);
        if (f->r8) {
            uint32_t size=sizeof(source);
            memcpy((void *)f->r8,&source,sizeof(source)); memcpy((void *)f->r9,&size,4);
        }
        ret=(int64_t)n;
    }
    net_socket_finish(&wait,receiving && ret>=0); return ret;
}
