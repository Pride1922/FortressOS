#include "net_tcp_syscall.h"
#include "net_tcp.h"
#include "net_socket.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
#include "thread.h"
#include "process_table.h"
#include "percpu.h"
#include "vmm.h"
#include "string.h"
/* BSP IF-clear only, no retained user pointer or packet-size stack buffer. */
static uint8_t received[TCP_BUFFER_SIZE];
static bool range(uintptr_t p, size_t n, bool write) {
    return !n || vmm_validate_user_range(vmm_get_active_pml4_virt(),p,n,write);
}
static bool interrupted(unsigned slot, const net_tcp_wait_t *wait, bool connect) {
    if (!process_signal_interrupt()) return false;
    if (connect) net_tcp_cancel_connect(slot,wait->generation);
    return true;
}
static int64_t accept_outputs(const interrupt_frame_t *f) {
    if (!!f->rsi!=!!f->rdx || (f->r10 && f->r10!=NET_SOCK_CLOEXEC)) return SYSCALL_EINVAL;
    if (!f->rsi) return 0;
    if (!range(f->rsi,16,true) || !range(f->rdx,4,true)) return SYSCALL_EFAULT;
    uint32_t size; memcpy(&size,(const void *)f->rdx,4);
    if (size<16 || (f->rsi<f->rdx+4 && f->rdx<f->rsi+16)) return SYSCALL_EINVAL;
    return 0;
}
static int64_t accept_socket(tcb_t *caller, unsigned slot, interrupt_frame_t *f) {
    if (!net_tcp_listener(slot)) return SYSCALL_EINVAL;
    net_tcp_wait_t wait;
    if (!net_tcp_snapshot(slot,&wait)) return SYSCALL_EBADF;
    uint64_t identity=wait.generation;
    for (;;) {
        int64_t result=accept_outputs(f); if (result) return result;
        if (!net_socket_available()) return SYSCALL_EIO;
        if (process_signal_interrupt()) return SYSCALL_EINTR;
        /* STOP may switch inside the signal check. Repeat validation AFTER it
         * and stage nothing until all potentially blocking checks finish. */
        result=accept_outputs(f); if (result) return result;
        if (!net_socket_available()) return SYSCALL_EIO;
        if (!net_tcp_snapshot(slot,&wait) || wait.generation!=identity) return SYSCALL_EBADF;
        net_tcp_child_t child; net_sockaddr_in_t peer;
        result=net_tcp_accept_peek(slot,&child,&peer);
        if (result==SYSCALL_EAGAIN) {
            sched_wait_until(net_tcp_channel(&wait),net_tcp_ready,&wait); continue;
        }
        if (result) return result;
        /* BSP IF remains clear: queued child and outputs cannot change. The
         * staged constructor owns no transport block; failure leaves queue
         * membership untouched. No signal check/sleep after this point. */
        file_t *accepted; result=net_socket_stage_stream(&accepted);
        if (result) return result;
        int fd=fd_alloc(caller,accepted);
        if (fd<0) { vfs_close(accepted); return SYSCALL_EMFILE; }
        net_tcp_accept_commit(slot,net_socket_index(accepted),child);
        net_socket_finish_adopt(accepted);
        if (f->r10==NET_SOCK_CLOEXEC) caller->fd_flags[fd]=FD_FLAG_CLOEXEC;
        if (f->rsi) {
            uint32_t size=16;
            memcpy((void *)f->rsi,&peer,16); memcpy((void *)f->rdx,&size,4);
        }
        return fd;
    }
}
int64_t net_tcp_syscall(interrupt_frame_t *f) {
    tcb_t *caller=thread_current();
    if (cpu_current()->id || !caller || caller->cpu_affinity!=0) return SYSCALL_EOPNOTSUPP;
    if (f->rdi>=MAX_PROCESS_FDS) return SYSCALL_EBADF;
    file_t *file=fd_get(caller,(int)f->rdi);
    if (!net_socket_stream(file)) return SYSCALL_EBADF;
    unsigned slot=net_socket_index(file);
    if (f->rax==SYS_LISTEN) {
        if (!f->rsi || f->rsi>NET_TCP_BACKLOG_MAX) return SYSCALL_EINVAL;
        if (!net_socket_available()) return SYSCALL_EIO;
        return net_tcp_listen(slot,(unsigned)f->rsi);
    }
    if (f->rax==SYS_ACCEPT) return accept_socket(caller,slot,f);
    if (f->rax==SYS_SHUTDOWN) {
        if (f->rsi!=NET_SHUT_WR) return SYSCALL_EINVAL;
        return net_tcp_shutdown(slot);
    }
    bool connect=f->rax==SYS_CONNECT;
    bool receive=f->rax==SYS_RECV || f->rax==SYS_READ;
    bool send=f->rax==SYS_SEND || f->rax==SYS_WRITE;
    if (!connect && !receive && !send) return SYSCALL_ENOSYS;
    if (connect) {
        if (f->rdx!=16) return SYSCALL_EINVAL;
        if (!range(f->rsi,16,false)) return SYSCALL_EFAULT;
        net_sockaddr_in_t address; memcpy(&address,(const void *)f->rsi,16);
        if (address.family!=NET_AF_INET || !address.port || !net_ipv4_unicast(address.address)) return SYSCALL_EINVAL;
        for (unsigned i=0; i<8; ++i) if (address.reserved[i]) return SYSCALL_EINVAL;
        if (!net_socket_available()) return SYSCALL_EIO;
        int64_t result=net_tcp_connect(slot,address.address,ntohs(address.port));
        if (result) return result;
    } else {
        if (f->rdx>MAX_SYSCALL_WRITE_LEN ||
            ((f->rax==SYS_SEND || f->rax==SYS_RECV) && f->r10 && f->r10!=NET_MSG_DONTWAIT)) return SYSCALL_EINVAL;
        if (!range(f->rsi,f->rdx,receive)) return SYSCALL_EFAULT;
        if (!f->rdx) return 0;
    }
    net_tcp_wait_t wait;
    if (!net_tcp_snapshot(slot,&wait)) return SYSCALL_ENOTCONN;
    uint64_t identity=wait.generation;
    for (;;) {
        if (interrupted(slot,&wait,connect)) return SYSCALL_EINTR;
        if (!net_tcp_snapshot(slot,&wait) || wait.generation!=identity) return SYSCALL_ENOTCONN;
        /* Snapshot event before readiness test; IF stays clear through copy /
         * commit. Competing readers may run only while this continuation sleeps. */
        int64_t result;
        if (connect) result=net_tcp_connected(slot);
        else {
            if (!range(f->rsi,f->rdx,receive)) return SYSCALL_EFAULT;
            if (!net_socket_available()) return SYSCALL_EIO;
            if (receive) {
                size_t capacity=f->rdx; if (capacity>sizeof(received)) capacity=sizeof(received);
                result=net_tcp_peek(slot,received,capacity);
                if (result>0) {
                    memcpy((void *)f->rsi,received,(size_t)result);
                    (void)net_tcp_consume(slot,(size_t)result);
                }
            } else result=net_tcp_send(slot,(const void *)f->rsi,f->rdx);
        }
        if (result!=SYSCALL_EAGAIN) {
            if (send && result==SYSCALL_EPIPE)
                (void)process_signal_send(caller->tid,(int64_t)caller->tid,SIGPIPE);
            return result;
        }
        if (!connect && (f->rax==SYS_SEND || f->rax==SYS_RECV) && f->r10==NET_MSG_DONTWAIT) return result;
        sched_wait_until(net_tcp_channel(&wait),net_tcp_ready,&wait);
    }
}
