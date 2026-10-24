#include "wlan_mgr.h"
#include "spinlock.h"
#include "iwlwifi.h"
#include <string.h>

#define WLAN_RX_RING_SIZE 16

typedef struct {
    pbuf_t pbuf;
    uint8_t data[1536];
    bool in_use;
} wlan_rx_slot_t;

/* Global WLAN state */
static const wlan_hw_ops_t *s_hw_ops = NULL;
static net_dev_t            s_wlan_net_dev;
static spinlock_t           s_wlan_lock = SPINLOCK_RANKED(1, "wlan_mgr");

static uint8_t              s_local_mac[6];
static uint8_t              s_target_bssid[6];
static char                 s_target_ssid[IEEE80211_SSID_MAX_LEN + 1];
static uint8_t              s_target_pmk[WLAN_PMK_LEN];
static char                 s_cached_ssid[IEEE80211_SSID_MAX_LEN + 1];
static char                 s_cached_pass[65];
static uint8_t              s_cached_pmk[WLAN_PMK_LEN];
static bool                 s_has_cached_pmk = false;

static uint64_t             s_conn_start_time = 0;
static uint64_t             s_conn_last_retry_time = 0;
static uint32_t             s_conn_retry_count = 0;
static uint32_t             s_conn_tick_count = 0;

static uint8_t              s_current_channel = 1;
static int8_t               s_current_rssi = -60;
static uint16_t             s_target_beacon_int = 100;
static uint16_t             s_seq_num = 0;
static uint64_t             s_gtk_rx_pn = 0;

static wlan_state_t         s_wlan_state = WLAN_STATE_IDLE;
static wlan_bss_table_t     s_bss_table;
static wpa2_sm_t            s_wpa2_sm;

/* Statistics */
static uint64_t             s_stats_rx_packets = 0;
static uint64_t             s_stats_tx_packets = 0;

/* Rx Ring for Decapsulated 802.3 Frames */
static wlan_rx_slot_t       s_rx_slots[WLAN_RX_RING_SIZE];
static uint32_t             s_rx_head = 0;
static uint32_t             s_rx_tail = 0;
static uint32_t             s_rx_count = 0;

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
    memset(s_cached_ssid, 0, sizeof(s_cached_ssid));
    memset(s_cached_pass, 0, sizeof(s_cached_pass));
    memset(s_cached_pmk, 0, sizeof(s_cached_pmk));
    s_has_cached_pmk = false;
    s_conn_start_time = 0;
    s_conn_last_retry_time = 0;
    s_conn_retry_count = 0;
    s_conn_tick_count = 0;
    s_wlan_state = WLAN_STATE_IDLE;
    wlan_bss_table_init(&s_bss_table);
    memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
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
        while ((p = s_hw_ops->poll_raw_80211()) != NULL) {
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
        dst->security = src->has_wpa2_psk ? WIFI_SEC_WPA2_PSK : WIFI_SEC_OPEN;
        memcpy(dst->ssid, src->ssid, sizeof(dst->ssid) - 1);
    }
    out_result->count = count;

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

int wlan_mgr_connect(const char *ssid, const char *passphrase, const uint8_t bssid[6]) {
    if (!ssid || !passphrase) return -1;
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(passphrase);
    if (ssid_len == 0 || ssid_len > IEEE80211_SSID_MAX_LEN) return -1;
    if (pass_len < 8 || pass_len > 64) return -1;

    wlan_ensure_hardware();
    wlan_drain_hw_rx();

    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    memset(s_target_ssid, 0, sizeof(s_target_ssid));
    memcpy(s_target_ssid, ssid, ssid_len);

    /* Locate BSSID */
    bool found_bssid = false;
    if (bssid && (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5])) {
        memcpy(s_target_bssid, bssid, 6);
        const wlan_bss_entry_t *entry = wlan_bss_table_find(&s_bss_table, ssid);
        if (entry) {
            s_current_channel = entry->channel;
            s_current_rssi = entry->rssi;
            s_target_beacon_int = (entry->beacon_interval >= 20) ? entry->beacon_interval : 100;
        }
        found_bssid = true;
    } else {
        const wlan_bss_entry_t *entry = wlan_bss_table_find(&s_bss_table, ssid);
        if (entry) {
            memcpy(s_target_bssid, entry->bssid, 6);
            s_current_channel = entry->channel;
            s_current_rssi = entry->rssi;
            s_target_beacon_int = (entry->beacon_interval >= 20) ? entry->beacon_interval : 100;
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
            s_current_channel = entry->channel;
            s_current_rssi = entry->rssi;
            s_target_beacon_int = (entry->beacon_interval >= 20) ? entry->beacon_interval : 100;
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

    /* Fast-path PMK cache lookup: avoid 4096 rounds of PBKDF2 if reconnecting to same SSID & PSK */
    if (s_has_cached_pmk &&
        strcmp(s_cached_ssid, ssid) == 0 &&
        strcmp(s_cached_pass, passphrase) == 0) {
        memcpy(s_target_pmk, s_cached_pmk, WLAN_PMK_LEN);
    } else {
        wlan_pbkdf2_sha1(passphrase, (const uint8_t *)ssid, ssid_len, 4096, s_target_pmk, WLAN_PMK_LEN);
        memcpy(s_cached_pmk, s_target_pmk, WLAN_PMK_LEN);
        size_t sl = ssid_len < IEEE80211_SSID_MAX_LEN ? ssid_len : IEEE80211_SSID_MAX_LEN;
        memcpy(s_cached_ssid, ssid, sl);
        s_cached_ssid[sl] = '\0';
        size_t pl = pass_len < 64 ? pass_len : 64;
        memcpy(s_cached_pass, passphrase, pl);
        s_cached_pass[pl] = '\0';
        s_has_cached_pmk = true;
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
        s_hw_ops->prepare_auth(target_chan, target_bssid_copy, target_bi);
    }

    /* Build and send Open System Authentication Request (Seq 1) */
    irq = spin_lock_irqsave(&s_wlan_lock);
    if (s_hw_ops && s_hw_ops->send_raw_80211) {
        uint8_t auth_buf[64];
        size_t auth_len = net_80211_build_auth_req(auth_buf, sizeof(auth_buf), s_local_mac, s_target_bssid);
        if (auth_len > 0) {
            s_hw_ops->send_raw_80211(auth_buf, auth_len);
        }
    }
    spin_unlock_irqrestore(&s_wlan_lock, irq);

    /* Non-blocking asynchronous connect: frame reception, retries, and timeouts
     * progress autonomously in wlan_mgr_rx_raw_80211 and wlan_service_link */
    return 0;
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
        memset(s_target_pmk, 0, sizeof(s_target_pmk));
        s_gtk_rx_pn = 0;
    }
    spin_unlock_irqrestore(&s_wlan_lock, irq);

    if (was_active && s_hw_ops && s_hw_ops->deauth) {
        s_hw_ops->deauth();
    }
    return 0;
}

int wlan_mgr_status(wifi_status_t *out_status) {
    if (!out_status) return -1;
    wlan_ensure_hardware();
    wlan_drain_hw_rx();
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    if (s_wlan_state == WLAN_STATE_SCANNING) {
        if (s_hw_ops && s_hw_ops->is_scan_complete && s_hw_ops->is_scan_complete()) {
            s_wlan_state = WLAN_STATE_IDLE;
        }
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->state = (uint32_t)s_wlan_state;
    out_status->rssi = s_current_rssi;
    out_status->channel = s_current_channel;
    memcpy(out_status->bssid, s_target_bssid, 6);
    memcpy(out_status->ssid, s_target_ssid, sizeof(out_status->ssid) - 1);
    memcpy(out_status->mac, s_local_mac, 6);
    out_status->link_online = (s_wlan_state == WLAN_STATE_CONNECTED && wpa2_sm_is_authorized(&s_wpa2_sm)) ? 1 : 0;

    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return 0;
}

int wlan_mgr_rx_raw_80211(const uint8_t *frame, size_t len, int8_t rssi) {
    if (!frame || len < sizeof(ieee80211_hdr_t)) return -1;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)frame;
    uint8_t type = (hdr->frame_control >> 2) & 0x3;
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
            /* Auth Response */
            if (s_wlan_state == WLAN_STATE_AUTHENTICATING &&
                memcmp(hdr->addr2, s_target_bssid, 6) == 0 &&
                len >= 24 + 6) {
                uint16_t status = frame[28] | ((uint16_t)frame[29] << 8);
                if (status == 0) {
                    /* Auth Success -> Transition to ASSOCIATING and send Assoc Req */
                    s_wlan_state = WLAN_STATE_ASSOCIATING;
                    if (s_hw_ops && s_hw_ops->send_raw_80211) {
                        uint8_t assoc_buf[256];
                        size_t assoc_len = net_80211_build_assoc_req(assoc_buf, sizeof(assoc_buf),
                                                                    s_local_mac, s_target_bssid, s_target_ssid);
                        if (assoc_len > 0) s_hw_ops->send_raw_80211(assoc_buf, assoc_len);
                    }
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
                    /* Assoc Success -> Transition to 4-WAY HANDSHAKE */
                    s_wlan_state = WLAN_STATE_4WAY_HANDSHAKE;
                    s_gtk_rx_pn  = 0;
                    wpa2_sm_init(&s_wpa2_sm, s_local_mac, s_target_bssid, s_target_pmk);
                    uint16_t bi = s_target_beacon_int;
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    if (s_hw_ops && s_hw_ops->set_associated) {
                        s_hw_ops->set_associated(aid, bi);
                    }
                    return 0;
                }
            }
        } else if (subtype == IEEE80211_SUBTYPE_DEAUTH || subtype == IEEE80211_SUBTYPE_DISASSOC) {
            if (memcmp(hdr->addr2, s_target_bssid, 6) == 0) {
                bool was_connecting = (s_wlan_state == WLAN_STATE_AUTHENTICATING ||
                                       s_wlan_state == WLAN_STATE_ASSOCIATING ||
                                       s_wlan_state == WLAN_STATE_4WAY_HANDSHAKE);
                s_wlan_state = was_connecting ? WLAN_STATE_FAILED : WLAN_STATE_IDLE;
                s_wlan_net_dev.flags &= ~NET_RUNNING;
                memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
                memset(s_target_pmk, 0, sizeof(s_target_pmk));
                s_gtk_rx_pn = 0;
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
        size_t hdr_len = (subtype == IEEE80211_SUBTYPE_QOS_DATA) ? 26 : 24;
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
                
                uint8_t resp_eapol[512];
                int resp_len = wpa2_sm_rx_eapol(&s_wpa2_sm, payload + 8, payload_len - 8,
                                                resp_eapol, sizeof(resp_eapol));
                if (resp_len > 0) {
                    /* Encapsulate response EAPOL frame into 802.11 QoS Data frame and transmit */
                    uint8_t eth_eapol[512 + 14];
                    memcpy(eth_eapol, s_target_bssid, 6);
                    memcpy(eth_eapol + 6, s_local_mac, 6);
                    eth_eapol[12] = (EAPOL_ETHER_TYPE >> 8) & 0xFF;
                    eth_eapol[13] = EAPOL_ETHER_TYPE & 0xFF;
                    memcpy(eth_eapol + 14, resp_eapol, (size_t)resp_len);

                    uint8_t tx_wlan[1024];
                    int tx_len = net_80211_encapsulate(eth_eapol, 14 + (size_t)resp_len,
                                                      s_target_bssid, s_local_mac,
                                                      s_seq_num++, tx_wlan, sizeof(tx_wlan));
                    if (tx_len > 0 && s_hw_ops && s_hw_ops->send_raw_80211) {
                        s_hw_ops->send_raw_80211(tx_wlan, (size_t)tx_len);
                    }
                } else if (resp_len < 0 && s_wpa2_sm.handshake_step >= 2) {
                    /* EAPOL rejection (e.g. Message 3 MIC failure) -> fail authentication cleanly */
                    s_wlan_state = WLAN_STATE_FAILED;
                    s_wlan_net_dev.flags &= ~NET_RUNNING;
                    memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
                    memset(s_target_pmk, 0, sizeof(s_target_pmk));
                    s_gtk_rx_pn = 0;
                    spin_unlock_irqrestore(&s_wlan_lock, irq);
                    if (s_hw_ops && s_hw_ops->deauth) {
                        s_hw_ops->deauth();
                    }
                    return 0;
                }

                if (wpa2_sm_is_authorized(&s_wpa2_sm)) {
                    s_wlan_state = WLAN_STATE_CONNECTED;
                    s_wlan_net_dev.flags |= NET_RUNNING;
                }

                spin_unlock_irqrestore(&s_wlan_lock, irq);
                return 0;
            }
        }

        /* 4. Encrypted Data Frame (AES-128-CCMP) */
        if ((hdr->frame_control & IEEE80211_FCTL_PROTECTED) &&
            wpa2_sm_is_authorized(&s_wpa2_sm) &&
            memcmp(hdr->addr2, s_target_bssid, 6) == 0) {
            
            uint8_t unenc_frame[1600];
            size_t unenc_len = sizeof(unenc_frame);

            bool is_group = (hdr->addr1[0] & 0x01u) != 0;
            const uint8_t *dec_key = is_group ? s_wpa2_sm.gtk : s_wpa2_sm.tk;
            uint64_t *pn_tracker   = is_group ? &s_gtk_rx_pn : &s_wpa2_sm.rx_pn;

            /* Decrypt CCMP */
            int dec_rc = wlan_ccmp_decrypt_frame(dec_key, frame, len,
                                                 unenc_frame, &unenc_len,
                                                 pn_tracker);
            if (dec_rc == 0 && unenc_len > 0) {
                if (s_rx_count < WLAN_RX_RING_SIZE) {
                    wlan_rx_slot_t *slot = &s_rx_slots[s_rx_head];
                    int eth_len = net_80211_decapsulate(unenc_frame, unenc_len,
                                                        slot->data, sizeof(slot->data));
                    if (eth_len > 0) {
                        slot->pbuf.payload = slot->data;
                        slot->pbuf.length = (uint16_t)eth_len;
                        slot->pbuf.total_len = (uint16_t)eth_len;
                        slot->in_use = true;
                        s_rx_head = (s_rx_head + 1) % WLAN_RX_RING_SIZE;
                        s_rx_count++;
                        s_stats_rx_packets++;
                    }
                }
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
    uint8_t wlan_plain[1600];
    int wlan_plain_len = net_80211_encapsulate((const uint8_t *)buf, len,
                                              s_target_bssid, s_local_mac,
                                              s_seq_num++, wlan_plain, sizeof(wlan_plain));
    if (wlan_plain_len <= 0) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -1;
    }

    /* 2. Encrypt with AES-128-CCMP */
    uint8_t wlan_cipher[1650];
    size_t cipher_len = sizeof(wlan_cipher);
    int enc_rc = wlan_ccmp_encrypt_frame(s_wpa2_sm.tk, ++s_wpa2_sm.tx_pn,
                                         wlan_plain, (size_t)wlan_plain_len,
                                         wlan_cipher, &cipher_len);
    if (enc_rc != 0) {
        spin_unlock_irqrestore(&s_wlan_lock, irq);
        return -1;
    }

    /* 3. Transmit via hardware driver */
    int tx_res = -1;
    if (s_hw_ops && s_hw_ops->send_raw_80211) {
        tx_res = s_hw_ops->send_raw_80211(wlan_cipher, cipher_len);
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
    /* 1. Poll raw packets from hardware driver */
    if (s_hw_ops && s_hw_ops->poll_raw_80211) {
        pbuf_t *p;
        while ((p = s_hw_ops->poll_raw_80211()) != NULL) {
            int8_t rssi = (p->flags & IWL_PBUF_META_VALID) ? IWL_PBUF_GET_RSSI(p->flags) : -60;
            wlan_mgr_rx_raw_80211(p->payload, p->length, rssi);
            if (s_hw_ops->recycle_raw_80211) {
                s_hw_ops->recycle_raw_80211(p);
            }
        }
    }

    /* 2. Asynchronous state machine progression, retries & timeouts */
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
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
            s_wlan_state = WLAN_STATE_FAILED;
            s_wlan_net_dev.flags &= ~NET_RUNNING;
            memset(&s_wpa2_sm, 0, sizeof(s_wpa2_sm));
            needs_deauth = true;
        } else if (retry_due) {
            if (st == WLAN_STATE_AUTHENTICATING && s_hw_ops && s_hw_ops->send_raw_80211) {
                uint8_t auth_buf[64];
                size_t auth_len = net_80211_build_auth_req(auth_buf, sizeof(auth_buf), s_local_mac, s_target_bssid);
                if (auth_len > 0) s_hw_ops->send_raw_80211(auth_buf, auth_len);
            } else if (st == WLAN_STATE_ASSOCIATING && s_hw_ops && s_hw_ops->send_raw_80211) {
                uint8_t assoc_buf[256];
                size_t assoc_len = net_80211_build_assoc_req(assoc_buf, sizeof(assoc_buf),
                                                            s_local_mac, s_target_bssid, s_target_ssid);
                if (assoc_len > 0) s_hw_ops->send_raw_80211(assoc_buf, assoc_len);
            }
        }
    } else if (st == WLAN_STATE_CONNECTED && wpa2_sm_is_authorized(&s_wpa2_sm)) {
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
    bool online = (s_wlan_state == WLAN_STATE_CONNECTED) && wpa2_sm_is_authorized(&s_wpa2_sm);
    spin_unlock_irqrestore(&s_wlan_lock, irq);
    return online;
}

uint32_t wlan_link_state_abi(const net_dev_t *dev) {
    (void)dev;
    uint64_t irq = spin_lock_irqsave(&s_wlan_lock);
    uint32_t abi_state = NET_IF_LINK_DOWN;
    if (s_wlan_state == WLAN_STATE_CONNECTED && wpa2_sm_is_authorized(&s_wpa2_sm)) {
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
