#include "wlan_mgr.h"
#include "../rx_diag.h"
#include "spinlock.h"
#include "iwlwifi.h"
#include <string.h>

#if !defined(IWLWIFI_HOST_TEST) && !defined(WIFI_OWNER_HOST) && !defined(HOST_TEST)
#include "serial.h"
#else
#define serial_puts(s) ((void)(s))
#define serial_print_hex(v) ((void)(v))
#define serial_print_dec(v) ((void)(v))
#endif


enum { WD_RAW, WD_GROUP, WD_TARGET, WD_OK, WD_REPLAY, WD_FAIL,
       WD_CIPHER, WD_GTK_LEN, WD_GTK_IDX, WD_PACKET_IDX, WD_COUNT };
enum { ED_QUEUED, ED_ARP, ED_FULL, ED_BAD, ED_COUNT };
static uint64_t s_wlan_diag[WD_COUNT], s_eth_diag[ED_COUNT];
size_t wlan_rx_profile_format(char *out, size_t cap) {
    static const char *const wn[WD_COUNT] = {"raw", "group", "group-target",
        "group-ok", "group-replay", "group-decrypt-fail", "cipher", "gtk-len",
        "gtk-idx", "packet-key-idx"};
    static const char *const en[ED_COUNT] = {"queued", "arp", "ring-full", "decap-fail"};
    size_t n = rx_diag_format(out, cap, "[RXDIAG WLAN]", wn, s_wlan_diag, WD_COUNT);
    return n + rx_diag_format(out + n, cap - n, "[RXDIAG DECAP]", en, s_eth_diag, ED_COUNT);
}

#define WLAN_RX_RING_SIZE 16

typedef struct {
    pbuf_t pbuf;
    uint8_t data[1536];
    bool in_use;
} wlan_rx_slot_t;

/* Global WLAN state */
static const wlan_hw_ops_t *s_hw_ops = NULL;
static const wlan_mac_ops_t *s_registered_drivers[WLAN_MAX_DRIVERS];
static uint32_t             s_registered_driver_count = 0;
static net_dev_t            s_wlan_net_dev;
static spinlock_t           s_wlan_lock = SPINLOCK_RANKED(1, "wlan_mgr");

static uint8_t              s_local_mac[6];
static uint8_t              s_target_bssid[6];
static char                 s_target_ssid[IEEE80211_SSID_MAX_LEN + 1];
static uint8_t              s_target_pmk[WLAN_PMK_LEN];
static uint8_t              s_target_group_cipher = 0x04;
static uint16_t             s_target_rsn_caps = 0;
static bool                 s_target_is_open = false;
static bool                 s_target_use_sae = false;
static bool                 s_target_has_ht = false;
static bool                 s_target_has_vht = false;
static uint8_t              s_target_chan_width = 0;
static char                 s_cached_ssid[IEEE80211_SSID_MAX_LEN + 1];
static char                 s_cached_pass[65];
static uint8_t              s_cached_pmk[WLAN_PMK_LEN];
static bool                 s_has_cached_pmk = false;

static char                 s_pending_ssid[IEEE80211_SSID_MAX_LEN + 1];
static char                 s_pending_pass[65];
static uint8_t              s_pending_pmk[WLAN_PMK_LEN];
static bool                 s_has_pending_pmk = false;

static uint64_t             s_conn_start_time = 0;
static uint64_t             s_conn_last_retry_time = 0;
static uint32_t             s_conn_retry_count = 0;
static uint32_t             s_conn_tick_count = 0;

static uint8_t              s_current_channel = 1;
static int8_t               s_current_rssi = -60;
static uint16_t             s_target_beacon_int = 100;
static uint16_t             s_seq_num = 0;
static uint64_t             s_gtk_rx_pn = 0;
static uint64_t             s_gtk_rx_bitmap = 0;

static wlan_state_t         s_wlan_state = WLAN_STATE_IDLE;
static wlan_bss_table_t     s_bss_table;
static wpa2_sm_t            s_wpa2_sm;
static wlan_sae_ctx_t       s_sae_ctx;

/* Statistics */
static uint64_t             s_stats_rx_packets = 0;
static uint64_t             s_stats_tx_packets = 0;

/* Rx Ring for Decapsulated 802.3 Frames */
static wlan_rx_slot_t       s_rx_slots[WLAN_RX_RING_SIZE];
static uint32_t             s_rx_head = 0;
static uint32_t             s_rx_tail = 0;
static uint32_t             s_rx_count = 0;

/* Staging buffers protected by s_wlan_lock to preserve kernel thread stack */
static uint8_t              s_tx_wlan_plain[1600];
static uint8_t              s_tx_wlan_cipher[1650];
static uint8_t              s_rx_unenc_frame[1600];
static uint8_t              s_tx_eapol_eth[512 + 14];
static uint8_t              s_tx_eapol_wlan[1024];
static uint8_t              s_resp_eapol[512];
static uint8_t              s_mgmt_tx_buf[256];

int wlan_register_driver(const wlan_mac_ops_t *ops) {
    if (!ops || !ops->name) return -1;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    for (uint32_t i = 0; i < s_registered_driver_count; i++) {
        if (s_registered_drivers[i] == ops ||
            (s_registered_drivers[i]->name && strcmp(s_registered_drivers[i]->name, ops->name) == 0)) {
            s_registered_drivers[i] = ops;
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return 0;
        }
    }
    if (s_registered_driver_count >= WLAN_MAX_DRIVERS) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -1;
    }
    s_registered_drivers[s_registered_driver_count++] = ops;
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

int wlan_mgr_load_firmware(const void *fw_data, size_t fw_len) {
    if (!fw_data || fw_len == 0) return -1;
    if (s_hw_ops && s_hw_ops->load_firmware) {
        return s_hw_ops->load_firmware(fw_data, fw_len) ? 0 : -1;
    }
    for (uint32_t i = 0; i < s_registered_driver_count; i++) {
        const wlan_mac_ops_t *drv = s_registered_drivers[i];
        if (drv && drv->load_firmware && (!drv->is_detected || drv->is_detected())) {
            return drv->load_firmware(fw_data, fw_len) ? 0 : -1;
        }
    }
    return -2; /* Fallback to default driver loader if no explicit load_firmware hook */
}

void wlan_mgr_init(const wlan_hw_ops_t *ops) {
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    s_hw_ops = ops;
    memset(s_local_mac, 0, 6);
    if (ops && ops->get_mac) {
        ops->get_mac(s_local_mac);
    }
    memset(s_target_bssid, 0, 6);
    memset(s_target_ssid, 0, sizeof(s_target_ssid));
    memset(s_target_pmk, 0, sizeof(s_target_pmk));
    s_target_is_open = false;
    s_target_use_sae = false;
    s_target_has_ht = false;
    s_target_has_vht = false;
    s_target_chan_width = 0;
    memset(s_cached_ssid, 0, sizeof(s_cached_ssid));
    memset(s_cached_pass, 0, sizeof(s_cached_pass));
    memset(s_cached_pmk, 0, sizeof(s_cached_pmk));
    s_has_cached_pmk = false;
    memset(s_pending_pmk, 0, sizeof(s_pending_pmk));
    s_has_pending_pmk = false;
    s_conn_start_time = 0;
    s_conn_last_retry_time = 0;
    s_conn_retry_count = 0;
    s_conn_tick_count = 0;
    s_wlan_state = WLAN_STATE_IDLE;
    wlan_bss_table_init(&s_bss_table);
    memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
    memset(&s_sae_ctx, 0, sizeof(s_sae_ctx));
    s_stats_rx_packets = 0;
    s_stats_tx_packets = 0;

    /* Initialize Rx slots */
    memset(s_rx_slots, 0, sizeof(s_rx_slots));
    for (uint32_t i = 0; i < WLAN_RX_RING_SIZE; i++) {
        pbuf_init(&s_rx_slots[i].pbuf);
        s_rx_slots[i].pbuf.payload = s_rx_slots[i].data;
    }
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_count = 0;

    /* Populate polymorphic net_dev_t */
    memset(&s_wlan_net_dev, 0, sizeof(s_wlan_net_dev));
    memcpy(s_wlan_net_dev.name, "wlan0", 6);
    memcpy(s_wlan_net_dev.mac_addr, s_local_mac, 6);
    s_wlan_net_dev.mtu = 1500;
    s_wlan_net_dev.flags = NET_UP;
    s_wlan_net_dev.send_packet = wlan_send_packet;
    s_wlan_net_dev.poll_rx = wlan_poll_rx;
    s_wlan_net_dev.recycle_rx = wlan_recycle_rx;
    s_wlan_net_dev.service_link = wlan_service_link;
    s_wlan_net_dev.network_online = wlan_network_online;
    s_wlan_net_dev.link_state_abi = wlan_link_state_abi;
    s_wlan_net_dev.get_stats = wlan_get_stats;
    s_wlan_net_dev.priv = NULL;

    spin_unlock_irqrestore(&s_wlan_lock, irq);
}

static void wlan_ensure_hardware(void) {
    if (!s_hw_ops) {
        for (uint32_t i = 0; i < s_registered_driver_count; i++) {
            const wlan_mac_ops_t *drv = s_registered_drivers[i];
            if (drv && drv->is_detected && drv->is_detected()) {
                if (!drv->init_hw || drv->init_hw()) {
                    if (!s_hw_ops) wlan_mgr_init(drv);
                    break;
                }
            }
        }
    }
    if (!s_hw_ops && iwlwifi_is_detected()) {
        iwlwifi_init_hw();
    }
    if (s_hw_ops && s_hw_ops->get_mac) {
        uint8_t mac[6];
        if (s_hw_ops->get_mac(mac) == 0) {
            uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
            memcpy(s_local_mac, mac, 6);
            memcpy(s_wlan_net_dev.mac_addr, mac, 6);
            spin_unlock_irqrestore(&s_wlan_lock, irq);
        }
    }
}

net_dev_t *wlan_get_net_device(void) {
    wlan_ensure_hardware();
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    net_dev_t *dev = (s_hw_ops != NULL) ? &s_wlan_net_dev : NULL;
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return dev;
}

static void wlan_drain_hw_rx(void) {
    if (s_hw_ops && s_hw_ops->poll_raw_80211) {
        pbuf_t *p;
        int budget = 64;
        while (budget-- > 0 && (p = s_hw_ops->poll_raw_80211()) != NULL) {
            int8_t rssi = (p->flags & IWL_PBUF_META_VALID) ? IWL_PBUF_GET_RSSI(p->flags) : -60;
            wlan_mgr_rx_raw_80211(p->payload, p->length, rssi);
            if (s_hw_ops->recycle_raw_80211) {
                s_hw_ops->recycle_raw_80211(p);
            }
        }
    }
}

int wlan_mgr_scan(wifi_scan_result_t *out_result) {
    if (!out_result) return -1;
    wlan_ensure_hardware();

    if (out_result->max_entries == 0) {
        /* Asynchronously trigger multi-channel scan */
        uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
        if (s_wlan_state == WLAN_STATE_IDLE || s_wlan_state == WLAN_STATE_FAILED) {
            s_wlan_state = WLAN_STATE_SCANNING;
        }
        spin_unlock_irqrestore(&s_wlan_lock, irq);

        if (s_hw_ops && s_hw_ops->start_scan) {
            s_hw_ops->start_scan("");
        }
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t probe_buf[128];
            size_t probe_len = net_80211_build_probe_req(probe_buf, sizeof(probe_buf), s_local_mac, "");
            if (probe_len > 0) {
                s_hw_ops->send_raw_80211(probe_buf, probe_len);
            }
        }
        out_result->count = 0;
        return 0;
    }

    /* Drain pending hardware RX packets into scan table before acquiring lock */
    wlan_drain_hw_rx();

    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    if (s_wlan_state == WLAN_STATE_SCANNING) {
        s_wlan_state = WLAN_STATE_IDLE;
    }

    /* Populate entries from scan table */
    uint32_t count = s_bss_table.count;
    if (count > out_result->max_entries) count = out_result->max_entries;
    if (count > WIFI_MAX_SCAN_ENTRIES) count = WIFI_MAX_SCAN_ENTRIES;

    for (uint32_t i = 0; i < count; i++) {
        const wlan_bss_entry_t *src = &s_bss_table.entries[i];
        wifi_scan_entry_t *dst = &out_result->entries[i];
        memset(dst, 0, sizeof(*dst));
        memcpy(dst->bssid, src->bssid, 6);
        dst->channel = src->channel;
        dst->rssi = src->rssi;
        dst->ssid_len = src->ssid_len;
        if (src->has_wpa3_sae && src->has_wpa2_psk) {
            dst->security = WIFI_SEC_WPA2_WPA3;
        } else if (src->has_wpa3_sae) {
            dst->security = WIFI_SEC_WPA3_SAE;
        } else if (src->has_wpa2_psk) {
            dst->security = WIFI_SEC_WPA2_PSK;
        } else {
            dst->security = WIFI_SEC_OPEN;
        }
        memcpy(dst->ssid, src->ssid, sizeof(dst->ssid) - 1);
    }
    out_result->count = count;

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

static void wlan_select_bss_params(const wlan_bss_entry_t *entry) {
    if (!entry) return;
    s_current_channel = entry->channel;
    s_current_rssi = entry->rssi;
    s_target_beacon_int = (entry->beacon_interval >= 20) ? entry->beacon_interval : 100;
    s_target_group_cipher = entry->group_cipher ? entry->group_cipher : 0x04;
    s_target_rsn_caps = entry->rsn_caps;
    s_target_has_ht = entry->has_ht;
    s_target_has_vht = entry->has_vht;
    s_target_chan_width = entry->has_vht ? entry->vht_chan_width : entry->ht_chan_width;

    if (!entry->has_wpa2_psk && !entry->has_wpa3_sae && !(entry->capabilities & 0x0010)) {
        s_target_is_open = true;
        s_target_use_sae = false;
    } else if (entry->has_wpa2_psk) {
        s_target_is_open = false;
        s_target_use_sae = false;
    } else if (entry->has_wpa3_sae) {
        s_target_is_open = false;
        s_target_use_sae = true;
    } else {
        s_target_is_open = false;
        s_target_use_sae = false;
    }
}

int wlan_mgr_connect(const char *ssid, const char *passphrase, const uint8_t bssid[6]) {
    if (!ssid) return -1;
    const char *pass = passphrase ? passphrase : "";
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > IEEE80211_SSID_MAX_LEN) return -1;
    if (pass_len > 0 && (pass_len < 8 || pass_len > 64)) return -1;

    wlan_ensure_hardware();
    wlan_drain_hw_rx();

    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    /* If previously active, transmit deauth leaving BSS to clean air state */
    if (s_wlan_state != WLAN_STATE_IDLE && s_wlan_state != WLAN_STATE_FAILED) {
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t deauth_buf[64];
            size_t deauth_len = net_80211_build_deauth(deauth_buf, sizeof(deauth_buf),
                                                      s_local_mac, s_target_bssid, 3 /* Deauth leaving */);
            if (deauth_len > 0) s_hw_ops->send_raw_80211(deauth_buf, deauth_len);
        }
    }

    /* Unconditionally wipe any stale WPA2/SAE session authorization */
    memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
    memset(&s_sae_ctx, 0, sizeof(s_sae_ctx));
    s_gtk_rx_pn = 0;
    s_gtk_rx_bitmap = 0;
    s_target_is_open = false; /* Only a cached open beacon can authorize an open link. */
    s_target_use_sae = false;
    s_target_has_ht = false;
    s_target_has_vht = false;
    s_target_chan_width = 0;

    memset(s_target_ssid, 0, sizeof(s_target_ssid));
    memcpy(s_target_ssid, ssid, ssid_len);

    /* Locate BSSID */
    bool found_bssid = false;
    if (bssid && (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5])) {
        memcpy(s_target_bssid, bssid, 6);
        /* A GUI-selected BSSID may no longer be the strongest AP for this
         * SSID. Its PHY channel/timing must come from that exact AP, not the
         * current strongest same-name entry (the unpinned CLI policy). */
        const wlan_bss_entry_t *entry = NULL;
        for (uint32_t i=0;i<s_bss_table.count;++i) {
            const wlan_bss_entry_t *candidate=&s_bss_table.entries[i];
            if (!memcmp(candidate->bssid,bssid,6) && !strcmp(candidate->ssid,ssid)) {
                entry=candidate; break;
            }
        }
        if (entry) {
            wlan_select_bss_params(entry);
        }
        found_bssid = true;
    } else {
        const wlan_bss_entry_t *entry = wlan_bss_table_find(&s_bss_table, ssid);
        if (entry) {
            memcpy(s_target_bssid, entry->bssid, 6);
            wlan_select_bss_params(entry);
            found_bssid = true;
        }
    }

    /* If not in scan cache yet and hardware supports UMAC scan, perform a directed scan first */
    if (!found_bssid && s_hw_ops && s_hw_ops->start_scan) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        s_hw_ops->start_scan(ssid);
        wlan_drain_hw_rx();
        irq = spin_lock_irqsave(&s_wlan_lock);
        const wlan_bss_entry_t *entry = wlan_bss_table_find(&s_bss_table, ssid);
        if (entry) {
            memcpy(s_target_bssid, entry->bssid, 6);
            wlan_select_bss_params(entry);
            found_bssid = true;
        }
    }

    if (!found_bssid) {
        /* Broadcast probe for target SSID first */
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t probe_buf[128];
            size_t probe_len = net_80211_build_probe_req(probe_buf, sizeof(probe_buf), s_local_mac, ssid);
            if (probe_len > 0) s_hw_ops->send_raw_80211(probe_buf, probe_len);
        }
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -2; /* Not found in scan cache, probe sent */
    }

    /* An absent credential never makes a protected AP open. In particular,
     * fresh installations must not authorize data after association alone. */
    if (!s_target_is_open && pass_len == 0) {
        s_wlan_state = WLAN_STATE_FAILED;
        s_wlan_net_dev.flags &= ~NET_RUNNING;
        memset(s_target_pmk, 0, sizeof(s_target_pmk));
        s_has_pending_pmk = false;
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -4; /* Credential required; owner maps to EACCES. */
    }

    if (s_target_is_open) {
        memset(s_target_pmk, 0, sizeof(s_target_pmk));
        s_has_pending_pmk = false;
    } else if (s_target_use_sae) {
        if (wlan_sae_derive_pwe(&s_sae_ctx, s_local_mac, s_target_bssid, pass) != 0) {
            s_wlan_state = WLAN_STATE_FAILED;
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return -1;
        }
        s_has_pending_pmk = false;
    } else {
        /* Fast-path PMK cache lookup: avoid 4096 rounds of PBKDF2 if reconnecting to same SSID & PSK */
        if (s_has_cached_pmk &&
            strcmp(s_cached_ssid, ssid) == 0 &&
            strcmp(s_cached_pass, pass) == 0) {
            memcpy(s_target_pmk, s_cached_pmk, WLAN_PMK_LEN);
            s_has_pending_pmk = false;
        } else {
            wlan_pbkdf2_sha1(pass, (const uint8_t *)ssid, ssid_len, 4096, s_target_pmk, WLAN_PMK_LEN);
            memcpy(s_pending_pmk, s_target_pmk, WLAN_PMK_LEN);
            size_t sl = ssid_len < IEEE80211_SSID_MAX_LEN ? ssid_len : IEEE80211_SSID_MAX_LEN;
            memcpy(s_pending_ssid, ssid, sl);
            s_pending_ssid[sl] = '\0';
            size_t pl = pass_len < 64 ? pass_len : 64;
            memcpy(s_pending_pass, pass, pl);
            s_pending_pass[pl] = '\0';
            s_has_pending_pmk = true;
        }
    }

    uint8_t target_chan = s_current_channel;
    uint8_t target_bssid_copy[6];
    memcpy(target_bssid_copy, s_target_bssid, 6);
    uint16_t target_bi = s_target_beacon_int;

    /* Transition state to AUTHENTICATING */
    s_wlan_state = WLAN_STATE_AUTHENTICATING;
    s_wlan_net_dev.flags &= ~NET_RUNNING;
    s_conn_start_time = 0;
    s_conn_last_retry_time = 0;
    s_conn_retry_count = 0;
    s_conn_tick_count = 0;
    spin_unlock_irqrestore(&s_wlan_lock, irq);

    /* Configure MVM PHY/MAC/Binding/Station context and session protection Time Event */
    if (s_hw_ops && s_hw_ops->prepare_auth) {
        if (s_hw_ops->prepare_auth(target_chan, target_bssid_copy, target_bi) < 0) {
            /* Hardware refused (e.g. firmware DEAD): fail closed instead of timing out */
            irq = spin_lock_irqsave(&s_wlan_lock);
            s_wlan_state = WLAN_STATE_FAILED;
            s_wlan_net_dev.flags &= ~NET_RUNNING;
            memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return -3;
        }
    }

    /* Build and send Authentication Request (Open System Seq 1 or WPA3-SAE Commit Seq 1) */
    irq = spin_lock_irqsave(&s_wlan_lock);
    if (s_hw_ops && s_hw_ops->send_raw_80211) {
        size_t auth_len = 0;
        if (s_target_use_sae) {
            uint8_t commit_body[WLAN_SAE_COMMIT_LEN];
            if (wlan_sae_build_commit(&s_sae_ctx, commit_body) == 0) {
                auth_len = net_80211_build_sae_auth(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                    s_local_mac, s_target_bssid,
                                                    1, 0, commit_body, sizeof(commit_body));
            }
        } else {
            auth_len = net_80211_build_auth_req(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                s_local_mac, s_target_bssid);
        }
        if (auth_len > 0) {
            s_hw_ops->send_raw_80211(s_mgmt_tx_buf, auth_len);
        }
    }
    spin_unlock_irqrestore(&s_wlan_lock, irq);

    /* The Intel session-protection window needs prompt RX. The registered
     * owner stays runnable during connection setup and services <=64 RX per
     * turn, yielding between turns. Never monopolize it with a synchronous
     * 600000-iteration pump. Dell on-channel timing requires physical retest. */
    return 0;
}

bool wlan_mgr_work_pending(void) {
    uint64_t irq=spin_lock_irqsave(&s_wlan_lock);
    bool pending=s_wlan_state==WLAN_STATE_AUTHENTICATING ||
        s_wlan_state==WLAN_STATE_ASSOCIATING || s_wlan_state==WLAN_STATE_4WAY_HANDSHAKE;
    spin_unlock_irqrestore(&s_wlan_lock,irq);
    return pending;
}

int wlan_mgr_disconnect(void) {
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    bool was_active = (s_wlan_state != WLAN_STATE_IDLE);
    if (was_active) {
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t deauth_buf[64];
            size_t deauth_len = net_80211_build_deauth(deauth_buf, sizeof(deauth_buf),
                                                      s_local_mac, s_target_bssid, 3 /* Deauth leaving */);
            if (deauth_len > 0) s_hw_ops->send_raw_80211(deauth_buf, deauth_len);
        }
        s_wlan_state = WLAN_STATE_IDLE;
        s_wlan_net_dev.flags &= ~NET_RUNNING;
        memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
        memset(&s_sae_ctx, 0, sizeof(s_sae_ctx));
        memset(s_target_pmk, 0, sizeof(s_target_pmk));
        s_target_is_open = false;
        s_target_use_sae = false;
        s_gtk_rx_pn = 0;
        s_gtk_rx_bitmap = 0;
        s_has_cached_pmk = false;
        s_has_pending_pmk = false;
    }
    spin_unlock_irqrestore(&s_wlan_lock, irq);

    if (was_active && s_hw_ops && s_hw_ops->deauth) {
        s_hw_ops->deauth();
    }
    return 0;
}

int wlan_mgr_status(wifi_status_t *out_status) {
    if (!out_status) return -1;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    memset(out_status, 0, sizeof(*out_status));
    out_status->state = (uint32_t)s_wlan_state;
    out_status->rssi = s_current_rssi;
    out_status->channel = s_current_channel;
    memcpy(out_status->bssid, s_target_bssid, 6);
    memcpy(out_status->ssid, s_target_ssid, sizeof(out_status->ssid) - 1);
    memcpy(out_status->mac, s_local_mac, 6);
    out_status->link_online = (s_wlan_state == WLAN_STATE_CONNECTED &&
                               (s_target_is_open || wpa2_sm_is_authorized(&s_wpa2_sm))) ? 1 : 0;

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

static size_t wlan_build_current_assoc_req(uint8_t *buf, size_t max_len) {
    const uint8_t *pmkid = (s_target_use_sae && s_sae_ctx.peer_committed) ? s_sae_ctx.pmkid : NULL;
    bool allow_ht_vht = !(s_hw_ops && s_hw_ops->name && strcmp(s_hw_ops->name, "iwlwifi") == 0);
    return net_80211_build_assoc_req_full(buf, max_len,
                                          s_local_mac, s_target_bssid, s_target_ssid,
                                          s_target_is_open, s_target_use_sae,
                                          s_target_group_cipher, s_target_rsn_caps,
                                          pmkid,
                                          allow_ht_vht && s_target_has_ht,
                                          allow_ht_vht && s_target_has_vht);
}

static void wlan_enqueue_decapsulated_rx(const uint8_t *wlan_frame, size_t wlan_len) {
    if (s_rx_count < WLAN_RX_RING_SIZE) {
        wlan_rx_slot_t *slot = &s_rx_slots[s_rx_head];
        int eth_len = net_80211_decapsulate(wlan_frame, wlan_len,
                                            slot->data, sizeof(slot->data));
        if (eth_len > 0) {
            slot->pbuf.payload = slot->data;
            slot->pbuf.length = (uint16_t)eth_len;
            slot->pbuf.total_len = (uint16_t)eth_len;
            slot->in_use = true;
            s_rx_head = (s_rx_head + 1) % WLAN_RX_RING_SIZE;
            s_rx_count++;
            s_stats_rx_packets++;
            rx_diag_inc(&s_eth_diag[ED_QUEUED]);
            if (eth_len >= 14 && slot->data[12] == 0x08 && slot->data[13] == 0x06)
                rx_diag_inc(&s_eth_diag[ED_ARP]);
        } else rx_diag_inc(&s_eth_diag[ED_BAD]);
    } else rx_diag_inc(&s_eth_diag[ED_FULL]);
}

int wlan_mgr_rx_raw_80211(const uint8_t *frame, size_t len, int8_t rssi) {
    if (!frame || len < sizeof(ieee80211_hdr_t)) return -1;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)frame;
    rx_diag_inc(&s_wlan_diag[WD_RAW]);
    uint8_t type = (hdr->frame_control >> 2) & 0x3;
    if (type == IEEE80211_TYPE_DATA && (hdr->addr1[0] & 1u)) {
        rx_diag_inc(&s_wlan_diag[WD_GROUP]);
        if (memcmp(hdr->addr2, s_target_bssid, 6) == 0)
            rx_diag_inc(&s_wlan_diag[WD_TARGET]);
    }
    uint8_t subtype = (hdr->frame_control >> 4) & 0xF;

    /* 1. Beacon or Probe Response -> Update BSS Scan Table */
    if (type == IEEE80211_TYPE_MGMT &&
        (subtype == IEEE80211_SUBTYPE_BEACON || subtype == IEEE80211_SUBTYPE_PROBE_RESP)) {
        wlan_bss_entry_t bss;
        if (net_80211_parse_beacon_or_probe(frame, len, rssi, &bss)) {
            wlan_bss_table_update(&s_bss_table, &bss);
            if (memcmp(bss.bssid, s_target_bssid, 6) == 0) {
                s_current_rssi = rssi;
                s_current_channel = bss.channel;
            }
        }
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return 0;
    }

    /* 2. Management Frame Handling */
    if (type == IEEE80211_TYPE_MGMT) {
        if (subtype == IEEE80211_SUBTYPE_AUTH) {
            /* Auth Response (Open System or WPA3-SAE Commit/Confirm) */
            if (s_wlan_state == WLAN_STATE_AUTHENTICATING &&
                memcmp(hdr->addr2, s_target_bssid, 6) == 0 &&
                len >= 24 + 6) {
                uint16_t auth_alg = frame[24] | ((uint16_t)frame[25] << 8);
                uint16_t auth_seq = frame[26] | ((uint16_t)frame[27] << 8);
                uint16_t status   = frame[28] | ((uint16_t)frame[29] << 8);

                if (s_target_use_sae && auth_alg == IEEE80211_AUTH_ALG_SAE) {
                    if (status != 0) {
                        s_wlan_state = WLAN_STATE_FAILED;
                        s_wlan_net_dev.flags &= ~NET_RUNNING;
                    } else if (auth_seq == 1) {
                        /* AP SAE Commit -> derive KCK/PMK/PMKID and send SAE Confirm (Seq 2) */
                        if (wlan_sae_process_commit(&s_sae_ctx, frame + 30, len - 30) == 0) {
                            memcpy(s_target_pmk, s_sae_ctx.pmk, WLAN_PMK_LEN);
                            uint8_t confirm_body[WLAN_SAE_CONFIRM_LEN];
                            if (wlan_sae_build_confirm(&s_sae_ctx, confirm_body) == 0 &&
                                s_hw_ops && s_hw_ops->send_raw_80211) {
                                size_t clen = net_80211_build_sae_auth(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                                       s_local_mac, s_target_bssid,
                                                                       2, 0, confirm_body, sizeof(confirm_body));
                                if (clen > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, clen);
                            }
                        } else {
                            s_wlan_state = WLAN_STATE_FAILED;
                            s_wlan_net_dev.flags &= ~NET_RUNNING;
                        }
                    } else if (auth_seq == 2) {
                        /* AP SAE Confirm -> verify confirm token and transition to ASSOCIATING */
                        if (wlan_sae_verify_confirm(&s_sae_ctx, frame + 30, len - 30) == 0) {
                            serial_puts("[WLAN] WPA3-SAE Confirm verified, sending Assoc Req\n");
                            s_wlan_state = WLAN_STATE_ASSOCIATING;
                            if (s_hw_ops && s_hw_ops->send_raw_80211) {
                                size_t assoc_len = wlan_build_current_assoc_req(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf));
                                if (assoc_len > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, assoc_len);
                            }
                        } else {
                            s_wlan_state = WLAN_STATE_FAILED;
                            s_wlan_net_dev.flags &= ~NET_RUNNING;
                        }
                    }
                } else if (status == 0) {
                    serial_puts("[WLAN] Auth response SUCCESS from AP\n");
                    /* Auth Success -> Transition to ASSOCIATING and send Assoc Req */
                    s_wlan_state = WLAN_STATE_ASSOCIATING;
                    if (s_hw_ops && s_hw_ops->send_raw_80211) {
                        size_t assoc_len = wlan_build_current_assoc_req(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf));
                        if (assoc_len > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, assoc_len);
                    }
                } else {
                    serial_puts("[WLAN] Auth REJECTED by AP, status=0x");
                    serial_print_hex(status);
                    serial_puts("\n");
                    s_wlan_state = WLAN_STATE_FAILED;
                    s_wlan_net_dev.flags &= ~NET_RUNNING;
                }
            }
        } else if (subtype == IEEE80211_SUBTYPE_ASSOC_RESP) {
            /* Association Response */
            if (s_wlan_state == WLAN_STATE_ASSOCIATING &&
                memcmp(hdr->addr2, s_target_bssid, 6) == 0 &&
                len >= 24 + 6) {
                uint16_t status = frame[26] | ((uint16_t)frame[27] << 8);
                uint16_t aid    = (uint16_t)((frame[28] | ((uint16_t)frame[29] << 8)) & 0x3FFFu);
                if (status == 0) {
                    serial_puts("[WLAN] Assoc response SUCCESS from AP, AID=");
                    serial_print_dec(aid);
                    serial_puts("\n");
                    s_gtk_rx_pn  = 0;
                    s_gtk_rx_bitmap = 0;
                    wpa2_sm_init(&s_wpa2_sm, s_local_mac, s_target_bssid, s_target_pmk);
                    wpa2_sm_set_rsn(&s_wpa2_sm, s_target_group_cipher, s_target_rsn_caps);
                    if (s_target_use_sae) {
                        wpa2_sm_set_sae(&s_wpa2_sm, true, s_sae_ctx.pmkid);
                    }
                    if (s_target_is_open) {
                        /* Open / Public network: skip 4-way handshake and authorize port immediately */
                        serial_puts("[WLAN] Open network associated, Port AUTHORIZED (Connected)\n");
                        s_wpa2_sm.port_authorized = true;
                        s_wpa2_sm.handshake_step = 4;
                        s_wlan_state = WLAN_STATE_CONNECTED;
                        s_wlan_net_dev.flags |= NET_RUNNING;
                    } else {
                        /* Secured network: transition to 4-WAY HANDSHAKE */
                        s_wlan_state = WLAN_STATE_4WAY_HANDSHAKE;
                    }
                    uint16_t bi = s_target_beacon_int;
                    bool ht_en = s_target_has_ht;
                    bool vht_en = s_target_has_vht;
                    uint8_t cw = s_target_chan_width;
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    if (s_hw_ops && s_hw_ops->configure_ht_vht && (ht_en || vht_en)) {
                        s_hw_ops->configure_ht_vht(ht_en, vht_en, cw, 2);
                    }
                    if (s_hw_ops && s_hw_ops->set_associated) {
                        s_hw_ops->set_associated(aid, bi);
                    }
                    return 0;
                } else {
                    serial_puts("[WLAN] Assoc REJECTED by AP, status=");
                    serial_print_hex(status);
                    serial_puts("\n");
                    s_wlan_state = WLAN_STATE_FAILED;
                    s_wlan_net_dev.flags &= ~NET_RUNNING;
                }
            }
        } else if (subtype == IEEE80211_SUBTYPE_ACTION) {
            /* 802.11n/ac Block Ack Action Frame (Category 3 = BACK, Action 0 = ADDBA Req) */
            if (memcmp(hdr->addr2, s_target_bssid, 6) == 0 && len >= 24 + 9) {
                uint8_t category = frame[24];
                uint8_t action   = frame[25];
                if (category == IEEE80211_CATEGORY_BACK && action == IEEE80211_ACTION_ADDBA_REQ) {
                    uint8_t dialog_token = frame[26];
                    uint16_t ba_param = (uint16_t)(frame[27] | ((uint16_t)frame[28] << 8));
                    uint8_t tid = (uint8_t)((ba_param >> 2) & 0x0Fu);
                    uint16_t ssn_seq = (uint16_t)(frame[31] | ((uint16_t)frame[32] << 8));
                    uint16_t ssn = (uint16_t)(ssn_seq >> 4);
                    if (s_hw_ops && s_hw_ops->send_raw_80211) {
                        size_t rlen = net_80211_build_addba_resp(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                                 s_local_mac, s_target_bssid,
                                                                 dialog_token, tid, 64);
                        if (rlen > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, rlen);
                    }
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    if (s_hw_ops && s_hw_ops->add_ba_session) {
                        s_hw_ops->add_ba_session(tid, ssn, 64);
                    }
                    return 0;
                }
            }
        } else if (subtype == IEEE80211_SUBTYPE_DEAUTH || subtype == IEEE80211_SUBTYPE_DISASSOC) {
            if (memcmp(hdr->addr2, s_target_bssid, 6) == 0) {
                uint16_t reason = len >= 26 ? (frame[24] | ((uint16_t)frame[25] << 8)) : 0;
                serial_puts("[WLAN] AP Deauth/Disassoc, reason=");
                serial_print_hex(reason);
                serial_puts("\n");
                bool was_connecting = (s_wlan_state == WLAN_STATE_AUTHENTICATING ||
                                       s_wlan_state == WLAN_STATE_ASSOCIATING ||
                                       s_wlan_state == WLAN_STATE_4WAY_HANDSHAKE);
                s_wlan_state = was_connecting ? WLAN_STATE_FAILED : WLAN_STATE_IDLE;
                if (was_connecting) {
                    s_has_cached_pmk = false;
                    s_has_pending_pmk = false;
                }
                s_wlan_net_dev.flags &= ~NET_RUNNING;
                memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
                memset(&s_sae_ctx, 0, sizeof(s_sae_ctx));
                memset(s_target_pmk, 0, sizeof(s_target_pmk));
                s_gtk_rx_pn = 0;
                s_gtk_rx_bitmap = 0;
                spin_unlock_irqrestore(&s_wlan_lock, irq);
                if (s_hw_ops && s_hw_ops->deauth) {
                    s_hw_ops->deauth();
                }
                return 0;
            }
        }
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return 0;
    }

    /* 3. Data Frame Handling */
    if (type == IEEE80211_TYPE_DATA) {
        size_t hdr_len = (subtype == IEEE80211_SUBTYPE_QOS_DATA || (subtype & 0x08) != 0) ? 26 : 24;
        if (len < hdr_len) {
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return -1;
        }

        const uint8_t *payload = frame + hdr_len;
        size_t payload_len = len - hdr_len;

        /* Check for EAPOL Frame (Message 1 or Message 3) */
        /* LLC/SNAP for 802.1X is: AA AA 03 00 00 00 88 8E */
        if (payload_len >= 8 + sizeof(eapol_hdr_t)) {
            const llc_snap_hdr_t *snap = (const llc_snap_hdr_t *)payload;
            if (snap->dsap == 0xAA && snap->ssap == 0xAA && snap->control == 0x03 &&
                snap->oui[0] == 0 && snap->oui[1] == 0 && snap->oui[2] == 0 &&
                snap->ethertype == __builtin_bswap16(EAPOL_ETHER_TYPE)) {

                if (s_wlan_state != WLAN_STATE_4WAY_HANDSHAKE && s_wlan_state != WLAN_STATE_CONNECTED) {
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    return 0;
                }
                
                int resp_len = wpa2_sm_rx_eapol(&s_wpa2_sm, payload + 8, payload_len - 8,
                                                s_resp_eapol, sizeof(s_resp_eapol));
                if (resp_len > 0) {
                    if (s_wpa2_sm.handshake_step == 2) {
                        serial_puts("[WLAN] EAPOL M1 received, sent M2\n");
                    } else if (s_wpa2_sm.handshake_step == 4) {
                        serial_puts("[WLAN] EAPOL M3 received, sent M4\n");
                    }
                    /* Encapsulate response EAPOL frame into 802.11 QoS Data frame and transmit */
                    memcpy(s_tx_eapol_eth, s_target_bssid, 6);
                    memcpy(s_tx_eapol_eth + 6, s_local_mac, 6);
                    s_tx_eapol_eth[12] = (EAPOL_ETHER_TYPE >> 8) & 0xFF;
                    s_tx_eapol_eth[13] = EAPOL_ETHER_TYPE & 0xFF;
                    memcpy(s_tx_eapol_eth + 14, s_resp_eapol, (size_t)resp_len);

                    int tx_len = net_80211_encapsulate(s_tx_eapol_eth, 14 + (size_t)resp_len,
                                                      s_target_bssid, s_local_mac,
                                                      s_seq_num++, s_tx_eapol_wlan, sizeof(s_tx_eapol_wlan));
                    if (tx_len > 0 && s_hw_ops && s_hw_ops->send_raw_80211) {
                        s_hw_ops->send_raw_80211(s_tx_eapol_wlan, (size_t)tx_len);
                    }
                } else if (resp_len < 0 && s_wpa2_sm.handshake_step >= 2) {
                    serial_puts("[WLAN] EAPOL MIC verification FAILED (wrong passphrase or corrupt frame)\n");
                    /* EAPOL rejection (e.g. Message 3 MIC failure) -> fail authentication cleanly */
                    s_wlan_state = WLAN_STATE_FAILED;
                    s_wlan_net_dev.flags &= ~NET_RUNNING;
                    memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
                    memset(s_target_pmk, 0, sizeof(s_target_pmk));
                    s_gtk_rx_pn = 0;
                    s_gtk_rx_bitmap = 0;
                    s_has_cached_pmk = false;
                    s_has_pending_pmk = false;
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    if (s_hw_ops && s_hw_ops->deauth) {
                        s_hw_ops->deauth();
                    }
                    return 0;
                }

                if (wpa2_sm_is_authorized(&s_wpa2_sm)) {
                    serial_puts("[WLAN] 4-Way Handshake COMPLETE, Port AUTHORIZED (Connected)\n");
                    s_wlan_state = WLAN_STATE_CONNECTED;
                    s_wlan_net_dev.flags |= NET_RUNNING;
                    if (s_has_pending_pmk) {
                        memcpy(s_cached_pmk, s_pending_pmk, WLAN_PMK_LEN);
                        memcpy(s_cached_ssid, s_pending_ssid, sizeof(s_cached_ssid));
                        memcpy(s_cached_pass, s_pending_pass, sizeof(s_cached_pass));
                        s_has_cached_pmk = true;
                        s_has_pending_pmk = false;
                    }
                }

                spin_unlock_irqrestore(&s_wlan_lock, irq);
                return 0;
            }
        }

        /* 4A. Unencrypted Data Frame on Open Network */
        if (!(hdr->frame_control & IEEE80211_FCTL_PROTECTED) &&
            s_target_is_open &&
            s_wlan_state == WLAN_STATE_CONNECTED &&
            wpa2_sm_is_authorized(&s_wpa2_sm) &&
            memcmp(hdr->addr2, s_target_bssid, 6) == 0) {
            wlan_enqueue_decapsulated_rx(frame, len);
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return 0;
        }

        /* 4B. Encrypted Data Frame (AES-128-CCMP or TKIP Group) */
        if ((hdr->frame_control & IEEE80211_FCTL_PROTECTED) &&
            !s_target_is_open &&
            wpa2_sm_is_authorized(&s_wpa2_sm) &&
            memcmp(hdr->addr2, s_target_bssid, 6) == 0) {
            
            size_t unenc_len = sizeof(s_rx_unenc_frame);

            bool is_qos = (subtype == IEEE80211_SUBTYPE_QOS_DATA || (subtype & 0x08) != 0);
            uint8_t tid = (is_qos && len >= 26) ? (frame[24] & 0x0Fu) : 0;
            if (tid >= 16) tid = 0;

            bool is_group = (hdr->addr1[0] & 0x01u) != 0;
            const uint8_t *dec_key = is_group ? s_wpa2_sm.gtk : s_wpa2_sm.tk;
            uint64_t *pn_tracker   = is_group ? &s_gtk_rx_pn : &s_wpa2_sm.rx_pn[tid];
            uint64_t *bm_tracker   = is_group ? &s_gtk_rx_bitmap : &s_wpa2_sm.rx_bitmap[tid];

            if (is_group) {
                rx_diag_set(&s_wlan_diag[WD_CIPHER], s_target_group_cipher);
                rx_diag_set(&s_wlan_diag[WD_GTK_LEN], s_wpa2_sm.gtk_len);
                rx_diag_set(&s_wlan_diag[WD_GTK_IDX], s_wpa2_sm.gtk_idx);
                size_t iv_offset = is_qos ? 26u : 24u;
                if (len >= iv_offset + 4)
                    rx_diag_set(&s_wlan_diag[WD_PACKET_IDX], frame[iv_offset + 3] >> 6);
            }
            int dec_rc;
            if (is_group && s_target_group_cipher == 0x02) {
                /* Mixed-Mode AP Group Cipher = TKIP (Broadcast ARP / Multicast / DHCP Offer) */
                dec_rc = wlan_tkip_decrypt_frame(s_wpa2_sm.gtk, frame, len,
                                                 s_rx_unenc_frame, &unenc_len,
                                                 pn_tracker, bm_tracker);
            } else {
                /* Decrypt CCMP with per-TID 64-packet replay window */
                dec_rc = wlan_ccmp_decrypt_frame(dec_key, frame, len,
                                                 s_rx_unenc_frame, &unenc_len,
                                                 pn_tracker, bm_tracker);
            }
            if (is_group)
                rx_diag_inc(&s_wlan_diag[dec_rc == 0 ? WD_OK :
                    (dec_rc == -2 ? WD_REPLAY : WD_FAIL)]);
            if (dec_rc == 0 && unenc_len > 0) {
                wlan_enqueue_decapsulated_rx(s_rx_unenc_frame, unenc_len);
            }
        }
    }

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

int wlan_send_packet(net_dev_t *dev, const void *buf, size_t len) {
    (void)dev;
    if (!buf || len < 14 || len > 1514) return -1;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    if (s_wlan_state != WLAN_STATE_CONNECTED || !wpa2_sm_is_authorized(&s_wpa2_sm)) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -1;
    }

    /* 1. Encapsulate 802.3 Ethernet frame into 802.11 QoS Data Frame with LLC/SNAP */
    int wlan_plain_len = net_80211_encapsulate((const uint8_t *)buf, len,
                                              s_target_bssid, s_local_mac,
                                              s_seq_num++, s_tx_wlan_plain, sizeof(s_tx_wlan_plain));
    if (wlan_plain_len <= 0) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -1;
    }

    const uint8_t *tx_frame = s_tx_wlan_plain;
    size_t tx_frame_len = (size_t)wlan_plain_len;

    if (!s_target_is_open) {
        /* 2. Encrypt with AES-128-CCMP */
        size_t cipher_len = sizeof(s_tx_wlan_cipher);
        int enc_rc = wlan_ccmp_encrypt_frame(s_wpa2_sm.tk, ++s_wpa2_sm.tx_pn,
                                             s_tx_wlan_plain, (size_t)wlan_plain_len,
                                             s_tx_wlan_cipher, &cipher_len);
        if (enc_rc != 0) {
            spin_unlock_irqrestore(&s_wlan_lock, irq);
            return -1;
        }
        tx_frame = s_tx_wlan_cipher;
        tx_frame_len = cipher_len;
    }

    /* 3. Transmit via hardware driver */
    int tx_res = -1;
    if (s_hw_ops && s_hw_ops->send_raw_80211) {
        tx_res = s_hw_ops->send_raw_80211(tx_frame, tx_frame_len);
        if (tx_res == 0) s_stats_tx_packets++;
    }

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return tx_res;
}

pbuf_t *wlan_poll_rx(net_dev_t *dev) {
    (void)dev;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    if (s_rx_count == 0) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return NULL;
    }

    wlan_rx_slot_t *slot = &s_rx_slots[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % WLAN_RX_RING_SIZE;
    s_rx_count--;
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return &slot->pbuf;
}

void wlan_recycle_rx(net_dev_t *dev, pbuf_t *packet) {
    (void)dev;
    if (!packet) return;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    wlan_rx_slot_t *slot = (wlan_rx_slot_t *)packet;
    slot->in_use = false;
    spin_unlock_irqrestore(&s_wlan_lock, irq);
}

bool wlan_service_link(net_dev_t *dev, uint64_t now, uint64_t tick_hz) {
    /* Status is a pure snapshot. Scan completion belongs to the owner turn,
     * with no WLAN lock across the driver hook. */
    bool scan_complete=s_hw_ops && s_hw_ops->is_scan_complete && s_hw_ops->is_scan_complete();
    /* 1. Poll raw packets from hardware driver with bounded budget (up to 64 packets per tick) */
    if (s_hw_ops && s_hw_ops->poll_raw_80211) {
        pbuf_t *p;
        int budget = 64;
        while (budget-- > 0 && (p = s_hw_ops->poll_raw_80211()) != NULL) {
            int8_t rssi = (p->flags & IWL_PBUF_META_VALID) ? IWL_PBUF_GET_RSSI(p->flags) : -60;
            wlan_mgr_rx_raw_80211(p->payload, p->length, rssi);
            if (s_hw_ops->recycle_raw_80211) {
                s_hw_ops->recycle_raw_80211(p);
            }
        }
    }

    /* 2. Asynchronous state machine progression, retries & timeouts */
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    if (s_wlan_state==WLAN_STATE_SCANNING && scan_complete) s_wlan_state=WLAN_STATE_IDLE;
    wlan_state_t st = s_wlan_state;
    bool needs_deauth = false;
    bool trigger_roam = false;
    uint8_t roam_bssid[6];
    uint8_t roam_chan = 1;
    uint16_t roam_bi = 100;

    if (st == WLAN_STATE_AUTHENTICATING || st == WLAN_STATE_ASSOCIATING || st == WLAN_STATE_4WAY_HANDSHAKE) {
        if (s_conn_start_time == 0 && now > 0) {
            s_conn_start_time = now;
            s_conn_last_retry_time = now;
        }

        bool timeout = false;
        bool retry_due = false;

        if (tick_hz > 0 && s_conn_start_time > 0) {
            if (now >= s_conn_start_time + 6 * tick_hz) {
                timeout = true;
            } else if (now >= s_conn_last_retry_time + (tick_hz * 3 / 2)) { /* 1.5s interval */
                if (s_conn_retry_count < 3) {
                    retry_due = true;
                    s_conn_last_retry_time = now;
                    s_conn_retry_count++;
                }
            }
        } else {
            s_conn_tick_count++;
            if (s_conn_tick_count >= 600) {
                timeout = true;
            } else if ((s_conn_tick_count % 150) == 0 && s_conn_retry_count < 3) {
                retry_due = true;
                s_conn_retry_count++;
            }
        }

        if (timeout) {
            serial_puts("[WLAN] Connection timed out (6.0s), failing closed\n");
            s_wlan_state = WLAN_STATE_FAILED;
            s_wlan_net_dev.flags &= ~NET_RUNNING;
            memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
            s_has_cached_pmk = false;
            s_has_pending_pmk = false;
            needs_deauth = true;
        } else if (retry_due) {
            if (st == WLAN_STATE_AUTHENTICATING && s_hw_ops && s_hw_ops->send_raw_80211) {
                size_t auth_len = 0;
                if (s_target_use_sae) {
                    if (!s_sae_ctx.peer_committed) {
                        uint8_t commit_body[WLAN_SAE_COMMIT_LEN];
                        if (wlan_sae_build_commit(&s_sae_ctx, commit_body) == 0) {
                            auth_len = net_80211_build_sae_auth(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                                s_local_mac, s_target_bssid, 1, 0,
                                                                commit_body, sizeof(commit_body));
                        }
                    } else {
                        uint8_t confirm_body[WLAN_SAE_CONFIRM_LEN];
                        if (wlan_sae_build_confirm(&s_sae_ctx, confirm_body) == 0) {
                            auth_len = net_80211_build_sae_auth(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf),
                                                                s_local_mac, s_target_bssid, 2, 0,
                                                                confirm_body, sizeof(confirm_body));
                        }
                    }
                } else {
                    auth_len = net_80211_build_auth_req(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf), s_local_mac, s_target_bssid);
                }
                if (auth_len > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, auth_len);
            } else if (st == WLAN_STATE_ASSOCIATING && s_hw_ops && s_hw_ops->send_raw_80211) {
                size_t assoc_len = wlan_build_current_assoc_req(s_mgmt_tx_buf, sizeof(s_mgmt_tx_buf));
                if (assoc_len > 0) s_hw_ops->send_raw_80211(s_mgmt_tx_buf, assoc_len);
            }
        }
    } else if (st == WLAN_STATE_CONNECTED && (s_target_is_open || wpa2_sm_is_authorized(&s_wpa2_sm))) {
        /* Autonomous Roaming Monitor: if current link RSSI is weak (< -75 dBm),
         * check if an alternative BSSID for this SSID has significantly better signal (>= +15 dBm) */
        if (s_current_rssi < -75 && s_target_ssid[0] != '\0') {
            for (uint32_t i = 0; i < s_bss_table.count; i++) {
                const wlan_bss_entry_t *cand = &s_bss_table.entries[i];
                if (strcmp(cand->ssid, s_target_ssid) == 0 &&
                    memcmp(cand->bssid, s_target_bssid, 6) != 0 &&
                    cand->rssi >= (s_current_rssi + 15)) {
                    /* Stronger AP found in multi-BSSID network */
                    memcpy(roam_bssid, cand->bssid, 6);
                    roam_chan = cand->channel;
                    roam_bi = (cand->beacon_interval >= 20) ? cand->beacon_interval : 100;
                    trigger_roam = true;
                    break;
                }
            }
        }
    }

    if (trigger_roam) {
        /* Send deauth leaving BSS */
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t deauth_buf[64];
            size_t deauth_len = net_80211_build_deauth(deauth_buf, sizeof(deauth_buf),
                                                      s_local_mac, s_target_bssid, 3 /* Leaving BSS */);
            if (deauth_len > 0) s_hw_ops->send_raw_80211(deauth_buf, deauth_len);
        }
        memcpy(s_target_bssid, roam_bssid, 6);
        s_current_channel = roam_chan;
        s_target_beacon_int = roam_bi;
        s_current_rssi = -60;
        s_wlan_state = WLAN_STATE_AUTHENTICATING;
        s_wlan_net_dev.flags &= ~NET_RUNNING;
        memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
        s_gtk_rx_pn = 0;
        s_gtk_rx_bitmap = 0;
        s_conn_start_time = now;
        s_conn_last_retry_time = now;
        s_conn_retry_count = 0;
        s_conn_tick_count = 0;
    }

    spin_unlock_irqrestore(&s_wlan_lock, irq);

    if (needs_deauth && s_hw_ops && s_hw_ops->deauth) {
        s_hw_ops->deauth();
    }
    if (trigger_roam) {
        if (s_hw_ops && s_hw_ops->prepare_auth) {
            s_hw_ops->prepare_auth(roam_chan, roam_bssid, roam_bi);
        }
        if (s_hw_ops && s_hw_ops->send_raw_80211) {
            uint8_t auth_buf[64];
            size_t auth_len = net_80211_build_auth_req(auth_buf, sizeof(auth_buf), s_local_mac, roam_bssid);
            if (auth_len > 0) s_hw_ops->send_raw_80211(auth_buf, auth_len);
        }
    }

    return wlan_network_online(dev);
}

bool wlan_network_online(net_dev_t *dev) {
    (void)dev;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    bool online = (s_wlan_state == WLAN_STATE_CONNECTED) &&
                  (s_target_is_open || wpa2_sm_is_authorized(&s_wpa2_sm));
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return online;
}

uint32_t wlan_link_state_abi(const net_dev_t *dev) {
    (void)dev;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    uint32_t abi_state = NET_IF_LINK_DOWN;
    if (s_wlan_state == WLAN_STATE_CONNECTED &&
        (s_target_is_open || wpa2_sm_is_authorized(&s_wpa2_sm))) {
        abi_state = NET_IF_LINK_ONLINE;
    } else if (s_wlan_state != WLAN_STATE_IDLE && s_wlan_state != WLAN_STATE_FAILED) {
        abi_state = NET_IF_LINK_WAITING;
    }
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return abi_state;
}

void wlan_get_stats(const net_dev_t *dev, uint64_t *rx_packets, uint64_t *tx_packets) {
    (void)dev;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    if (rx_packets) *rx_packets = s_stats_rx_packets;
    if (tx_packets) *tx_packets = s_stats_tx_packets;
    spin_unlock_irqrestore(&s_wlan_lock, irq);
}
