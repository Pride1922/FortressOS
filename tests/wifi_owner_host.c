/* Actual SYS_WIFI adapter, generic handoff, registered service, WLAN manager,
 * MAC and crypto. Only CPU/scheduler/VM/heap/radio adapters are mocked. */
#define main baseline_main
#define thread_current baseline_thread_current
#define sched_wait_until baseline_sched_wait_until
#define sched_wake_all baseline_sched_wake_all
#define process_signal_interrupt baseline_signal_interrupt
#include "net_ifconfig_host.c"
#undef main
#undef thread_current
#undef sched_wait_until
#undef sched_wake_all
#undef process_signal_interrupt
#include "net_wifi_owner.h"
#include "net_control.h"
#include "wlan/wlan_mgr.h"
#include "iwlwifi.h"
#include "vmm.h"
#include <stdlib.h>
#include <sched.h>
#include <time.h>

static _Thread_local tcb_t task;
static _Thread_local unsigned cpu, depth, ranks[8];
static _Thread_local bool interrupted_call, cancel_wait, invalidate_output;
static pthread_mutex_t event=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned running, stop, parked=1, started, done, successes, busy_results;
static unsigned radio_calls, firmware_calls, allocations;
static bool owner_open, fail_allocation, hold_firmware, firmware_entered;
static unsigned char fw[128];
static void owner_only(void) { assert(cpu==0 && task.tid==9000); }
void net_test_assert_unheld(void) { assert(!depth); }
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(depth<8 && (!depth || ranks[depth-1]<l->rank));
    assert(!pthread_mutex_lock(&l->mutex)); ranks[depth++]=l->rank; return 0;
}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {
    (void)f; assert(depth && ranks[--depth]==l->rank); assert(!pthread_mutex_unlock(&l->mutex));
}
tcb_t *thread_current(void) { return &task; }
bool process_signal_interrupt(void) { return interrupted_call; }
void sched_wake_all(const void *channel) {
    (void)channel; assert(!depth);
    pthread_mutex_lock(&event); pthread_cond_broadcast(&changed); pthread_mutex_unlock(&event);
}
void sched_wait_until(const void *channel,bool (*ready)(void *),void *arg) {
    (void)channel; assert(!depth);
    if (cancel_wait) { interrupted_call=true; return; }
    pthread_mutex_lock(&event);
    while (!ready(arg)) pthread_cond_wait(&changed,&event);
    pthread_mutex_unlock(&event);
}
uint64_t *vmm_get_active_pml4_virt(void) { return NULL; }
bool vmm_validate_user_range(uint64_t *root,uintptr_t p,size_t n,bool write) {
    (void)root;
    if (invalidate_output && write && ++ranks[7]>=3) return false;
    return p>=4096 && n && p<=UINTPTR_MAX-n;
}
void *kmalloc(size_t n) {
    assert(!depth); if (fail_allocation) return NULL;
    __atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED); return malloc(n);
}
void kfree(void *p) {
    owner_only(); assert(!depth);
    for (unsigned i=0;i<sizeof(fw);++i) assert(!((unsigned char *)p)[i]);
    __atomic_sub_fetch(&allocations,1,__ATOMIC_RELAXED); free(p);
}
bool iwlwifi_is_detected(void) { owner_only(); return false; }
bool iwlwifi_init_hw(void) { owner_only(); return false; }
bool iwlwifi_load_firmware(const void *p,size_t n) {
    owner_only(); assert(!depth && p!=fw && n==sizeof(fw) && !memcmp(p,fw,n));
    __atomic_add_fetch(&firmware_calls,1,__ATOMIC_RELAXED);
    pthread_mutex_lock(&event); firmware_entered=true; pthread_cond_broadcast(&changed);
    while (hold_firmware) pthread_cond_wait(&changed,&event);
    pthread_mutex_unlock(&event); return true;
}
static int mac(uint8_t p[6]) { owner_only(); memset(p,0x24,6); ++radio_calls; return 0; }
static int send_frame(const void *p,size_t n) { owner_only(); assert(p && n); ++radio_calls; return 0; }
static int scan(const char *ssid) { owner_only(); assert(ssid); ++radio_calls; return 0; }
static int deauth(void) { owner_only(); ++radio_calls; return 0; }
static pbuf_t *poll_frame(void) { owner_only(); ++radio_calls; return NULL; }
static bool scan_done(void) { owner_only(); ++radio_calls; return true; }
static const wlan_hw_ops_t ops={.get_mac=mac,.send_raw_80211=send_frame,
    .start_scan=scan,.deauth=deauth,.poll_raw_80211=poll_frame,.is_scan_complete=scan_done};

static void *worker(void *unused) {
    (void)unused; task.tid=9000; cpu=0; net_control_register_owner(); wlan_mgr_init(&ops);
    __atomic_store_n(&running,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&event);
        while (!owner_open) { __atomic_store_n(&parked,1,__ATOMIC_RELEASE); pthread_cond_wait(&changed,&event); }
        __atomic_store_n(&parked,0,__ATOMIC_RELEASE); pthread_mutex_unlock(&event);
        net_control_service(); sched_yield();
    }
    return NULL;
}
static void gate(bool open) {
    pthread_mutex_lock(&event); owner_open=open; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&event);
    if (!open) while (!__atomic_load_n(&parked,__ATOMIC_ACQUIRE)) sched_yield();
}
static void clean(void) { while (handoff_stats().live) sched_yield(); assert(!allocations); }
static int64_t call(unsigned command,void *p,size_t n) { return net_wifi_syscall(command,(uintptr_t)p,n); }
static void *producer(void *arg) {
    task.tid=(uintptr_t)arg+1; cpu=task.tid%4;
    __atomic_add_fetch(&started,1,__ATOMIC_RELEASE);
    for (unsigned n=0;n<100;++n) {
        wifi_status_t s; int64_t rc=call(SYS_WIFI_CMD_STATUS,&s,sizeof(s));
        assert(!rc || rc==SYSCALL_EAGAIN);
        if (!rc) { assert(!s.reserved); __atomic_add_fetch(&successes,1,__ATOMIC_RELAXED); }
        else __atomic_add_fetch(&busy_results,1,__ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&done,1,__ATOMIC_RELEASE); return NULL;
}
static void *upload(void *arg) {
    (void)arg; task.tid=7000; cpu=2;
    assert(!call(SYS_WIFI_CMD_LOAD_FW,fw,sizeof(fw))); return NULL;
}
int main(void) {
    memset(fw,0xa5,sizeof(fw)); task.tid=8000; cpu=3;
    assert(call(0,NULL,0)==SYSCALL_EINVAL);
    assert(call(SYS_WIFI_CMD_STATUS,NULL,1)==SYSCALL_EINVAL);
    assert(call(SYS_WIFI_CMD_STATUS,NULL,sizeof(wifi_status_t))==SYSCALL_EFAULT);
    assert(call(SYS_WIFI_CMD_LOAD_FW,fw,4u*1024*1024+1)==SYSCALL_EINVAL);
    assert(call(SYS_WIFI_CMD_STATUS,fw,sizeof(wifi_status_t))==SYSCALL_EIO); /* Owner unavailable. */
    net_control_start(); pthread_t owner; assert(!pthread_create(&owner,NULL,worker,NULL));
    while (!__atomic_load_n(&running,__ATOMIC_ACQUIRE)) sched_yield();
    pthread_t callers[48];
    for (unsigned i=0;i<48;++i) assert(!pthread_create(&callers[i],NULL,producer,(void *)(uintptr_t)i));
    while (__atomic_load_n(&started,__ATOMIC_ACQUIRE)!=48) sched_yield();
    /* Four records retained, all further callers fail boundedly until owner runs. */
    while (handoff_stats().queued!=4 || !__atomic_load_n(&busy_results,__ATOMIC_ACQUIRE)) sched_yield();
    gate(true);
    for (unsigned i=0;i<48;++i) assert(!pthread_join(callers[i],NULL));
    clean(); assert(done==48 && successes && busy_results);
    wifi_scan_result_t result={0}; assert(!call(SYS_WIFI_CMD_SCAN,&result,sizeof(result))); clean();
    result.max_entries=UINT32_MAX;
    assert(!call(SYS_WIFI_CMD_SCAN,&result,sizeof(result)) && result.max_entries==16 && !result.count); clean();
    wifi_connect_req_t req={.ssid="test",.passphrase="password",.bssid={1,2,3,4,5,6}};
    assert(!call(SYS_WIFI_CMD_CONNECT,&req,sizeof(req))); clean();
    wifi_status_t status;
    unsigned before=radio_calls;
    assert(!call(SYS_WIFI_CMD_STATUS,&status,sizeof(status))); clean();
    assert(status.state==WIFI_STATUS_AUTHENTICATING && radio_calls==before); /* Snapshot has zero hardware calls. */
    assert(!call(SYS_WIFI_CMD_DISCONNECT,NULL,0)); clean();
    fail_allocation=true; assert(call(SYS_WIFI_CMD_LOAD_FW,fw,sizeof(fw))==SYSCALL_ENOMEM); clean(); fail_allocation=false;
    assert(!call(SYS_WIFI_CMD_LOAD_FW,fw,sizeof(fw))); clean();
    gate(false); cancel_wait=true;
    assert(call(SYS_WIFI_CMD_CONNECT,&req,sizeof(req))==SYSCALL_EINTR);
    cancel_wait=false; interrupted_call=false;
    before=radio_calls; gate(true); clean(); assert(radio_calls==before);
    /* Real thread-death cleanup entry uses exactly this generic abandon API. */
    gate(false); handoff_registration_t dead={0};
    assert(!handoff_begin(&dead,(handoff_request_t){.actor=6000,.kind=NET_OWNER_WIFI,
        .control=true,.retained=true,.cancel_status=SYSCALL_EINTR}));
    assert(!handoff_publish(dead.token)); handoff_abandon(&dead); gate(true); clean();
    invalidate_output=true; ranks[7]=0;
    assert(call(SYS_WIFI_CMD_STATUS,&status,sizeof(status))==SYSCALL_EFAULT); invalidate_output=false; clean();
    pthread_mutex_lock(&event); hold_firmware=true; firmware_entered=false; pthread_mutex_unlock(&event);
    pthread_t uploader; assert(!pthread_create(&uploader,NULL,upload,NULL));
    pthread_mutex_lock(&event); while (!firmware_entered) pthread_cond_wait(&changed,&event); pthread_mutex_unlock(&event);
    assert(call(SYS_WIFI_CMD_LOAD_FW,fw,sizeof(fw))==SYSCALL_EAGAIN);
    pthread_mutex_lock(&event); hold_firmware=false; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&event);
    assert(!pthread_join(uploader,NULL)); clean();
    assert(firmware_calls==2);
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE); assert(!pthread_join(owner,NULL));
    printf("Wi-Fi owner actual-code PASS: 4800 concurrent calls, bounded records, BSP/AP adapters, all radio hooks CPU0/registered owner, snapshot, scan/connect/disconnect/firmware, cancellation/death, output revalidation, allocation failure and firmware exclusion; reclaimed=%llu\n",(unsigned long long)handoff_stats().reclaimed);
}
