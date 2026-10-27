#define IWLWIFI_HOST_TEST 1

/*
 * Fault-injection host tests for the Wi-Fi manager and the iwlwifi health state machine.
 * A scripted mock radio can: answer Auth/Assoc immediately (responsive AP), stay silent
 * (frame loss), or refuse prepare_auth (firmware DEAD). Asserts fail-closed behaviour and
 * that prepare_auth/deauth stay balanced across a connect/disconnect soak.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "wifi_abi.h"
#include "netctl_abi.h"
#include "net_80211.h"
#include "wlan_wpa2.h"
#include "wlan_mgr.h"
#include "iwlwifi.h"
#include "iwl_health.h"

void serial_puts(const char *s) {
    assert(strncmp(s, "[WLAN] ", 7) == 0);
}

uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!pthread_mutex_lock(&l->mutex));
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags;
    assert(!pthread_mutex_unlock(&l->mutex));
}

/* wlan_mgr.c only needs these to lazily bring up real hardware; the mock is pre-registered */
bool iwlwifi_is_detected(void) { return false; }
bool iwlwifi_init_hw(void) { return false; }

static uint8_t s_sta_mac[6] = { 0x00, 0x24, 0xFD, 0x82, 0x65, 0x01 };
static uint8_t s_ap_bssid[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };

typedef enum { AP_SILENT, AP_RESPONSIVE } ap_mode_t;
static ap_mode_t s_ap_mode = AP_SILENT;
static int  s_prepare_rc = 0;
static uint32_t s_prepare_ok = 0, s_prepare_fail = 0, s_deauth = 0, s_tx = 0;

#define Q_MAX 8
static uint8_t s_q_data[Q_MAX][64];
static size_t  s_q_len[Q_MAX];
static unsigned s_q_head = 0, s_q_tail = 0;
static pbuf_t  s_pbuf;
static uint8_t s_pbuf_data[64];

static void q_push(const uint8_t *d, size_t n) {
    assert(((s_q_tail + 1) % Q_MAX) != s_q_head);
    memcpy(s_q_data[s_q_tail], d, n);
    s_q_len[s_q_tail] = n;
    s_q_tail = (s_q_tail + 1) % Q_MAX;
}

static void build_resp(uint8_t *f, uint8_t subtype, uint8_t b24, uint8_t b26, uint8_t b28, uint8_t b29) {
    memset(f, 0, 30);
    ieee80211_hdr_t *h = (ieee80211_hdr_t *)f;
    h->frame_control = IEEE80211_TYPE_MGMT | (subtype << 4);
    memcpy(h->addr1, s_sta_mac, 6);
    memcpy(h->addr2, s_ap_bssid, 6);
    memcpy(h->addr3, s_ap_bssid, 6);
    f[24] = b24; f[26] = b26; f[28] = b28; f[29] = b29;
}

static int m_get_mac(uint8_t mac[6]) { memcpy(mac, s_sta_mac, 6); return 0; }
static int m_set_channel(uint8_t c) { (void)c; return 0; }
static int m_prepare(uint8_t c, const uint8_t b[6], uint16_t bi) {
    (void)c; (void)b; (void)bi;
    if (s_prepare_rc < 0) { s_prepare_fail++; return s_prepare_rc; }
    s_prepare_ok++;
    return 0;
}
static int m_deauth(void) { s_deauth++; return 0; }
static int m_send(const void *buf, size_t len) {
    s_tx++;
    if (len < 24 || s_ap_mode != AP_RESPONSIVE) return 0;
    const ieee80211_hdr_t *h = (const ieee80211_hdr_t *)buf;
    uint8_t sub = (h->frame_control >> 4) & 0xF;
    uint8_t resp[30];
    if (sub == IEEE80211_SUBTYPE_AUTH) {
        build_resp(resp, IEEE80211_SUBTYPE_AUTH, 0, 2, 0, 0);
        q_push(resp, sizeof(resp));
    } else if (sub == IEEE80211_SUBTYPE_ASSOC_REQ) {
        build_resp(resp, IEEE80211_SUBTYPE_ASSOC_RESP, 0x11, 0, 0x01, 0xC0);
        resp[25] = 0x04;
        q_push(resp, sizeof(resp));
    }
    return 0;
}
static pbuf_t *m_poll(void) {
    if (s_q_head == s_q_tail) return NULL;
    memset(&s_pbuf, 0, sizeof(s_pbuf));
    memcpy(s_pbuf_data, s_q_data[s_q_head], s_q_len[s_q_head]);
    s_pbuf.payload = s_pbuf_data;
    s_pbuf.length = (uint32_t)s_q_len[s_q_head];
    s_q_head = (s_q_head + 1) % Q_MAX;
    return &s_pbuf;
}
static void m_recycle(pbuf_t *p) { (void)p; }

static const wlan_hw_ops_t s_ops = {
    .name = "fault_mock",
    .get_mac = m_get_mac,
    .set_channel = m_set_channel,
    .prepare_auth = m_prepare,
    .deauth = m_deauth,
    .send_raw_80211 = m_send,
    .poll_raw_80211 = m_poll,
    .recycle_raw_80211 = m_recycle,
};

static size_t build_beacon(uint8_t *buf, const char *ssid, uint8_t channel) {
    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    hdr->frame_control = IEEE80211_TYPE_MGMT | (IEEE80211_SUBTYPE_BEACON << 4);
    hdr->duration_id = 0;
    memset(hdr->addr1, 0xFF, 6);
    memcpy(hdr->addr2, s_ap_bssid, 6);
    memcpy(hdr->addr3, s_ap_bssid, 6);
    hdr->seq_ctrl = 0;
    uint8_t *p = buf + sizeof(ieee80211_hdr_t);
    memset(p, 0, 8); p += 8;
    *p++ = 0x64; *p++ = 0x00; *p++ = 0x11; *p++ = 0x04;
    size_t sl = strlen(ssid);
    *p++ = IEEE80211_IE_SSID; *p++ = (uint8_t)sl; memcpy(p, ssid, sl); p += sl;
    *p++ = IEEE80211_IE_DS_PARAM; *p++ = 1; *p++ = channel;
    *p++ = IEEE80211_IE_RSN; *p++ = 20;
    *p++ = 0x01; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04;
    *p++ = 0x01; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04;
    *p++ = 0x01; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x02;
    *p++ = 0x00; *p++ = 0x00;
    return (size_t)(p - buf);
}

static void test_health(void) {
    iwl_health_t h;
    bool reset = false;

    /* Fault: single firmware error is recoverable and never blocks commands */
    iwl_health_init(&h);
    assert(h.state == IWL_HEALTH_OK && iwl_health_cmd_allowed(&h));
    iwl_health_on_fw_error(&h, 0x2000000u);
    assert(h.state == IWL_HEALTH_ERROR);
    assert(iwl_health_cmd_allowed(&h));            /* no sticky lockout */
    assert(iwl_health_begin_recovery(&h, &reset) && reset);
    assert(h.state == IWL_HEALTH_OK && h.generation == 1 && h.recoveries == 1);
    assert(iwl_health_begin_recovery(&h, &reset) && !reset);   /* idempotent when healthy */

    /* A successful command after an error also proves liveness */
    iwl_health_on_fw_error(&h, 0x2000000u);
    iwl_health_on_cmd_result(&h, IWL_CMD_OK);
    assert(h.state == IWL_HEALTH_OK && h.consec_errors == 0 && h.recoveries == 2);

    /* Fault: rejections are not liveness failures */
    for (int i = 0; i < 20; i++) iwl_health_on_cmd_result(&h, IWL_CMD_REJECTED);
    assert(h.state == IWL_HEALTH_OK && h.cmd_rejects == 20);

    /* Fault: consecutive timeouts escalate to DEAD, a response in between resets the streak */
    for (unsigned i = 0; i < IWL_HEALTH_MAX_CONSEC_TIMEOUTS - 1; i++) iwl_health_on_cmd_result(&h, IWL_CMD_TIMEOUT);
    iwl_health_on_cmd_result(&h, IWL_CMD_REJECTED);
    for (unsigned i = 0; i < IWL_HEALTH_MAX_CONSEC_TIMEOUTS - 1; i++) iwl_health_on_cmd_result(&h, IWL_CMD_TIMEOUT);
    assert(h.state == IWL_HEALTH_OK);
    iwl_health_on_cmd_result(&h, IWL_CMD_TIMEOUT);
    assert(h.state == IWL_HEALTH_DEAD);
    assert(!iwl_health_cmd_allowed(&h) && h.cmd_fast_failed == 1);
    assert(!iwl_health_begin_recovery(&h, &reset) && !reset);

    /* Fault: repeated errors with no successful command in between -> DEAD */
    iwl_health_init(&h);
    for (unsigned i = 0; i < IWL_HEALTH_MAX_CONSEC_ERRORS; i++) {
        iwl_health_on_fw_error(&h, 0x2000000u);
        if (i + 1 < IWL_HEALTH_MAX_CONSEC_ERRORS) assert(iwl_health_begin_recovery(&h, &reset));
    }
    assert(h.state == IWL_HEALTH_DEAD);
    printf("  [PASS] iwl_health: error recovery, no sticky lockout, DEAD escalation\n");
}

static void test_manager(void) {
    wifi_status_t st;
    uint8_t beacon[256];
    size_t bl = build_beacon(beacon, "FaultNet", 1);

    wlan_mgr_init(&s_ops);
    net_dev_t *dev = wlan_get_net_device();
    assert(dev);
    assert(wlan_mgr_rx_raw_80211(beacon, bl, -45) == 0);

    /* Responsive AP: Auth+Assoc are serviced synchronously inside connect (RX pump) */
    s_ap_mode = AP_RESPONSIVE;
    s_prepare_rc = 0;
    assert(wlan_mgr_connect("FaultNet", "SuperSecretKey2026", s_ap_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_HANDSHAKE);
    assert(wlan_mgr_disconnect() == 0);
    printf("  [PASS] RX pump advances Auth/Assoc synchronously within wlan_mgr_connect\n");

    /* Frame loss: silent AP -> retransmit then fail closed with hardware teardown */
    s_ap_mode = AP_SILENT;
    uint32_t deauth_before = s_deauth;
    assert(wlan_mgr_connect("FaultNet", "SuperSecretKey2026", s_ap_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0 && st.state == WIFI_STATUS_AUTHENTICATING);
    wlan_service_link(dev, 1000, 1000);
    wlan_service_link(dev, 7100, 1000);
    assert(wlan_mgr_status(&st) == 0 && st.state == WIFI_STATUS_FAILED);
    assert(s_deauth == deauth_before + 1);
    printf("  [PASS] Silent AP: fail-closed timeout with hardware deauth\n");
    assert(wlan_mgr_disconnect() == 0);

    /* Firmware DEAD: prepare_auth refusal fails closed immediately */
    s_prepare_rc = -1;
    uint32_t tx_before = s_tx;
    assert(wlan_mgr_connect("FaultNet", "SuperSecretKey2026", s_ap_bssid) == -3);
    assert(wlan_mgr_status(&st) == 0 && st.state == WIFI_STATUS_FAILED);
    assert(s_tx == tx_before);          /* no Auth frame sent into a dead radio */
    assert(!wlan_network_online(dev));
    printf("  [PASS] prepare_auth refusal (firmware DEAD) fails closed without TX\n");
    assert(wlan_mgr_disconnect() == 0);
    s_prepare_rc = 0;

    /* Soak: 300 mixed cycles; hardware setup/teardown must stay balanced */
    uint32_t ok0 = s_prepare_ok, de0 = s_deauth;
    uint32_t fail_cycles = 0;
    for (int i = 0; i < 300; i++) {
        s_ap_mode = (i % 3 == 0) ? AP_RESPONSIVE : AP_SILENT;
        s_prepare_rc = (i % 7 == 0) ? -1 : 0;
        int rc = wlan_mgr_connect("FaultNet", "SuperSecretKey2026", s_ap_bssid);
        if (s_prepare_rc < 0) { assert(rc == -3); fail_cycles++; }
        else assert(rc == 0);
        if (i % 5 == 0 && rc == 0) {
            wlan_service_link(dev, 100000u + (uint64_t)i * 10000u, 1000);
            wlan_service_link(dev, 100000u + (uint64_t)i * 10000u + 7000u, 1000);
        }
        assert(wlan_mgr_disconnect() == 0);
        assert(wlan_mgr_status(&st) == 0 && st.state == WIFI_STATUS_IDLE);
        assert(!wlan_network_online(dev));
    }
    uint32_t setups = s_prepare_ok - ok0;
    uint32_t teardowns = s_deauth - de0;
    assert(fail_cycles > 0 && setups > 0);
    assert(teardowns >= setups);
    printf("  [PASS] Soak: 300 connect/disconnect cycles (%u setups, %u teardowns), no leaked state\n",
           setups, teardowns);
}

int main(void) {
    printf("[WIFI FAULT HOST TEST] Starting fault-injection verification...\n");
    test_health();
    test_manager();
    printf("\n>>> ALL WIFI FAULT-INJECTION HOST TESTS PASSED <<<\n");
    return 0;
}
