/* Copied ONLY into a disposable guest snapshot by test_wifi_owner.py.
 * Mock radio hooks enforce the real registered-owner assertion on every call.
 * This is ownership/scheduling evidence, never Intel radio acceptance. */
#include "net_wifi_owner.h"
#include "net_control.h"
#include "wlan/wlan_mgr.h"
#include "percpu.h"
#include "string.h"
#include "syscall_abi.h"

static uint64_t entries, kinds, radios, calls, held;
static bool beacon_pending, scanning;
static pbuf_t beacon;
static const uint8_t bssid[6]={2,3,4,5,6,7};
static void radio(void) {
    net_control_assert_owner();
    __atomic_fetch_or(&radios,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);
    __atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
}
static int mac(uint8_t out[6]) { radio(); memset(out,0x24,6); return 0; }
static int scan(const char *ssid) { radio(); (void)ssid; scanning=true; beacon_pending=true; return 0; }
static int send_frame(const void *p,size_t n) { radio(); return p && n ? 0 : -1; }
static int prepare(uint8_t channel,const uint8_t ap[6],uint16_t interval) {
    radio(); (void)channel; (void)ap; (void)interval; return 0;
}
static int deauth(void) { radio(); return 0; }
static pbuf_t *poll_frame(void) {
    radio(); if (!beacon_pending) return NULL;
    beacon_pending=false; scanning=false; return &beacon;
}
static void recycle(pbuf_t *p) { radio(); (void)p; }
static bool complete(void) { radio(); return !scanning; }
static const wlan_hw_ops_t ops={.get_mac=mac,.start_scan=scan,.send_raw_80211=send_frame,
    .prepare_auth=prepare,.deauth=deauth,.poll_raw_80211=poll_frame,
    .recycle_raw_80211=recycle,.is_scan_complete=complete};
void wifi_fixture_init(void) {
    net_control_assert_owner();
    wlan_mgr_init(&ops);
    pbuf_init(&beacon);
    memset(beacon.data,0,sizeof(beacon.data));
    ieee80211_hdr_t *hdr=(ieee80211_hdr_t *)beacon.data;
    hdr->frame_control=IEEE80211_SUBTYPE_BEACON<<4;
    memcpy(hdr->addr2,bssid,6); memcpy(hdr->addr3,bssid,6); memset(hdr->addr1,0xff,6);
    uint8_t *p=beacon.data+sizeof(*hdr)+12;
    beacon.data[sizeof(*hdr)+8]=100;
    *p++=0; *p++=7; memcpy(p,"Fixture",7); p+=7;
    *p++=3; *p++=1; *p++=1;
    beacon.length=(uint16_t)(p-beacon.data);
}
void wifi_fixture_entry(void) { __atomic_fetch_or(&entries,1ULL<<cpu_current()->id,__ATOMIC_RELAXED); }
bool wifi_fixture_gate(unsigned kind) {
    net_control_assert_owner();
    __atomic_fetch_or(&kinds,1ULL<<(kind-NET_OWNER_WIFI),__ATOMIC_RELAXED);
    return __atomic_load_n(&held,__ATOMIC_ACQUIRE)!=0;
}
bool wifi_fixture_real_load(const void *,size_t);
bool iwlwifi_load_firmware(const void *p,size_t n) { radio(); return wifi_fixture_real_load(p,n); }
int64_t wifi_fixture_observe(unsigned op) {
    switch (op) {
    case 0: return cpu_current()->id;
    case 1: __atomic_store_n(&held,1,__ATOMIC_RELEASE); return 0;
    case 2: __atomic_store_n(&held,0,__ATOMIC_RELEASE); handoff_notify(); return 0;
    case 3: return handoff_stats().pending;
    case 4: return handoff_stats().live;
    case 5: return __atomic_load_n(&kinds,__ATOMIC_ACQUIRE);
    case 6: return __atomic_load_n(&entries,__ATOMIC_ACQUIRE);
    case 7: return __atomic_load_n(&radios,__ATOMIC_ACQUIRE);
    case 8: return __atomic_load_n(&calls,__ATOMIC_ACQUIRE);
    default: return SYSCALL_EINVAL;
    }
}
