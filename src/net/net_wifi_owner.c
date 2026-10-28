#include "net_wifi_owner.h"
#include "net_control.h"
#include "wlan/wlan_mgr.h"
#include "iwlwifi.h"
#include "thread.h"
#include "vmm.h"
#include "heap.h"
#include "spinlock.h"
#include "string.h"
#include "syscall_abi.h"

#define WIFI_RECORDS 4u
#define WIFI_FW_MAX (4u * 1024u * 1024u)
typedef union {
    wifi_scan_result_t scan;
    wifi_connect_req_t connect;
    wifi_status_t status;
} wifi_value_t;
typedef struct {
    bool used;
    handoff_token_t token;
    wifi_value_t value;
    void *firmware;
    size_t firmware_size;
} wifi_record_t;
static wifi_record_t records[WIFI_RECORDS];
static bool firmware_busy;
static spinlock_t lock=SPINLOCK_RANKED(1,"wifi_handoff_values");
_Static_assert(sizeof(records)<=4096,"Wi-Fi copied state budget");

static void wipe(void *ptr,size_t size) {
    volatile uint8_t *p=ptr;
    while (size--) *p++=0;
}
static bool wifi_kind(unsigned kind) {
    return kind>=NET_OWNER_WIFI && kind<=NET_OWNER_WIFI_LAST;
}
/* Only called by the serialized owner. PREPARED storage belongs exclusively
 * to the caller; COMPLETED storage cannot recycle until cleanup acknowledges. */
void net_wifi_maintenance(void) {
    net_control_assert_owner();
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        handoff_token_t t; handoff_request_t r; handoff_state_t st; bool caller;
        if (!handoff_inspect(&t,&r,&st,&caller) || !wifi_kind(r.kind) ||
            st!=HANDOFF_COMPLETED || caller) continue;
        for (unsigned n=0;n<WIFI_RECORDS;++n) {
            wifi_record_t *v=&records[n];
            uint64_t f=spin_lock_irqsave(&lock);
            bool match=v->used && v->token.slot==t.slot && v->token.generation==t.generation;
            spin_unlock_irqrestore(&lock,f);
            if (!match) continue;
            if (v->firmware) { wipe(v->firmware,v->firmware_size); kfree(v->firmware); }
            f=spin_lock_irqsave(&lock);
            if (v->firmware) firmware_busy=false;
            wipe(v,sizeof(*v));
            spin_unlock_irqrestore(&lock,f);
            break;
        }
        handoff_cleaned(t);
    }
}

static int64_t abandon(handoff_registration_t *r,int64_t status) {
    handoff_abandon(r);
    return status;
}
static bool range(uintptr_t arg,size_t size,bool write) {
    return vmm_validate_user_range(vmm_get_active_pml4_virt(),arg,size,write);
}

int64_t net_wifi_syscall(uint64_t cmd,uintptr_t arg,size_t size) {
    /* Preserve the existing public SYS_WIFI access policy: no capability gate.
     * Desktop privilege/display policy is not kernel authorization. */
    bool output=cmd==SYS_WIFI_CMD_SCAN || cmd==SYS_WIFI_CMD_STATUS;
    size_t expected=cmd==SYS_WIFI_CMD_SCAN ? sizeof(wifi_scan_result_t) :
        cmd==SYS_WIFI_CMD_STATUS ? sizeof(wifi_status_t) : sizeof(wifi_connect_req_t);
    if (cmd<SYS_WIFI_CMD_SCAN || cmd>SYS_WIFI_CMD_LOAD_FW) return SYSCALL_EINVAL;
    if (cmd==SYS_WIFI_CMD_LOAD_FW) {
        if (!size || size>WIFI_FW_MAX) return SYSCALL_EINVAL;
    } else if (cmd!=SYS_WIFI_CMD_DISCONNECT && size!=expected) return SYSCALL_EINVAL;
    if (cmd!=SYS_WIFI_CMD_DISCONNECT && !range(arg,size,output)) return SYSCALL_EFAULT;
    tcb_t *self=thread_current();
    if (!self) return SYSCALL_EIO;
    handoff_registration_t *r=&self->handoff;
    int rc;
    for (;;) {
        if (process_signal_interrupt()) return SYSCALL_EINTR;
        uint64_t sequence=handoff_admission_snapshot();
        rc=handoff_begin(r,(handoff_request_t){.actor=self->tid,
            .kind=NET_OWNER_WIFI+(unsigned)cmd-1,.control=true,.retained=true,
            .cancel_status=SYSCALL_EINTR});
        if (rc!=HANDOFF_WAIT) break;
        sched_wait_until(&handoff_admission_channel,handoff_admissible,&sequence);
    }
    if (rc) return SYSCALL_EIO;
    /* No waits/signals between record reservation and publication. Revalidate
     * user memory after admission sleeps; the worker never accesses it. */
    if (cmd!=SYS_WIFI_CMD_DISCONNECT && !range(arg,size,output))
        return abandon(r,SYSCALL_EFAULT);
    wifi_record_t *v=NULL;
    uint64_t f=spin_lock_irqsave(&lock);
    if (cmd!=SYS_WIFI_CMD_LOAD_FW || !firmware_busy) {
        for (unsigned i=0;i<WIFI_RECORDS;++i) if (!records[i].used) {
            v=&records[i];
            memset(v,0,sizeof(*v)); v->used=true; v->token=r->token;
            if (cmd==SYS_WIFI_CMD_LOAD_FW) firmware_busy=true;
            break;
        }
    }
    spin_unlock_irqrestore(&lock,f);
    if (!v) return abandon(r,SYSCALL_EAGAIN);
    if (cmd==SYS_WIFI_CMD_SCAN) {
        memcpy(&v->value.scan.max_entries,(const void *)(arg+offsetof(wifi_scan_result_t,max_entries)),4);
        if (v->value.scan.max_entries>WIFI_MAX_SCAN_ENTRIES) v->value.scan.max_entries=WIFI_MAX_SCAN_ENTRIES;
    } else if (cmd==SYS_WIFI_CMD_CONNECT) {
        memcpy(&v->value.connect,(const void *)arg,sizeof(wifi_connect_req_t));
        v->value.connect.ssid[WIFI_SSID_MAX_LEN]=0;
        v->value.connect.passphrase[WIFI_PASSPHRASE_MAX_LEN]=0;
    } else if (cmd==SYS_WIFI_CMD_LOAD_FW) {
        v->firmware=kmalloc(size);
        if (!v->firmware) {
            f=spin_lock_irqsave(&lock); firmware_busy=false; spin_unlock_irqrestore(&lock,f);
            return abandon(r,SYSCALL_ENOMEM);
        }
        v->firmware_size=size;
        memcpy(v->firmware,(const void *)arg,size);
    }
    handoff_token_t t=r->token;
    for (;;) {
        if (process_signal_interrupt()) return abandon(r,SYSCALL_EINTR);
        uint64_t sequence=handoff_admission_snapshot();
        rc=handoff_publish(t);
        if (rc!=HANDOFF_WAIT) break;
        sched_wait_until(&handoff_admission_channel,handoff_admissible,&sequence);
    }
    if (rc) return abandon(r,SYSCALL_EIO);
    while (!handoff_ready(&t)) {
        if (process_signal_interrupt()) return abandon(r,SYSCALL_EINTR);
        sched_wait_until(handoff_channel(t),handoff_ready,&t);
    }
    if (process_signal_interrupt()) return abandon(r,SYSCALL_EINTR);
    handoff_reply_t reply;
    rc=handoff_collect(t,&reply);
    int64_t result=rc ? SYSCALL_EIO : reply.status;
    if (!result && output) {
        if (!range(arg,size,true)) result=SYSCALL_EFAULT;
        else memcpy((void *)arg,&v->value,size);
    }
    handoff_release(r);
    return result;
}

void net_wifi_step(handoff_token_t t,handoff_request_t r) {
    net_control_assert_owner();
    wifi_record_t *v=NULL;
    for (unsigned i=0;i<WIFI_RECORDS;++i) {
        uint64_t f=spin_lock_irqsave(&lock);
        bool match=records[i].used && records[i].token.slot==t.slot && records[i].token.generation==t.generation;
        spin_unlock_irqrestore(&lock,f);
        if (match) { v=&records[i]; break; }
    }
    int rc=-1;
    int64_t result=SYSCALL_EIO;
    if (handoff_cancelled(t)) result=SYSCALL_EINTR;
    else if (v) {
        switch (r.kind-NET_OWNER_WIFI+1) {
        case SYS_WIFI_CMD_SCAN: rc=wlan_mgr_scan(&v->value.scan); break;
        case SYS_WIFI_CMD_STATUS: rc=wlan_mgr_status(&v->value.status); break;
        case SYS_WIFI_CMD_CONNECT:
            rc=wlan_mgr_connect(v->value.connect.ssid,v->value.connect.passphrase,v->value.connect.bssid);
            if (rc==-2) rc=0;
            wipe(&v->value.connect,sizeof(v->value.connect));
            break;
        case SYS_WIFI_CMD_DISCONNECT: rc=wlan_mgr_disconnect(); break;
        case SYS_WIFI_CMD_LOAD_FW: rc=iwlwifi_load_firmware(v->firmware,v->firmware_size) ? 0 : -1; break;
        }
        result=rc ? SYSCALL_EIO : 0;
    }
    /* Cancellation before claim prevents effects. Once claimed the bounded
     * action may finish; death never runs driver teardown on the caller CPU. */
    handoff_finish(t,(handoff_reply_t){.status=result},false);
}
