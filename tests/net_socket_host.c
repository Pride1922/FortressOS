#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef WNOHANG
#undef WUNTRACED
#undef WCONTINUED
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef WIFSTOPPED
#undef WSTOPSIG
#undef WIFCONTINUED
#include <string.h>
#include "net_socket.h"
#include "net_socket_syscall.h"
#include "net_ipv4.h"
#include "ipv4.h"
#include "udp.h"
#include "syscall_abi.h"
#include "thread.h"
#include "percpu.h"
static tcb_t process;
void net_test_assert_unheld(void);
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];
static uint64_t now;
static bool echo=true, cached=true, signal_pending, online=true, tx_fail;
static int fail_alloc=-1;
static unsigned allocations, sends, arps, wakes;
static uint8_t frame[1514], reply[1500];
static const uint8_t readonly[16]={0};
static net_dev_t dev;
static uint64_t timer_hz=100;
uint64_t apic_timer_get_frequency(void) { return timer_hz; }
uint64_t apic_timer_get_bsp_ticks(void) { return now; }
void *kmalloc(size_t n) {
    net_test_assert_unheld();
    if (fail_alloc==0) return NULL;
    if (fail_alloc>0) --fail_alloc;
    void *p=malloc(n); if (p) ++allocations; return p;
}
void kfree(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
tcb_t *thread_current(void) { return &process; }
bool process_signal_interrupt(void) { return signal_pending; }
int64_t process_signal_send(uint64_t sender, int64_t selector, unsigned signal) {
    (void)sender; (void)selector; (void)signal; return 0;
}
uint64_t *vmm_get_active_pml4_virt(void) { return NULL; }
bool vmm_validate_user_range(uint64_t *root, uintptr_t p, size_t n, bool write) {
    net_test_assert_unheld();
    (void)root;
    if (p<4096 || p>=0x800000000000ull || n>0x800000000000ull-p) return false;
    if (write && p<(uintptr_t)readonly+16 && (uintptr_t)readonly<p+n) return false;
    return true;
}
void (*net_host_fd_inserted)(tcb_t *, unsigned);
int fd_alloc(tcb_t *t, file_t *f) {
    for (unsigned i=0; i<32; ++i) if (!t->fd_table[i]) {
        t->fd_table[i]=f; t->fd_flags[i]=0;
        if (net_host_fd_inserted) net_host_fd_inserted(t,i);
        return i;
    }
    return -1;
}
file_t *fd_get(tcb_t *t, int fd) { return fd>=0 && fd<32 ? t->fd_table[fd] : NULL; }
int vfs_close(file_t *f) {
    if (__atomic_sub_fetch(&f->ref_count,1,__ATOMIC_ACQ_REL)==0) { f->node->close(f->node); kfree(f); }
    return 0;
}
static void closefd(int fd) { vfs_close(process.fd_table[fd]); process.fd_table[fd]=NULL; }
void sched_wake_all(const void *channel) { assert(channel); ++wakes; }
int net_arp_lookup(uint32_t ip, uint8_t mac[6]) { (void)ip; memset(mac,2,6); return cached ? 0 : -1; }
int arp_resolve(net_dev_t *d, uint32_t ip, uint8_t mac[6]) { (void)d; ++arps; return cached ? net_arp_lookup(ip,mac) : 1; }
static int transmit(net_dev_t *d, const void *bytes, size_t n) {
    net_test_assert_unheld();
    assert(d==&dev && n<=1514); ++sends; memcpy(frame,bytes,n);
    if (tx_fail) return -1;
    ipv4_header_t ip; const uint8_t *payload, *data; size_t len, dn; udp_header_t h;
    assert(!ipv4_decode(frame+14,n-14,&ip,&payload,&len));
    assert(!udp_decode(payload,len,ip.src_ip,ip.dst_ip,&h,&data,&dn));
    if (echo) {
        assert(!udp_encode(reply+20,1480,ip.dst_ip,ip.src_ip,h.destination,h.source,data,dn));
        assert(!ipv4_encode(reply,1500,ip.dst_ip,ip.src_ip,17,(uint16_t)(dn+8),64,NULL));
        net_ipv4_input(reply,dn+28);
    }
    return 0;
}
static void tick(void) {
    net_socket_worker_tick(now,online); net_ipv4_tick(now); net_socket_worker_tick(now,online); ++now;
}
void (*net_host_wait_override)(const void *, bool (*)(void *), void *);
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    if (net_host_wait_override) { net_host_wait_override(channel,ready,arg); return; }
    assert(channel);
    static spinlock_t scheduler=SPINLOCK_RANKED(1,"host_sched");
    for (unsigned i=0; i<1000; ++i) {
        uint64_t irq=spin_lock_irqsave(&scheduler);
        bool done=ready(arg) || signal_pending;
        spin_unlock_irqrestore(&scheduler,irq);
        if (done) return;
        tick();
    }
    assert(!"unbounded socket wait");
}
static int64_t call(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    interrupt_frame_t frame={.rax=nr,.rdi=a,.rsi=b,.rdx=c,.r10=d,.r8=e,.r9=f};
    return net_socket_syscall(&frame);
}
static int create(void) { return (int)call(SYS_SOCKET,2,2,0,0,0,0); }
static net_sockaddr_in_t destination, source;
static uint8_t buffer[1472], output[1472];
static uint32_t source_size;
static int64_t sendfd(int fd, size_t n) { return call(SYS_SENDTO,fd,(uintptr_t)buffer,n,0,(uintptr_t)&destination,16); }
static int64_t recvfd(int fd, size_t n, unsigned flags) {
    source_size=16;
    return call(SYS_RECVFROM,fd,(uintptr_t)output,n,flags,(uintptr_t)&source,(uintptr_t)&source_size);
}
int main(void) {
    process.cpu_affinity=0;
    dev=(net_dev_t){.mtu=1500,.send_packet=transmit};
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    destination=(net_sockaddr_in_t){.family=2,.port=htons(7777),.address=cfg.gateway};
    for (unsigned i=0; i<1472; ++i) buffer[i]=(uint8_t)i;
    cpu_locals[0].id=1; assert(create()==SYSCALL_EOPNOTSUPP); cpu_locals[0].id=0;
    process.cpu_affinity=-1; assert(create()==SYSCALL_EOPNOTSUPP); process.cpu_affinity=0;
    assert(call(SYS_SOCKET,2,0x100000002ull,0,0,0,0)==SYSCALL_EOPNOTSUPP);
    for (int fail=0; fail<2; ++fail) { fail_alloc=fail; assert(create()==SYSCALL_ENOMEM); assert(!allocations); }
    fail_alloc=-1;
    int fd=create(); assert(fd>=0);
    assert(call(SYS_SENDTO,fd,0,1473,0,0,16)==SYSCALL_EINVAL);
    assert(call(SYS_SENDTO,fd,0,1,0,(uintptr_t)&destination,16)==SYSCALL_EFAULT);
    assert(call(SYS_BIND,fd,0,16,0,0,0)==SYSCALL_EFAULT);
    assert(call(SYS_BIND,fd,0,15,0,0,0)==SYSCALL_EINVAL);
    net_sockaddr_in_t local={.family=2,.port=htons(7777)};
    assert(!call(SYS_BIND,fd,(uintptr_t)&local,16,0,0,0));
    int other=create(); assert(call(SYS_BIND,other,(uintptr_t)&local,16,0,0,0)==SYSCALL_EEXIST);
    assert(call(SYS_RECVFROM,fd,(uintptr_t)readonly,16,0,0,0)==SYSCALL_EFAULT);
    source_size=16;
    assert(call(SYS_RECVFROM,fd,(uintptr_t)output,16,0,(uintptr_t)output,(uintptr_t)&source_size)==SYSCALL_EINVAL);
    assert(recvfd(fd,16,NET_MSG_DONTWAIT)==SYSCALL_EAGAIN);
    unsigned lengths[]={0,1,9,1472};
    for (unsigned i=0; i<4; ++i) {
        assert(sendfd(fd,lengths[i])==(int64_t)lengths[i]);
        assert(recvfd(fd,1472,0)==(int64_t)lengths[i]);
        assert(!memcmp(buffer,output,lengths[i]) && source.address==destination.address && source.port==destination.port && source_size==16);
    }
    assert(sendfd(fd,9)==9); assert(recvfd(fd,3,0)==3); assert(recvfd(fd,1,NET_MSG_DONTWAIT)==SYSCALL_EAGAIN);
    for (unsigned i=0; i<5; ++i) { uint8_t value=(uint8_t)i; net_socket_input(cfg.gateway,123,7777,&value,1); }
    for (unsigned i=0; i<4; ++i) { assert(recvfd(fd,1,0)==1 && output[0]==i); }
    assert(recvfd(fd,1,NET_MSG_DONTWAIT)==SYSCALL_EAGAIN);
    uint64_t start=now; assert(recvfd(fd,1,0)==SYSCALL_EAGAIN && now-start>=500 && now-start<505);
    signal_pending=true; assert(recvfd(fd,1,0)==SYSCALL_EINTR); signal_pending=false;
    net_socket_wait_t stale, active;
    assert(!net_socket_receive(process.fd_table[fd],false,&stale));
    assert(recvfd(fd,1,0)==SYSCALL_EAGAIN); now+=701; tick();
    assert(!net_socket_live(&stale));
    assert(!net_socket_receive(process.fd_table[fd],false,&active));
    net_socket_finish(&stale,false); assert(net_socket_live(&active)); net_socket_finish(&active,false);
    cached=false; unsigned before=arps;
    assert(sendfd(fd,1)==SYSCALL_EIO && arps-before==3); cached=true;
    tx_fail=true; assert(sendfd(fd,1)==SYSCALL_EIO); tx_fail=false;
    online=false; tick(); assert(recvfd(fd,1,0)==SYSCALL_EIO); online=true; tick();
    /* Shared file reference survives one close; final close releases binding. */
    process.fd_table[31]=process.fd_table[fd]; ++process.fd_table[fd]->ref_count;
    closefd(fd); assert(sendfd(31,1)==1); assert(recvfd(31,1,0)==1); closefd(31);
    assert(!call(SYS_BIND,other,(uintptr_t)&local,16,0,0,0)); closefd(other);
    int fds[16];
    for (unsigned i=0; i<16; ++i) { fds[i]=create(); assert(fds[i]>=0); }
    assert(create()==SYSCALL_ENOSPC);
    /* Sixteen simultaneous cold sends share one ARP attempt per next hop. */
    net_socket_wait_t pending[16]; cached=false; echo=false; before=arps;
    for (unsigned i=0; i<16; ++i)
        assert(!net_socket_send(process.fd_table[fds[i]],&destination,buffer,9,&pending[i]));
    tick(); assert(arps==before+1);
    unsigned sent_before=sends; cached=true; tick();
    assert(sends==sent_before+16);
    for (unsigned i=0; i<16; ++i) {
        assert(net_socket_ready(&pending[i]) && net_socket_result(&pending[i],NULL,NULL)==9);
        net_socket_finish(&pending[i],false);
    }
    /* Close/reuse while old ARP work is pending; stale cancellation cannot
     * touch the replacement operation or submit the old payload. */
    cached=false;
    assert(!net_socket_send(process.fd_table[fds[0]],&destination,buffer,9,&stale)); tick();
    closefd(fds[0]); fds[0]=create(); assert(fds[0]>=0);
    assert(!net_socket_send(process.fd_table[fds[0]],&destination,buffer,3,&active));
    net_socket_finish(&stale,false); assert(net_socket_live(&active));
    cached=true; sent_before=sends; tick(); assert(sends==sent_before+1);
    assert(net_socket_result(&active,NULL,NULL)==3); net_socket_finish(&active,false);
    echo=true;
    /* fd capacity precedence before global socket exhaustion. */
    for (unsigned i=0; i<32; ++i) if (!process.fd_table[i]) process.fd_table[i]=process.fd_table[fds[0]];
    assert(create()==SYSCALL_EMFILE);
    for (unsigned i=0; i<32; ++i) {
        bool original=false; for (unsigned j=0; j<16; ++j) if (fds[j]==(int)i) original=true;
        if (!original) process.fd_table[i]=NULL;
    }
    for (unsigned i=0; i<16; ++i) closefd(fds[i]);
    assert(!allocations && sends && wakes);
    puts("UDP socket/syscall ASan/UBSan PASS: actual codecs/stack/pool/syscalls; mocked memory/fd/tick/scheduler adapters; bounds, payloads, timeout, lease, bind/queue/pool saturation, shared lifetime, failures");
    return 0;
}
