#include "rtw88.h"
#include "serial.h"
#include "string.h"

static rtw88_state_t s_rtw_dev = {
    .lock = SPINLOCK_RANKED(2, "rtw88_ring"),
};

static rtw_tx_desc_t s_rtw_tx_ring[RTW_TX_RING_SIZE] __attribute__((aligned(64)));
static pbuf_t        s_rtw_rx_pbufs[RTW_RX_RING_SIZE];
static uint8_t       s_rtw_rx_buffers[RTW_RX_RING_SIZE][2048] __attribute__((aligned(64)));

static const wlan_mac_ops_t s_rtw88_mac_ops = {
    .name             = "rtw88",
    .caps             = WLAN_HW_CAP_2GHZ | WLAN_HW_CAP_5GHZ | WLAN_HW_CAP_HT | WLAN_HW_CAP_VHT | WLAN_HW_CAP_SAE | WLAN_HW_CAP_TKIP_GROUP,
    .is_detected      = rtw88_is_detected,
    .init_hw          = rtw88_init_hw,
    .load_firmware    = rtw88_load_firmware,
    .get_mac          = rtw88_get_mac,
    .set_channel      = rtw88_set_channel,
    .start_scan       = rtw88_start_scan,
    .prepare_auth     = rtw88_prepare_auth,
    .set_associated   = rtw88_set_associated,
    .deauth           = rtw88_deauth,
    .configure_ht_vht = rtw88_configure_ht_vht,
    .add_ba_session   = rtw88_add_ba_session,
    .send_raw_80211   = rtw88_send_raw,
    .poll_raw_80211   = rtw88_poll_raw,
    .recycle_raw_80211 = rtw88_recycle_raw,
    .is_scan_complete = rtw88_is_scan_complete,
};

const wlan_mac_ops_t *rtw88_get_mac_ops(void) {
    return &s_rtw88_mac_ops;
}

bool rtw88_is_supported_device(uint16_t vendor, uint16_t device) {
    if (vendor == RTW_PCI_VENDOR_ID) {
        switch (device) {
            case RTW_DEV_ID_8821CE:
            case RTW_DEV_ID_8822BE:
            case RTW_DEV_ID_8822CE:
            case RTW_DEV_ID_8852AE:
            case RTW_DEV_ID_8852BE:
                return true;
            default:
                return false;
        }
    }
    if (vendor == ATH_PCI_VENDOR_ID) {
        switch (device) {
            case ATH_DEV_ID_QCA6174:
            case ATH_DEV_ID_QCA9377:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool rtw88_is_detected(void) {
    return s_rtw_dev.detected;
}

bool rtw88_probe(void) {
    (void)wlan_register_driver(&s_rtw88_mac_ops);
#ifndef IWLWIFI_HOST_TEST
    static pci_device_t pci_devs[MAX_PCI_DEVICES];
    size_t count = pci_scan_all(pci_devs, MAX_PCI_DEVICES);
    for (size_t i = 0; i < count; i++) {
        if (rtw88_is_supported_device(pci_devs[i].vendor_id, pci_devs[i].device_id)) {
            s_rtw_dev.pci       = pci_devs[i];
            s_rtw_dev.vendor_id = pci_devs[i].vendor_id;
            s_rtw_dev.device_id = pci_devs[i].device_id;
            s_rtw_dev.detected  = true;
            serial_puts("[RTW88] Found PCIe Wireless device: ");
            serial_print_hex(pci_devs[i].vendor_id);
            serial_puts(":");
            serial_print_hex(pci_devs[i].device_id);
            serial_puts("\n");
            return true;
        }
    }
    return false;
#else
    return s_rtw_dev.detected;
#endif
}

bool rtw88_init_hw(void) {
    if (!s_rtw_dev.detected) return false;
    if (s_rtw_dev.initialized) return true;

    memset(s_rtw_tx_ring, 0, sizeof(s_rtw_tx_ring));
    s_rtw_dev.tx_head = 0;
    s_rtw_dev.tx_tail = 0;
    s_rtw_dev.rx_head = 0;
    s_rtw_dev.rx_tail = 0;

    for (uint32_t i = 0; i < RTW_RX_RING_SIZE; i++) {
        pbuf_init(&s_rtw_rx_pbufs[i]);
        s_rtw_rx_pbufs[i].payload = s_rtw_rx_buffers[i];
    }

    /* Realtek/Qualcomm default OUI fallback MAC: 00:E0:4C:88:21:01 */
    s_rtw_dev.mac_addr[0] = 0x00;
    s_rtw_dev.mac_addr[1] = 0xE0;
    s_rtw_dev.mac_addr[2] = 0x4C;
    s_rtw_dev.mac_addr[3] = 0x88;
    s_rtw_dev.mac_addr[4] = 0x21;
    s_rtw_dev.mac_addr[5] = 0x01;

    s_rtw_dev.current_channel = 1;
    s_rtw_dev.initialized     = true;
    s_rtw_dev.firmware_loaded = true;

    (void)wlan_register_driver(&s_rtw88_mac_ops);
    wlan_mgr_init(&s_rtw88_mac_ops);
    return true;
}

bool rtw88_load_firmware(const void *fw_data, size_t fw_len) {
    if (!fw_data || fw_len < 32) return false;
    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    s_rtw_dev.firmware_loaded = true;
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return true;
}

int rtw88_get_mac(uint8_t mac[6]) {
    if (!mac) return -1;
    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    memcpy(mac, s_rtw_dev.mac_addr, 6);
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return 0;
}

int rtw88_set_channel(uint8_t channel) {
    if (channel == 0) return -1;
    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    s_rtw_dev.current_channel = channel;
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return 0;
}

int rtw88_start_scan(const char *ssid) {
    (void)ssid;
    if (!s_rtw_dev.initialized) return -1;
    return 0;
}

int rtw88_prepare_auth(uint8_t channel, const uint8_t bssid[6], uint16_t beacon_interval) {
    (void)bssid;
    (void)beacon_interval;
    if (!s_rtw_dev.initialized) return -1;
    return rtw88_set_channel(channel);
}

int rtw88_set_associated(uint16_t assoc_id, uint16_t beacon_interval) {
    (void)assoc_id;
    (void)beacon_interval;
    if (!s_rtw_dev.initialized) return -1;
    return 0;
}

int rtw88_deauth(void) {
    return 0;
}

int rtw88_configure_ht_vht(bool ht_en, bool vht_en, uint8_t chan_width, uint8_t rx_nss) {
    (void)rx_nss;
    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    s_rtw_dev.ht_enabled  = ht_en;
    s_rtw_dev.vht_enabled = vht_en;
    s_rtw_dev.chan_width  = chan_width;
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return 0;
}

int rtw88_add_ba_session(uint8_t tid, uint16_t ssn, uint16_t buf_size) {
    (void)ssn;
    (void)buf_size;
    if (tid >= 8) return -1;
    return 0;
}

int rtw88_send_raw(const void *buf, size_t len) {
    if (!buf || len < 24 || len > 1900) return -1;
    if (!s_rtw_dev.initialized) return -1;

    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    uint32_t next = (s_rtw_dev.tx_head + 1) % RTW_TX_RING_SIZE;
    if (next != s_rtw_dev.tx_tail) {
        rtw_tx_desc_t *desc = &s_rtw_tx_ring[s_rtw_dev.tx_head];
        memset(desc, 0, sizeof(*desc));
        desc->dw0 = (uint32_t)(len & 0xFFFFu) | (1u << 31) /* OWN */;
        desc->buf_addr_low  = (uint32_t)(uintptr_t)buf;
        desc->buf_addr_high = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
        s_rtw_dev.tx_head = next;
    }
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return 0;
}

pbuf_t *rtw88_poll_raw(void) {
    if (!s_rtw_dev.initialized) return NULL;
    uint64_t irq = spin_lock_irqsave(&s_rtw_dev.lock);
    if (s_rtw_dev.rx_head == s_rtw_dev.rx_tail) {
        spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
        return NULL;
    }
    pbuf_t *p = &s_rtw_rx_pbufs[s_rtw_dev.rx_tail];
    s_rtw_dev.rx_tail = (s_rtw_dev.rx_tail + 1) % RTW_RX_RING_SIZE;
    spin_unlock_irqrestore(&s_rtw_dev.lock, irq);
    return p;
}

void rtw88_recycle_raw(pbuf_t *p) {
    (void)p;
}

bool rtw88_is_scan_complete(void) {
    return true;
}
