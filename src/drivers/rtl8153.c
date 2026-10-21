#include "rtl8153.h"
#include <string.h>

#ifndef RTL8153_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "apic.h"
#include "time.h"
#include "serial.h"
#include "spinlock.h"
#else
#include <stdio.h>
#define serial_puts(s)          printf("%s", s)
#define serial_print_hex(v)     printf("%lx", (unsigned long)(v))
#endif

#define XHCI_TRB_TYPE_NORMAL          1u
#define XHCI_TRB_TYPE_LINK            6u
#define XHCI_TRB_TYPE_CONFIG_EP_CMD   12u

/* Forward declarations */
static void rtl8153_clflush(const void *p, size_t len);

/* RTL8153 Controller and Ring State */
typedef struct {
    bool                active;
    uint8_t             slot_id;
    uint8_t             port_num;
    uint8_t             speed;
    uint8_t             bulk_in_ep;
    uint8_t             bulk_out_ep;
    uint8_t             bulk_in_dci;
    uint8_t             bulk_out_dci;
    uint16_t            bulk_in_max_packet;
    uint16_t            bulk_out_max_packet;
    uint8_t             mac_addr[6];

    /* Bulk IN Transfer Ring */
    uintptr_t           rx_ring_phys;
    xhci_trb_t         *rx_ring_virt;
    uint32_t            rx_ring_idx;
    uint8_t             rx_ring_cycle;

    /* Bulk OUT Transfer Ring */
    uintptr_t           tx_ring_phys;
    xhci_trb_t         *tx_ring_virt;
    uint32_t            tx_ring_idx;
    uint8_t             tx_ring_cycle;

    /* DMA Buffers */
    uintptr_t           rx_buf_phys;
    uint8_t            *rx_buf_virt;
    uintptr_t           tx_buf_phys;
    uint8_t            *tx_buf_virt;

    /* I/O Context pointers */
    const xhci_rings_io_t *io;
    xhci_dma_buffers_t    *ring_dma;
    const xhci_dev_dma_t  *dev_dma;
    xhci_bot_device_t     *bot_device;

    /* Link and statistics state */
    bool                link_online;
    uint64_t            last_link_poll;
    uint64_t            rx_packets;
    uint64_t            tx_packets;
    uint64_t            rx_bytes;
    uint64_t            tx_bytes;
    uint64_t            rx_errors;
    uint64_t            tx_errors;
} rtl8153_state_t;

static rtl8153_state_t s_rtl8153;
static net_dev_t       s_net_dev;
static spinlock_t      s_rtl_lock = SPINLOCK_RANKED(1, "rtl8153");

/* Static packet buffer pool for RX */
static pbuf_t          s_rx_packets[RTL8153_RX_POOL_SIZE];
static uint8_t         s_rx_pbuf_used[RTL8153_RX_POOL_SIZE];

static void rtl8153_clflush(const void *p, size_t len) {
#if defined(__x86_64__) || defined(_M_X64)
    uintptr_t start = (uintptr_t)p;
    uintptr_t end = start + len;
    start &= ~63ULL;
    for (; start < end; start += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(start) : "memory");
    }
#else
    (void)p; (void)len;
#endif
}

bool rtl8153_is_supported_device(uint16_t vid, uint16_t pid) {
    if (vid == REALTEK_VID) {
        return (pid == RTL8153_PID || pid == RTL8152_PID ||
                pid == RTL8151_PID || pid == RTL8156_PID);
    }
    if (vid == DELL_OEM_VID) {
        return (pid == DELL_WD19S_PID1 || pid == DELL_WD19S_PID2);
    }
    return false;
}

/* OCP Control Transfer Read */
static bool rtl8153_ocp_read(rtl8153_state_t *state,
                             uint16_t index,
                             uint16_t mcu_type,
                             void *data,
                             uint16_t len) {
    if (!state || !state->io || !state->ring_dma || !state->dev_dma) return false;
    usb_setup_pkt_t setup = {
        .bmRequestType = 0xC0, /* Device to Host, Vendor, Device */
        .bRequest = RTL8153_REQ_GET_REGS,
        .wValue = index,
        .wIndex = mcu_type,
        .wLength = len,
    };
    return xhci_dev_control_transfer(state->io, state->ring_dma, state->dev_dma,
                                     state->slot_id, setup, data, len, true, state->bot_device);
}

/* OCP Control Transfer Write */
static bool rtl8153_ocp_write(rtl8153_state_t *state,
                              uint16_t index,
                              uint16_t mcu_type,
                              uint16_t byteen,
                              const void *data,
                              uint16_t len) {
    if (!state || !state->io || !state->ring_dma || !state->dev_dma) return false;
    usb_setup_pkt_t setup = {
        .bmRequestType = 0x40, /* Host to Device, Vendor, Device */
        .bRequest = RTL8153_REQ_SET_REGS,
        .wValue = index,
        .wIndex = (uint16_t)(mcu_type | byteen),
        .wLength = len,
    };
    return xhci_dev_control_transfer(state->io, state->ring_dma, state->dev_dma,
                                     state->slot_id, setup, (void *)data, len, false, state->bot_device);
}

static uint8_t rtl8153_read_byte(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index) {
    uint32_t val = 0;
    uint16_t aligned_idx = index & ~3u;
    if (!rtl8153_ocp_read(state, aligned_idx, mcu_type, &val, 4)) return 0xFF;
    uint8_t shift = (uint8_t)((index & 3u) * 8);
    return (uint8_t)((val >> shift) & 0xFFu);
}

static void rtl8153_write_byte(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index, uint8_t val) {
    uint32_t data = (uint32_t)val << ((index & 3u) * 8);
    uint16_t byteen = (uint16_t)(BYTE_EN_BYTE << (index & 3u));
    rtl8153_ocp_write(state, index & ~3u, mcu_type, byteen, &data, 4);
}

static uint16_t rtl8153_read_word(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index) {
    uint32_t val = 0;
    uint16_t aligned_idx = index & ~3u;
    if (!rtl8153_ocp_read(state, aligned_idx, mcu_type, &val, 4)) return 0xFFFF;
    uint8_t shift = (uint8_t)((index & 2u) * 8);
    return (uint16_t)((val >> shift) & 0xFFFFu);
}

static void rtl8153_write_word(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index, uint16_t val) {
    uint32_t data = (uint32_t)val << ((index & 2u) * 8);
    uint16_t byteen = (uint16_t)(BYTE_EN_WORD << (index & 2u));
    rtl8153_ocp_write(state, index & ~3u, mcu_type, byteen, &data, 4);
}

static uint32_t __attribute__((unused)) rtl8153_read_dword(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index) {
    uint32_t val = 0;
    if (!rtl8153_ocp_read(state, index & ~3u, mcu_type, &val, 4)) return 0xFFFFFFFFu;
    return val;
}

static void rtl8153_write_dword(rtl8153_state_t *state, uint16_t mcu_type, uint16_t index, uint32_t val) {
    rtl8153_ocp_write(state, index & ~3u, mcu_type, BYTE_EN_DWORD, &val, 4);
}

static void rtl8153_ring_doorbell(const xhci_rings_io_t *io, uint8_t slot_id, uint8_t dci) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    io->write32(io->mmio_ctx, dboff + slot_id * 4, dci);
}

static void rtl8153_arm_rx_buffer(rtl8153_state_t *state) {
    uint32_t idx = state->rx_ring_idx;
    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)state->rx_buf_phys;
    trb.parameter_high = (uint32_t)(state->rx_buf_phys >> 32);
    trb.status = RTL8153_RX_BUF_SIZE;
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | (1u << 5) /* IOC */ | (state->rx_ring_cycle ? 1u : 0u);

    state->rx_ring_virt[idx] = trb;
    rtl8153_clflush(&state->rx_ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    state->rx_ring_idx++;
    if (state->rx_ring_idx == XHCI_RING_TRB_COUNT - 1) {
        state->rx_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (state->rx_ring_cycle ? 1u : 0u);
        rtl8153_clflush(&state->rx_ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        state->rx_ring_idx = 0;
        state->rx_ring_cycle ^= 1;
    }

    rtl8153_ring_doorbell(state->io, state->slot_id, state->bulk_in_dci);
}

/* Configures the RTL8153 USB device under xHCI */
bool rtl8153_configure_device(const xhci_rings_io_t *io,
                              xhci_dma_buffers_t *ring_dma,
                              xhci_dev_dma_t *dev_dma,
                              xhci_bot_device_t *device) {
    if (!io || !ring_dma || !dev_dma || !device) return false;

    serial_puts("[RTL8153] Configuring Realtek USB Gigabit Ethernet on Slot ");
    serial_print_hex(device->slot_id);
    serial_puts("\n");

    memset(&s_rtl8153, 0, sizeof(s_rtl8153));
    s_rtl8153.slot_id = device->slot_id;
    s_rtl8153.port_num = device->port_num;
    s_rtl8153.speed = device->speed;
    s_rtl8153.bulk_in_ep = device->bulk_in_ep;
    s_rtl8153.bulk_out_ep = device->bulk_out_ep;
    s_rtl8153.bulk_in_max_packet = device->bulk_in_max_packet ? device->bulk_in_max_packet : 1024;
    s_rtl8153.bulk_out_max_packet = device->bulk_out_max_packet ? device->bulk_out_max_packet : 1024;

    uint8_t ep_in_num = device->bulk_in_ep & 0x0Fu;
    uint8_t ep_out_num = device->bulk_out_ep & 0x0Fu;
    s_rtl8153.bulk_in_dci = (uint8_t)((ep_in_num * 2) + 1);
    s_rtl8153.bulk_out_dci = (uint8_t)(ep_out_num * 2);

    s_rtl8153.io = io;
    s_rtl8153.ring_dma = ring_dma;
    s_rtl8153.dev_dma = dev_dma;
    s_rtl8153.bot_device = device;
    s_rtl8153.rx_ring_cycle = 1;
    s_rtl8153.tx_ring_cycle = 1;

#ifndef RTL8153_HOST_TEST
    uintptr_t rx_ring_p = pmm_alloc_page();
    uintptr_t tx_ring_p = pmm_alloc_page();
    uintptr_t rx_buf_p = pmm_alloc_page();
    uintptr_t tx_buf_p = pmm_alloc_page();

    if (!rx_ring_p || !tx_ring_p || !rx_buf_p || !tx_buf_p) {
        if (rx_ring_p) pmm_free_page(rx_ring_p);
        if (tx_ring_p) pmm_free_page(tx_ring_p);
        if (rx_buf_p) pmm_free_page(rx_buf_p);
        if (tx_buf_p) pmm_free_page(tx_buf_p);
        serial_puts("[RTL8153] OOM allocating DMA buffers\n");
        return false;
    }

    s_rtl8153.rx_ring_phys = rx_ring_p;
    s_rtl8153.rx_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(rx_ring_p);
    s_rtl8153.tx_ring_phys = tx_ring_p;
    s_rtl8153.tx_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(tx_ring_p);
    s_rtl8153.rx_buf_phys = rx_buf_p;
    s_rtl8153.rx_buf_virt = (uint8_t *)vmm_phys_to_virt(rx_buf_p);
    s_rtl8153.tx_buf_phys = tx_buf_p;
    s_rtl8153.tx_buf_virt = (uint8_t *)vmm_phys_to_virt(tx_buf_p);
#else
    static xhci_trb_t mock_rx_ring[XHCI_RING_TRB_COUNT];
    static xhci_trb_t mock_tx_ring[XHCI_RING_TRB_COUNT];
    static uint8_t mock_rx_buf[4096];
    static uint8_t mock_tx_buf[4096];

    s_rtl8153.rx_ring_phys = 0x300000;
    s_rtl8153.rx_ring_virt = mock_rx_ring;
    s_rtl8153.tx_ring_phys = 0x400000;
    s_rtl8153.tx_ring_virt = mock_tx_ring;
    s_rtl8153.rx_buf_phys = 0x500000;
    s_rtl8153.rx_buf_virt = mock_rx_buf;
    s_rtl8153.tx_buf_phys = 0x600000;
    s_rtl8153.tx_buf_virt = mock_tx_buf;
#endif

    memset(s_rtl8153.rx_ring_virt, 0, 4096);
    memset(s_rtl8153.tx_ring_virt, 0, 4096);
    memset(s_rtl8153.rx_buf_virt, 0, 4096);
    memset(s_rtl8153.tx_buf_virt, 0, 4096);

    /* Link TRBs at end of rings */
    s_rtl8153.rx_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)s_rtl8153.rx_ring_phys;
    s_rtl8153.rx_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(s_rtl8153.rx_ring_phys >> 32);
    s_rtl8153.rx_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_C;
    rtl8153_clflush(s_rtl8153.rx_ring_virt, 4096);

    s_rtl8153.tx_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)s_rtl8153.tx_ring_phys;
    s_rtl8153.tx_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(s_rtl8153.tx_ring_phys >> 32);
    s_rtl8153.tx_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_C;
    rtl8153_clflush(s_rtl8153.tx_ring_virt, 4096);

    /* Setup Input Context for Configure Endpoint */
    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5; /* 64-byte vs 32-byte contexts */
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx   = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx   = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *ep_in_ctx  = &dev_dma->input_ctx_virt[(s_rtl8153.bulk_in_dci + 1) * ctx_dwords];
    uint32_t *ep_out_ctx = &dev_dma->input_ctx_virt[(s_rtl8153.bulk_out_dci + 1) * ctx_dwords];

    uint8_t max_dci = (s_rtl8153.bulk_in_dci > s_rtl8153.bulk_out_dci) ?
                       s_rtl8153.bulk_in_dci : s_rtl8153.bulk_out_dci;

    /* Add Context Flags: A0 (Slot Context), Bulk IN, Bulk OUT */
    ctrl_ctx[1] = (1u << 0) | (1u << s_rtl8153.bulk_in_dci) | (1u << s_rtl8153.bulk_out_dci);

    /* Slot Context: Context Entries = max_dci, preserving Route String & TT */
    if (dev_dma->slots_output_ctx_virt[device->slot_id]) {
        uint32_t *out_slot = dev_dma->slots_output_ctx_virt[device->slot_id];
        slot_ctx[0] = (out_slot[0] & ~0xF8000000u) | ((uint32_t)max_dci << 27);
        slot_ctx[1] = out_slot[1];
        slot_ctx[2] = out_slot[2];
        slot_ctx[3] = out_slot[3];
    } else {
        slot_ctx[0] = ((uint32_t)device->speed << 20) | ((uint32_t)max_dci << 27);
        slot_ctx[1] = (uint32_t)device->port_num << 16;
    }

    /* Bulk IN Endpoint Context:
     * DW1: CErr = 3, EP Type = 6 (Bulk IN), Max Packet Size
     * DW2-3: TR Dequeue Pointer (DCS = 1)
     * DW4: Average TRB Length = 1536 */
    ep_in_ctx[1] = (3u << 1) | (6u << 3) | ((uint32_t)s_rtl8153.bulk_in_max_packet << 16);
    ep_in_ctx[2] = ((uint32_t)s_rtl8153.rx_ring_phys & ~0x3fu) | 1u;
    ep_in_ctx[3] = (uint32_t)(s_rtl8153.rx_ring_phys >> 32);
    ep_in_ctx[4] = 1536;

    /* Bulk OUT Endpoint Context:
     * DW1: CErr = 3, EP Type = 2 (Bulk OUT), Max Packet Size
     * DW2-3: TR Dequeue Pointer (DCS = 1)
     * DW4: Average TRB Length = 1536 */
    ep_out_ctx[1] = (3u << 1) | (2u << 3) | ((uint32_t)s_rtl8153.bulk_out_max_packet << 16);
    ep_out_ctx[2] = ((uint32_t)s_rtl8153.tx_ring_phys & ~0x3fu) | 1u;
    ep_out_ctx[3] = (uint32_t)(s_rtl8153.tx_ring_phys >> 32);
    ep_out_ctx[4] = 1536;

    rtl8153_clflush(dev_dma->input_ctx_virt, 4096);
    __asm__ volatile("mfence" ::: "memory");

    /* Issue Configure Endpoint Command */
    xhci_trb_t cfg_cmd = {0};
    cfg_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    cfg_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    cfg_cmd.control = (XHCI_TRB_TYPE_CONFIG_EP_CMD << 10) | ((uint32_t)device->slot_id << 24);

    xhci_trb_t comp_event = {0};
    if (!xhci_dev_send_command(io, ring_dma, cfg_cmd, &comp_event)) {
        serial_puts("[RTL8153] Configure Endpoint command failed\n");
        return false;
    }

    /* Read permanent MAC address from PLA_IDR */
    uint8_t mac[8] = {0};
    if (!rtl8153_ocp_read(&s_rtl8153, PLA_IDR, MCU_TYPE_PLA, mac, 6)) {
        serial_puts("[RTL8153] PLA_IDR read failed, trying PLA_BACKUP\n");
        rtl8153_ocp_read(&s_rtl8153, PLA_BACKUP, MCU_TYPE_PLA, mac, 6);
    }

    /* Validate MAC: if all zeroes or all 0xFF, fallback */
    bool mac_valid = false;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00 && mac[i] != 0xFF) {
            mac_valid = true;
            break;
        }
    }
    if (!mac_valid) {
        mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
        mac[3] = 0x81; mac[4] = 0x53; mac[5] = 0x01;
    }
    memcpy(s_rtl8153.mac_addr, mac, 6);

    serial_puts("[RTL8153] Hardware MAC: ");
    for (int i = 0; i < 6; i++) {
        serial_print_hex(mac[i]);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    /* Soft reset NIC */
    rtl8153_write_byte(&s_rtl8153, MCU_TYPE_PLA, PLA_CR, RTL8153_CR_RST);
    for (int i = 0; i < 100; i++) {
        if (!(rtl8153_read_byte(&s_rtl8153, MCU_TYPE_PLA, PLA_CR) & RTL8153_CR_RST)) {
            break;
        }
#ifndef RTL8153_HOST_TEST
        if (io->delay_ms) io->delay_ms(io->mmio_ctx);
#endif
    }

    /* Disable OOB / Teredo */
    rtl8153_write_byte(&s_rtl8153, MCU_TYPE_PLA, PLA_OOB_CTRL, 0x00);

    /* Configure packet filter: APM | AM | AB */
    rtl8153_write_dword(&s_rtl8153, MCU_TYPE_PLA, PLA_RCR,
                        RTL8153_RCR_APM | RTL8153_RCR_AM | RTL8153_RCR_AB);

    /* Set maximum frame size (1536 bytes) */
    rtl8153_write_word(&s_rtl8153, MCU_TYPE_PLA, PLA_RMS, 1536);

    /* Disable RX aggregation for low latency and Dell WD19S dock compatibility */
    rtl8153_write_word(&s_rtl8153, MCU_TYPE_USB, USB_USB_CTRL,
                       RTL8153_RX_AGG_DISABLE | RTL8153_RX_ZERO_EN);

    /* Enable Receiver and Transmitter */
    rtl8153_write_byte(&s_rtl8153, MCU_TYPE_PLA, PLA_CR,
                       RTL8153_CR_RE | RTL8153_CR_TE);

    /* Pre-arm Bulk IN ring with RX buffer */
    rtl8153_arm_rx_buffer(&s_rtl8153);

    /* Populate polymorphic net_dev_t */
    memset(&s_net_dev, 0, sizeof(s_net_dev));
    memcpy(s_net_dev.name, "eth1", 5);
    memcpy(s_net_dev.mac_addr, s_rtl8153.mac_addr, 6);
    s_net_dev.mtu = 1500;
    s_net_dev.flags = NET_UP;
    s_net_dev.send_packet = rtl8153_send_packet;
    s_net_dev.poll_rx = rtl8153_poll_rx;
    s_net_dev.recycle_rx = rtl8153_recycle_rx;
    s_net_dev.service_link = rtl8153_service_link;
    s_net_dev.network_online = rtl8153_network_online;
    s_net_dev.link_state_abi = rtl8153_link_state_abi;
    s_net_dev.get_stats = rtl8153_get_stats;
    s_net_dev.priv = &s_rtl8153;

    s_rtl8153.active = true;
    serial_puts("[RTL8153] Initialized and registered as net device eth1\n");
    return true;
}

net_dev_t *rtl8153_get_net_device(void) {
    if (!s_rtl8153.active) return NULL;
    return &s_net_dev;
}

int rtl8153_send_packet(net_dev_t *dev, const void *buf, size_t len) {
    if (!dev || dev != &s_net_dev || !buf || len < 14 || len > RTL8153_MAX_FRAME_SIZE) {
        return -1;
    }
    if (!s_rtl8153.active || !s_rtl8153.tx_buf_virt || !s_rtl8153.tx_ring_virt) {
        return -1;
    }

    uint64_t irq = spin_lock_irqsave(&s_rtl_lock);

    /* Write 8-byte tx_desc header followed by packet data */
    rtl8153_tx_desc_t *tx_desc = (rtl8153_tx_desc_t *)s_rtl8153.tx_buf_virt;
    tx_desc->opts1 = (uint32_t)len | RTL8153_TX_FS | RTL8153_TX_LS;
    tx_desc->opts2 = 0;

    memcpy(s_rtl8153.tx_buf_virt + sizeof(rtl8153_tx_desc_t), buf, len);
    uint32_t total_tx_len = (uint32_t)(sizeof(rtl8153_tx_desc_t) + len);

    rtl8153_clflush(s_rtl8153.tx_buf_virt, total_tx_len);
    __asm__ volatile("mfence" ::: "memory");

    /* Submit Normal TRB on Bulk OUT ring */
    uint32_t idx = s_rtl8153.tx_ring_idx;
    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)s_rtl8153.tx_buf_phys;
    trb.parameter_high = (uint32_t)(s_rtl8153.tx_buf_phys >> 32);
    trb.status = total_tx_len;
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | (1u << 5) /* IOC */ | (s_rtl8153.tx_ring_cycle ? 1u : 0u);

    s_rtl8153.tx_ring_virt[idx] = trb;
    rtl8153_clflush(&s_rtl8153.tx_ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    s_rtl8153.tx_ring_idx++;
    if (s_rtl8153.tx_ring_idx == XHCI_RING_TRB_COUNT - 1) {
        s_rtl8153.tx_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (s_rtl8153.tx_ring_cycle ? 1u : 0u);
        rtl8153_clflush(&s_rtl8153.tx_ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        s_rtl8153.tx_ring_idx = 0;
        s_rtl8153.tx_ring_cycle ^= 1;
    }

    rtl8153_ring_doorbell(s_rtl8153.io, s_rtl8153.slot_id, s_rtl8153.bulk_out_dci);

    /* Wait for transmission completion */
    uint32_t residual = 0;
    uint8_t comp_code = 0;
    bool ok = xhci_mbox_wait_transfer(s_rtl8153.io, s_rtl8153.ring_dma,
                                     s_rtl8153.slot_id, s_rtl8153.bulk_out_dci,
                                     s_rtl8153.tx_buf_phys, &residual, &comp_code, 50000 /* 50ms */);

    if (ok) {
        s_rtl8153.tx_packets++;
        s_rtl8153.tx_bytes += len;
    } else {
        s_rtl8153.tx_errors++;
    }

    spin_unlock_irqrestore(&s_rtl_lock, irq);
    return ok ? 0 : -1;
}

pbuf_t *rtl8153_poll_rx(net_dev_t *dev) {
    if (!dev || dev != &s_net_dev || !s_rtl8153.active) return NULL;
    if (!s_rtl8153.rx_buf_virt || !s_rtl8153.rx_ring_virt) return NULL;

    uint64_t irq = spin_lock_irqsave(&s_rtl_lock);

    /* Check if an RX packet transfer completed on Bulk IN */
    uint32_t residual = 0;
    uint8_t comp_code = 0;
    bool completed = xhci_mbox_wait_transfer(s_rtl8153.io, s_rtl8153.ring_dma,
                                            s_rtl8153.slot_id, s_rtl8153.bulk_in_dci,
                                            s_rtl8153.rx_buf_phys, &residual, &comp_code, 0 /* non-blocking */);

    if (!completed) {
        spin_unlock_irqrestore(&s_rtl_lock, irq);
        return NULL;
    }

    uint32_t transferred = RTL8153_RX_BUF_SIZE > residual ? (RTL8153_RX_BUF_SIZE - residual) : 0;
    if (transferred < sizeof(rtl8153_rx_desc_t) + 14 + 4) {
        /* Short/invalid transfer; re-arm and return */
        rtl8153_arm_rx_buffer(&s_rtl8153);
        spin_unlock_irqrestore(&s_rtl_lock, irq);
        return NULL;
    }

    rtl8153_clflush(s_rtl8153.rx_buf_virt, transferred);
    __asm__ volatile("mfence" ::: "memory");

    rtl8153_rx_desc_t *desc = (rtl8153_rx_desc_t *)s_rtl8153.rx_buf_virt;
    uint32_t pkt_len = desc->opts1 & RTL8153_RX_LEN_MASK;

    if (pkt_len < 14 + 4 || pkt_len > RTL8153_MAX_FRAME_SIZE + 4) {
        s_rtl8153.rx_errors++;
        rtl8153_arm_rx_buffer(&s_rtl8153);
        spin_unlock_irqrestore(&s_rtl_lock, irq);
        return NULL;
    }

    /* Strip 4-byte Ethernet CRC */
    uint32_t frame_len = pkt_len - 4;

    /* Locate a free pbuf from our static pool */
    int free_slot = -1;
    for (int i = 0; i < (int)RTL8153_RX_POOL_SIZE; i++) {
        if (!s_rx_pbuf_used[i]) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) {
        /* Pool exhausted; drop packet to prevent stall and rearm */
        s_rtl8153.rx_errors++;
        rtl8153_arm_rx_buffer(&s_rtl8153);
        spin_unlock_irqrestore(&s_rtl_lock, irq);
        return NULL;
    }

    pbuf_t *packet = &s_rx_packets[free_slot];
    s_rx_pbuf_used[free_slot] = 1;

    pbuf_init(packet);
    memcpy(packet->data, s_rtl8153.rx_buf_virt + sizeof(rtl8153_rx_desc_t), frame_len);
    packet->payload = packet->data;
    packet->length = (uint16_t)frame_len;
    packet->total_len = (uint16_t)frame_len;
    packet->flags = PBUF_FLAG_ALLOCATED;

    s_rtl8153.rx_packets++;
    s_rtl8153.rx_bytes += frame_len;

    /* Re-arm Bulk IN ring for the next packet */
    rtl8153_arm_rx_buffer(&s_rtl8153);

    spin_unlock_irqrestore(&s_rtl_lock, irq);
    return packet;
}

void rtl8153_recycle_rx(net_dev_t *dev, pbuf_t *packet) {
    if (!dev || dev != &s_net_dev || !packet) return;

    uint64_t irq = spin_lock_irqsave(&s_rtl_lock);
    for (int i = 0; i < (int)RTL8153_RX_POOL_SIZE; i++) {
        if (&s_rx_packets[i] == packet) {
            pbuf_reset(packet);
            s_rx_pbuf_used[i] = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&s_rtl_lock, irq);
}

bool rtl8153_service_link(net_dev_t *dev, uint64_t now, uint64_t tick_hz) {
    if (!dev || dev != &s_net_dev || !s_rtl8153.active) return false;

    /* Rate-limit PHY polling to at most once per 200 ms */
    if (tick_hz && now < s_rtl8153.last_link_poll + (tick_hz / 5)) {
        return s_rtl8153.link_online;
    }
    s_rtl8153.last_link_poll = now;

    uint16_t phy_status = rtl8153_read_word(&s_rtl8153, MCU_TYPE_PLA, PLA_PHYSTATUS);
    bool online = (phy_status & RTL8153_PHY_LINK_STATUS) != 0;

    uint64_t irq = spin_lock_irqsave(&s_rtl_lock);
    s_rtl8153.link_online = online;
    if (online) {
        s_net_dev.flags |= NET_RUNNING;
    } else {
        s_net_dev.flags &= ~NET_RUNNING;
    }
    spin_unlock_irqrestore(&s_rtl_lock, irq);

    return online;
}

bool rtl8153_network_online(net_dev_t *dev) {
    if (!dev || dev != &s_net_dev || !s_rtl8153.active) return false;
    return s_rtl8153.link_online && (s_net_dev.flags & NET_UP);
}

uint32_t rtl8153_link_state_abi(const net_dev_t *dev) {
    if (!dev || dev != &s_net_dev || !s_rtl8153.active) {
        return NET_IF_LINK_UNINITIALIZED;
    }
    return s_rtl8153.link_online ? NET_IF_LINK_ONLINE : NET_IF_LINK_DOWN;
}

void rtl8153_get_stats(const net_dev_t *dev, uint64_t *rx_packets, uint64_t *tx_packets) {
    if (!dev || dev != &s_net_dev) {
        if (rx_packets) *rx_packets = 0;
        if (tx_packets) *tx_packets = 0;
        return;
    }
    uint64_t irq = spin_lock_irqsave(&s_rtl_lock);
    if (rx_packets) *rx_packets = s_rtl8153.rx_packets;
    if (tx_packets) *tx_packets = s_rtl8153.tx_packets;
    spin_unlock_irqrestore(&s_rtl_lock, irq);
}

#ifdef RTL8153_HOST_TEST
uint8_t *rtl8153_test_get_rx_buf(void) {
    return s_rtl8153.rx_buf_virt;
}
#endif
