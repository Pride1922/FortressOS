#define RTL8153_HOST_TEST 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "rtl8153.h"

/* Mock spinlock implementation for host testing */
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!pthread_mutex_lock(&l->mutex));
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags;
    assert(!pthread_mutex_unlock(&l->mutex));
}

/* Mock XHCI MMIO and Control Transfer registers */
static uint8_t  s_mock_pla_regs[65536];
static uint8_t  s_mock_usb_regs[65536];
static uint32_t s_mock_mmio[256];

static uint32_t mock_read32(void *ctx, uint32_t offset) {
    (void)ctx;
    if (offset / 4 < 256) return s_mock_mmio[offset / 4];
    return 0;
}

static void mock_write32(void *ctx, uint32_t offset, uint32_t value) {
    (void)ctx;
    if (offset / 4 < 256) s_mock_mmio[offset / 4] = value;
}

static bool mock_delay_ms(void *ctx) {
    (void)ctx;
    return true;
}

/* Mock control transfer dispatcher */
bool xhci_dev_control_transfer(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               uint8_t slot_id,
                               usb_setup_pkt_t setup,
                               void *data_buf,
                               uint16_t length,
                               bool dir_in,
                               xhci_bot_device_t *device) {
    (void)io; (void)ring_dma; (void)dev_dma; (void)slot_id; (void)device;
    if (dir_in) {
        uint16_t mcu = setup.wIndex & 0xFF00;
        uint16_t idx = setup.wValue;
        uint8_t *bank = (mcu == MCU_TYPE_PLA) ? s_mock_pla_regs : s_mock_usb_regs;
        if (idx + length <= 65536) {
            memcpy(data_buf, &bank[idx], length);
            return true;
        }
    } else {
        uint16_t mcu = setup.wIndex & 0xFF00;
        uint16_t idx = setup.wValue;
        uint8_t *bank = (mcu == MCU_TYPE_PLA) ? s_mock_pla_regs : s_mock_usb_regs;
        if (idx + length <= 65536) {
            memcpy(&bank[idx], data_buf, length);
            return true;
        }
    }
    return true;
}

/* Mock xHCI command sending */
bool xhci_dev_send_command(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *dma,
                           xhci_trb_t trb,
                           xhci_trb_t *out_event) {
    (void)io; (void)dma; (void)trb;
    if (out_event) {
        out_event->status = (1u << 24); /* Success */
    }
    return true;
}

/* Mock mailbox transfer wait */
static bool s_mock_transfer_success = true;
static uint32_t s_mock_rx_transfer_len = 0;

bool xhci_mbox_wait_transfer(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *dma,
                             uint8_t slot_id,
                             uint8_t dci,
                             uintptr_t submitted_phys,
                             uint32_t *out_residual,
                             uint8_t *out_code,
                             unsigned timeout_us) {
    (void)io; (void)dma; (void)slot_id; (void)submitted_phys; (void)timeout_us;
    if (!s_mock_transfer_success) return false;

    if (dci == 3) { /* Bulk IN */
        if (s_mock_rx_transfer_len > 0) {
            if (out_residual) *out_residual = RTL8153_RX_BUF_SIZE - s_mock_rx_transfer_len;
            if (out_code) *out_code = 1; /* Success */
            s_mock_rx_transfer_len = 0; /* Consumed */
            return true;
        }
        return false;
    }

    if (dci == 4) { /* Bulk OUT */
        if (out_residual) *out_residual = 0;
        if (out_code) *out_code = 1;
        return true;
    }

    return true;
}

/* ============================================================================
 * Test Cases
 * ============================================================================ */

static void test_device_identification(void) {
    printf("[TEST 1] Testing RTL8153 device identification...\n");

    /* Realtek vendor ID matches */
    assert(rtl8153_is_supported_device(0x0BDA, 0x8153)); /* RTL8153 Gigabit */
    assert(rtl8153_is_supported_device(0x0BDA, 0x8152)); /* RTL8152 Fast Ethernet */
    assert(rtl8153_is_supported_device(0x0BDA, 0x8151)); /* RTL8151 */
    assert(rtl8153_is_supported_device(0x0BDA, 0x8156)); /* RTL8156 2.5G */

    /* Dell OEM Dock matches */
    assert(rtl8153_is_supported_device(0x413C, 0xB06E)); /* Dell WD19S dock */
    assert(rtl8153_is_supported_device(0x413C, 0xB06F));

    /* Unrelated devices rejected */
    assert(!rtl8153_is_supported_device(0x8086, 0x100E)); /* Intel e1000 */
    assert(!rtl8153_is_supported_device(0x0BDA, 0x8168)); /* PCI RTL8168 */
    assert(!rtl8153_is_supported_device(0x1234, 0x5678));

    printf("[TEST 1] PASS: Device identification validated.\n");
}

static xhci_rings_io_t s_test_io = {
    .read32 = mock_read32,
    .write32 = mock_write32,
    .delay_ms = mock_delay_ms,
};
static xhci_dma_buffers_t s_test_ring_dma;
static uint32_t s_test_mock_input_ctx[1024];
static xhci_dev_dma_t s_test_dev_dma = {
    .input_ctx_virt = s_test_mock_input_ctx,
};
static xhci_bot_device_t s_test_bot_dev = {
    .slot_id = 2,
    .port_num = 1,
    .speed = 4, /* SuperSpeed */
    .bulk_in_ep = 0x81,
    .bulk_out_ep = 0x02,
    .bulk_in_max_packet = 1024,
    .bulk_out_max_packet = 1024,
};

static void test_initialization_and_mac(void) {
    printf("[TEST 2] Testing initialization and MAC retrieval...\n");

    memset(s_mock_pla_regs, 0, sizeof(s_mock_pla_regs));
    memset(s_mock_usb_regs, 0, sizeof(s_mock_usb_regs));
    memset(s_mock_mmio, 0, sizeof(s_mock_mmio));
    s_mock_mmio[0x14 / 4] = 0x100; /* Doorbell offset */

    /* Seed PLA_IDR with test MAC 00:E0:4C:68:01:23 */
    uint8_t test_mac[6] = {0x00, 0xE0, 0x4C, 0x68, 0x01, 0x23};
    memcpy(&s_mock_pla_regs[PLA_IDR], test_mac, 6);

    /* Set PHYSTATUS with link up, Gigabit */
    uint16_t phy = RTL8153_PHY_LINK_STATUS | RTL8153_PHY_1000M | RTL8153_PHY_FULL_DUP;
    memcpy(&s_mock_pla_regs[PLA_PHYSTATUS], &phy, 2);

    memset(&s_test_ring_dma, 0, sizeof(s_test_ring_dma));
    memset(s_test_mock_input_ctx, 0, sizeof(s_test_mock_input_ctx));

    bool ok = rtl8153_configure_device(&s_test_io, &s_test_ring_dma, &s_test_dev_dma, &s_test_bot_dev);
    assert(ok);

    net_dev_t *dev = rtl8153_get_net_device();
    assert(dev != NULL);
    assert(strcmp(dev->name, "eth1") == 0);
    assert(memcmp(dev->mac_addr, test_mac, 6) == 0);
    assert(dev->mtu == 1500);
    assert(dev->flags & NET_UP);

    /* Verify chip registers configured:
     * - PLA_RCR has APM | AM | AB
     * - PLA_RMS is 1536
     * - USB_USB_CTRL has RX_AGG_DISABLE
     * - PLA_CR has RE | TE */
    uint32_t rcr = 0;
    memcpy(&rcr, &s_mock_pla_regs[PLA_RCR], 4);
    assert((rcr & (RTL8153_RCR_APM | RTL8153_RCR_AM | RTL8153_RCR_AB)) ==
           (RTL8153_RCR_APM | RTL8153_RCR_AM | RTL8153_RCR_AB));

    uint16_t rms = 0;
    memcpy(&rms, &s_mock_pla_regs[PLA_RMS], 2);
    assert(rms == 1536);

    uint16_t usb_ctrl = 0;
    memcpy(&usb_ctrl, &s_mock_usb_regs[USB_USB_CTRL], 2);
    assert(usb_ctrl & RTL8153_RX_AGG_DISABLE);

    uint8_t cr = s_mock_pla_regs[PLA_CR];
    assert(cr & (RTL8153_CR_RE | RTL8153_CR_TE));

    printf("[TEST 2] PASS: Initialization, MAC, and registers validated.\n");
}

static void test_tx_framing(void) {
    printf("[TEST 3] Testing transmit descriptor framing...\n");

    net_dev_t *dev = rtl8153_get_net_device();
    assert(dev != NULL);

    /* Construct a 64-byte test Ethernet frame */
    uint8_t test_frame[64];
    memset(test_frame, 0xAA, sizeof(test_frame));
    test_frame[12] = 0x08; /* Ethertype IPv4 */
    test_frame[13] = 0x00;

    int ret = dev->send_packet(dev, test_frame, sizeof(test_frame));
    assert(ret == 0);

    /* Check stats */
    uint64_t rx_pkts = 0, tx_pkts = 0;
    dev->get_stats(dev, &rx_pkts, &tx_pkts);
    assert(tx_pkts == 1);

    /* Oversized frame rejected */
    uint8_t big_frame[2000];
    assert(dev->send_packet(dev, big_frame, sizeof(big_frame)) == -1);

    /* Undersized frame rejected */
    uint8_t small_frame[10];
    assert(dev->send_packet(dev, small_frame, sizeof(small_frame)) == -1);

    printf("[TEST 3] PASS: TX framing and boundaries validated.\n");
}

static void test_rx_framing_and_crc_stripping(void) {
    printf("[TEST 4] Testing receive descriptor parsing and CRC stripping...\n");

    net_dev_t *dev = rtl8153_get_net_device();
    assert(dev != NULL);

    /* When no packet is waiting, poll_rx returns NULL */
    pbuf_t *p = dev->poll_rx(dev);
    assert(p == NULL);

    /* Inject a 60-byte payload + 4-byte CRC = 64-byte packet into mock RX DMA */
    uint32_t payload_len = 60;
    uint32_t pkt_len = payload_len + 4; /* RTL8153 includes 4-byte CRC */

    uint8_t mock_rx_stream[256] = {0};
    rtl8153_rx_desc_t *desc = (rtl8153_rx_desc_t *)mock_rx_stream;
    desc->opts1 = pkt_len; /* opts1 & RTL8153_RX_LEN_MASK */
    desc->opts2 = 0;
    desc->opts3 = 0;

    uint8_t *wire_data = mock_rx_stream + sizeof(rtl8153_rx_desc_t);
    for (uint32_t i = 0; i < payload_len; i++) {
        wire_data[i] = (uint8_t)(i + 1);
    }
    /* 4-byte dummy CRC */
    wire_data[payload_len + 0] = 0xDE;
    wire_data[payload_len + 1] = 0xAD;
    wire_data[payload_len + 2] = 0xBE;
    wire_data[payload_len + 3] = 0xEF;

    /* Copy into driver's mock RX buffer */
    uint8_t *rx_buf_ptr = rtl8153_test_get_rx_buf();
    assert(rx_buf_ptr != NULL);
    memcpy(rx_buf_ptr, mock_rx_stream, sizeof(rtl8153_rx_desc_t) + pkt_len);

    s_mock_rx_transfer_len = sizeof(rtl8153_rx_desc_t) + pkt_len;

    /* Poll RX */
    p = dev->poll_rx(dev);
    assert(p != NULL);
    assert(p->length == payload_len);
    assert(p->total_len == payload_len);
    assert(p->payload == p->data);

    /* Verify payload data matches, and CRC was stripped */
    for (uint32_t i = 0; i < payload_len; i++) {
        assert(p->payload[i] == (uint8_t)(i + 1));
    }

    /* Recycle packet buffer */
    dev->recycle_rx(dev, p);

    /* Check stats */
    uint64_t rx_pkts = 0, tx_pkts = 0;
    dev->get_stats(dev, &rx_pkts, &tx_pkts);
    assert(rx_pkts == 1);

    printf("[TEST 4] PASS: RX parsing, CRC stripping, and buffer recycling validated.\n");
}

static void test_link_state_and_polling(void) {
    printf("[TEST 5] Testing physical link state monitoring and rate limiting...\n");

    net_dev_t *dev = rtl8153_get_net_device();
    assert(dev != NULL);

    /* Initial state: link is online */
    bool online = dev->service_link(dev, 1000, 1000);
    assert(online);
    assert(dev->network_online(dev));
    assert(dev->link_state_abi(dev) == NET_IF_LINK_ONLINE);

    /* Simulate cable unplug: clear LINK_STATUS in PLA_PHYSTATUS */
    uint16_t phy = RTL8153_PHY_1000M | RTL8153_PHY_FULL_DUP; /* No LINK_STATUS */
    memcpy(&s_mock_pla_regs[PLA_PHYSTATUS], &phy, 2);

    /* If polled too fast (< 200ms elapsed), returns cached state */
    bool cached = dev->service_link(dev, 1050, 1000);
    assert(cached == true); /* Rate-limited */

    /* After 200ms (now = 1250), polls PHY and notices link down */
    online = dev->service_link(dev, 1250, 1000);
    assert(!online);
    assert(!dev->network_online(dev));
    assert(dev->link_state_abi(dev) == NET_IF_LINK_DOWN);

    /* Reconnect cable */
    phy |= RTL8153_PHY_LINK_STATUS;
    memcpy(&s_mock_pla_regs[PLA_PHYSTATUS], &phy, 2);

    online = dev->service_link(dev, 1500, 1000);
    assert(online);
    assert(dev->network_online(dev));
    assert(dev->link_state_abi(dev) == NET_IF_LINK_ONLINE);

    printf("[TEST 5] PASS: Link state transitions and rate-limiting validated.\n");
}

int main(void) {
    printf("================================================================\n");
    printf("  RTL8153 USB 3.0 Gigabit Ethernet Host Verification Suite       \n");
    printf("================================================================\n");

    test_device_identification();
    test_initialization_and_mac();
    test_tx_framing();
    test_rx_framing_and_crc_stripping();
    test_link_state_and_polling();

    printf("================================================================\n");
    printf("  ALL 5 RTL8153 HOST TEST SUITES PASSED (100%%)                   \n");
    printf("================================================================\n");
    return 0;
}
