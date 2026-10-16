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
#include "apic.h"
#include "net_udp_owner.h"
static bool range(uintptr_t p, size_t n, bool write) {
    return !n || vmm_validate_user_range(vmm_get_active_pml4_virt(),p,n,write);
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
    if (!caller) return SYSCALL_EOPNOTSUPP;
    if ((f->rax==SYS_LISTEN || f->rax==SYS_ACCEPT) &&
        (cpu_current()->id || caller->cpu_affinity!=0)) return SYSCALL_EOPNOTSUPP;
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
        int64_t rc=net_udp_begin(NET_OWNER_TCP_SHUTDOWN,file,NULL,0,false);
        if (rc) return rc;
        handoff_token_t t=caller->handoff.token;
        while (!handoff_ready(&t)) {
            if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
            sched_wait_until(handoff_channel(t),handoff_ready,&t);
        }
        handoff_reply_t reply;
        rc=net_udp_collect(&reply); net_udp_release();
        return rc ? SYSCALL_EIO : reply.status;
    }
    bool timed=f->rax==SYS_CONNECT_UNTIL || f->rax==SYS_SEND_UNTIL || f->rax==SYS_RECV_UNTIL;
    bool connect=f->rax==SYS_CONNECT || f->rax==SYS_CONNECT_UNTIL;
    bool receive=f->rax==SYS_RECV || f->rax==SYS_READ || f->rax==SYS_RECV_UNTIL;
    bool send=f->rax==SYS_SEND || f->rax==SYS_WRITE || f->rax==SYS_SEND_UNTIL;
    uint64_t deadline=connect ? f->r10 : f->r8;
    uint64_t clock_floor=timed ? apic_timer_get_bsp_ticks() : 0;
    if (!connect && !receive && !send) return SYSCALL_ENOSYS;
    net_sockaddr_in_t address={0};
    if (connect) {
        if (f->rdx!=16) return SYSCALL_EINVAL;
        if (!range(f->rsi,16,false)) return SYSCALL_EFAULT;
        memcpy(&address,(const void *)f->rsi,16);
        if (address.family!=NET_AF_INET || !address.port || !net_ipv4_unicast(address.address)) return SYSCALL_EINVAL;
        for (unsigned i=0; i<8; ++i) if (address.reserved[i]) return SYSCALL_EINVAL;
        if (!net_socket_available()) return SYSCALL_EIO;
        if (timed) {
            uint64_t hz=apic_timer_get_frequency(), now=apic_timer_get_bsp_ticks();
            if (!net_tcp_deadline_clock_ok() || !hz || hz>UINT64_MAX/NET_TCP_IO_HORIZON_SECONDS) return SYSCALL_EIO;
            if (deadline>now && deadline-now>hz*NET_TCP_IO_HORIZON_SECONDS) return SYSCALL_EINVAL;
        }
    } else {
        if (f->rdx>(receive ? TCP_RXBUF_MAX : MAX_SYSCALL_WRITE_LEN) ||
            (timed && f->r10) ||
            ((f->rax==SYS_SEND || f->rax==SYS_RECV) && f->r10 && f->r10!=NET_MSG_DONTWAIT)) return SYSCALL_EINVAL;
        if (!range(f->rsi,f->rdx,receive)) return SYSCALL_EFAULT;
        if (!f->rdx) return 0;
        if (timed) {
            uint64_t hz=apic_timer_get_frequency(), now=apic_timer_get_bsp_ticks();
            if (!net_tcp_deadline_clock_ok() || !hz || hz>UINT64_MAX/NET_TCP_IO_HORIZON_SECONDS) return SYSCALL_EIO;
            if (deadline>now && deadline-now>hz*NET_TCP_IO_HORIZON_SECONDS) return SYSCALL_EINVAL;
        }
    }
    bool nb=!timed && (f->rax==SYS_SEND || f->rax==SYS_RECV) && f->r10==NET_MSG_DONTWAIT;
    size_t length=f->rdx;
    if (send && length>4096) length=4096;
    if (receive && length>TCP_RXBUF_MAX) length=TCP_RXBUF_MAX;
    int64_t rc=net_tcp_owner_begin(connect ? NET_OWNER_TCP_CONNECT :
        receive ? NET_OWNER_TCP_RECV : NET_OWNER_TCP_SEND,file,connect ? &address : NULL,
        length,nb,timed,deadline,clock_floor);
    if (rc) {
        if (!connect && (rc==SYSCALL_EIO || rc==SYSCALL_ETIMEDOUT)) {
            net_tcp_wait_t wait;
            if (!net_tcp_snapshot(slot,&wait)) return SYSCALL_ENOTCONN;
            if (!range(f->rsi,f->rdx,receive)) return SYSCALL_EFAULT;
            if (!net_socket_available()) return SYSCALL_EIO;
        }
        return rc;
    }
    handoff_token_t t=caller->handoff.token;
    bool decision=false;
    for (;;) {
        handoff_reply_t reply;
        rc=net_udp_collect(&reply);
        if (!rc) {
            net_udp_release();
            if (send && reply.status==SYSCALL_EPIPE)
                (void)process_signal_send_kernel(caller->tid,(int64_t)caller->tid,SIGPIPE);
            return reply.status;
        }
        if (rc<0) { net_udp_abandon(); return rc; }
        if (decision) {
            /* Copy decision is published. Deferral is only for the bounded
             * owner commit/cancel acknowledgement, never peer/resource waits. */
            sched_wait_until_ack(handoff_channel(t),handoff_ready,&t);
            continue;
        }
        if (process_signal_interrupt()) {
            net_tcp_owner_cancel(SYSCALL_EINTR); decision=true; continue;
        }
        if (connect && net_tcp_owner_connect_ack()) { decision=true; continue; }
        if (!connect) {
            if (!range(f->rsi,f->rdx,receive)) { net_udp_abandon(); return SYSCALL_EFAULT; }
            if (timed) {
                uint64_t now=apic_timer_get_bsp_ticks();
                int64_t error=(!net_tcp_deadline_clock_ok() || now<clock_floor) ? SYSCALL_EIO :
                    now>=deadline ? SYSCALL_ETIMEDOUT : 0;
                if (error) { net_tcp_owner_cancel(error); decision=true; continue; }
                clock_floor=now;
            }
            const uint8_t *data; int64_t count;
            if (net_udp_pin(&data,&count,NULL)) {
                if (receive) memcpy((void *)f->rsi,data,(size_t)count);
                else memcpy((void *)data,(const void *)f->rsi,(size_t)count);
                net_udp_unpin(true);
                if (receive) decision=true;
                /* SEND copy alone is not accepted TX. Owner may need space
                 * again; ordinary waits remain interruptible until accepted. */
                continue;
            }
        }
        sched_wait_until(handoff_channel(t),net_udp_ready,&t);
    }
}
