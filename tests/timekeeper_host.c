#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "../src/kernel/time.c"

static uint64_t mock_cycles=1000000000ULL,mock_ticks=100;
uint64_t apic_poll_clock_read(void) {return mock_cycles;}
uint64_t apic_timer_get_bsp_ticks(void) {return mock_ticks;}
static void *writer(void *unused) {
    (void)unused;
    for(int i=0;i<100000;i++) {
        timespec_t t={i&1?2000000000:1000000000,i&1?999999999:0};
        assert(time_set_realtime(&t)==0);
    }
    return NULL;
}
int main(void) {
    g_timekeeper=(kernel_timekeeper_t){.initialized=true,.tsc_valid=true,.tsc_hz=1000000000,
        .base_tsc=1000000000,.base_epoch_sec=1790000000,.boot_tsc=0,.base_bsp_ticks=100};
    timespec_t m,b,a,invalid={0,1000000000};
    assert(time_get_timespec(CLOCK_MONOTONIC,&m)==0&&m.tv_sec==1);
    assert(time_get_timespec(99,&a)==SYSCALL_EINVAL);
    assert(time_get_timespec(CLOCK_REALTIME,&b)==0);
    assert(time_set_realtime(&invalid)==SYSCALL_EINVAL);
    assert(time_get_timespec(CLOCK_REALTIME,&a)==0&&a.tv_sec==b.tv_sec&&a.tv_nsec==b.tv_nsec);
    timespec_t target={100,123456789};
    assert(time_set_realtime(&target)==0);
    assert(time_get_timespec(CLOCK_REALTIME,&a)==0&&a.tv_sec==100&&a.tv_nsec==123456789);
    assert(time_get_timespec(CLOCK_MONOTONIC,&a)==0&&a.tv_sec==m.tv_sec&&a.tv_nsec==m.tv_nsec);
    mock_cycles+=1500000000;
    assert(time_get_timespec(CLOCK_REALTIME,&a)==0&&a.tv_sec==101&&a.tv_nsec==623456789);
    target=(timespec_t){4133980800LL,0};assert(time_set_realtime(&target)==SYSCALL_EINVAL);
    target=(timespec_t){-1,0};assert(time_set_realtime(&target)==SYSCALL_EINVAL);
    pthread_t worker;mock_cycles=1000000000;
    target=(timespec_t){1000000000,0};assert(time_set_realtime(&target)==0);
    assert(!pthread_create(&worker,NULL,writer,NULL));
    for(int i=0;i<100000;i++) {
        assert(time_get_timespec(CLOCK_REALTIME,&a)==0);
        assert((a.tv_sec==1000000000&&a.tv_nsec==0)||(a.tv_sec==2000000000&&a.tv_nsec==999999999));
    }
    assert(!pthread_join(worker,NULL));
    g_timekeeper.tsc_valid=false;target=(timespec_t){200,900000000};
    assert(time_set_realtime(&target)==0);mock_ticks+=11;
    assert(time_get_timespec(CLOCK_REALTIME,&a)==0&&a.tv_sec==201&&a.tv_nsec==10000000);
    puts("PASS actual kernel atomic clock corrections, failure preservation, SMP coherence, monotonic isolation and tick fallback");
}
