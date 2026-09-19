#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xhci_dev.h"

enum mock_dev_fault {
    DEV_FAULT_NONE,
    DEV_FAULT_ENABLE_SLOT,
    DEV_FAULT_ADDRESS_DEVICE,
    DEV_FAULT_BAD_DESC_HEADER,
    DEV_FAULT_BAD_DEV_DESC,
    DEV_FAULT_NOT_MASS_STORAGE,
    DEV_FAULT_NO_BULK_ENDPOINTS,
    DEV_FAULT_SET_CONFIG
};

typedef struct {
    uint32_t regs[0x8000 / 4];
    enum mock_dev_fault fault;
    xhci_dma_buffers_t ring_dma;
    xhci_dev_dma_t dev_dma;
    unsigned db0_count;
    unsigned db1_count;
} mock_dev_hw_t;

static uint32_t mock_read32(void *ctx, uint32_t off) {
    mock_dev_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    return m->regs[off / 4];
}

/* Sample USB Descriptors */
static const uint8_t s_device_desc[18] = {
    18,                 /* bLength */
    USB_DESC_DEVICE,    /* bDescriptorType */
    0x00, 0x02,         /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,   /* Defined at Interface level */
    64,                 /* bMaxPacketSize0 = 64 */
    0x81, 0x07,         /* idVendor = 0x0781 (SanDisk) */
    0x83, 0x55,         /* idProduct = 0x5583 */
    0x00, 0x01,         /* bcdDevice 1.00 */
    1, 2, 3,            /* Strings */
    1                   /* bNumConfigurations = 1 */
};

/* Config descriptor (32 bytes total): Config(9) + Interface(9) + Endpoint IN(7) + Endpoint OUT(7) */
static const uint8_t s_config_desc[32] = {
    /* Configuration Descriptor (9 bytes) */
    9, USB_DESC_CONFIGURATION, 32, 0, 1, 1, 0, 0x80, 50,
    /* Interface Descriptor: Class 0x08 (Mass Storage), SubClass 0x06 (SCSI), Proto 0x50 (BOT) */
    9, USB_DESC_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    /* Bulk IN Endpoint (7 bytes) */
    7, USB_DESC_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0, /* Addr 0x81, Bulk, Max 512 */
    /* Bulk OUT Endpoint (7 bytes) */
    7, USB_DESC_ENDPOINT, 0x02, 0x02, 0x00, 0x02, 0  /* Addr 0x02, Bulk, Max 512 */
};

static void mock_write32(void *ctx, uint32_t off, uint32_t val) {
    mock_dev_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    m->regs[off / 4] = val;

    if (off == 0x40) { /* USBCMD */
        if (val & 1) {
            m->regs[0x44 / 4] &= ~1u; /* HCH = 0 (running) */
        } else {
            m->regs[0x44 / 4] |= 1u;  /* HCH = 1 (halted) */
        }
    } else if (off == 0x2000) { /* Doorbell 0 (Command Ring) */
        ++m->db0_count;
        /* Find pending command TRB */
        uint32_t cmd_idx = (m->db0_count == 1) ? 0 : (m->db0_count - 1);
        xhci_trb_t cmd_trb = m->ring_dma.cmd_ring_virt[cmd_idx];
        uint32_t trb_type = (cmd_trb.control >> 10) & 0x3f;

        xhci_trb_t *event = &m->ring_dma.event_ring_virt[m->db0_count - 1 + m->db1_count];
        uint64_t cmd_phys = m->ring_dma.cmd_ring_phys + cmd_idx * sizeof(xhci_trb_t);
        event->parameter_low = (uint32_t)cmd_phys;
        event->parameter_high = (uint32_t)(cmd_phys >> 32);

        if (trb_type == 9) { /* Enable Slot */
            if (m->fault == DEV_FAULT_ENABLE_SLOT) {
                event->status = 0x09u << 24; /* No Slots Available */
                event->control = (33u << 10) | 1u;
            } else {
                event->status = 1u << 24; /* Success */
                event->control = (33u << 10) | (1u << 24) /* Slot ID 1 */ | 1u;
            }
        } else if (trb_type == 11) { /* Address Device */
            if (m->fault == DEV_FAULT_ADDRESS_DEVICE) {
                event->status = 0x05u << 24; /* Parameter Error */
                event->control = (33u << 10) | (1u << 24) | 1u;
            } else {
                event->status = 1u << 24; /* Success */
                event->control = (33u << 10) | (1u << 24) | 1u;
            }
        } else if (trb_type == 13) { /* Evaluate Context */
            event->status = 1u << 24;
            event->control = (33u << 10) | (1u << 24) | 1u;
        }
    } else if (off == 0x2004) { /* Doorbell 1 (Slot 1, EP0) */
        ++m->db1_count;
        /* Read Setup packet from EP0 ring */
        xhci_trb_t *setup_trb = &m->dev_dma.ep0_ring_virt[(m->db1_count - 1) * 3];
        usb_setup_pkt_t setup;
        memcpy(&setup, &setup_trb->parameter_low, 8);

        if (setup.bRequest == USB_REQ_GET_DESCRIPTOR) {
            uint8_t desc_type = setup.wValue >> 8;
            if (desc_type == USB_DESC_DEVICE) {
                if (m->fault == DEV_FAULT_BAD_DESC_HEADER) {
                    memset(m->dev_dma.bounce_buf_virt, 0, setup.wLength);
                } else if (m->fault == DEV_FAULT_BAD_DEV_DESC && setup.wLength == 18) {
                    memset(m->dev_dma.bounce_buf_virt, 0xff, 18);
                } else {
                    memcpy(m->dev_dma.bounce_buf_virt, s_device_desc, setup.wLength);
                }
            } else if (desc_type == USB_DESC_CONFIGURATION) {
                uint16_t clen = setup.wLength < 32 ? setup.wLength : 32;
                if (m->fault == DEV_FAULT_NOT_MASS_STORAGE) {
                    uint8_t bad_cfg[32];
                    memcpy(bad_cfg, s_config_desc, 32);
                    bad_cfg[14] = 0x03; /* Change Class to HID */
                    memcpy(m->dev_dma.bounce_buf_virt, bad_cfg, clen);
                } else if (m->fault == DEV_FAULT_NO_BULK_ENDPOINTS) {
                    uint8_t bad_cfg[32];
                    memcpy(bad_cfg, s_config_desc, 32);
                    bad_cfg[20] = 0x03; /* Interrupt instead of Bulk */
                    bad_cfg[27] = 0x03; /* Interrupt instead of Bulk */
                    memcpy(m->dev_dma.bounce_buf_virt, bad_cfg, clen);
                } else {
                    memcpy(m->dev_dma.bounce_buf_virt, s_config_desc, clen);
                }
            }
        }

        /* Verify Setup Stage TRB bit layout according to xHCI spec */
        assert(((setup_trb->control >> 10) & 0x3f) == 2); /* Type 2: Setup Stage */
        assert(setup_trb->control & (1u << 6));           /* Bit 6: IDT */
        assert(!(setup_trb->control & (1u << 5)));        /* Bit 5: IOC must NOT be set on Setup Stage */
        assert(!(setup_trb->control & (1u << 4)));        /* Bit 4: Chain must NOT be set on Setup Stage */
        uint32_t expected_trt = (setup.wLength > 0) ? 3 : 0;
        assert(((setup_trb->control >> 16) & 3) == expected_trt); /* Bits 17:16: TRT */

        /* Post Transfer Event to Event Ring */
        xhci_trb_t *event = &m->ring_dma.event_ring_virt[m->db0_count + m->db1_count - 1];
        uint32_t num_trbs = (setup.wLength > 0) ? 3 : 2;
        uint32_t status_idx = (m->db1_count - 1) * 3 + (num_trbs - 1);
        uint64_t status_phys = m->dev_dma.ep0_ring_phys + status_idx * sizeof(xhci_trb_t);
        event->parameter_low = (uint32_t)status_phys;
        event->parameter_high = (uint32_t)(status_phys >> 32);

        if (m->fault == DEV_FAULT_SET_CONFIG && setup.bRequest == USB_REQ_SET_CONFIGURATION) {
            event->status = 0x04u << 24; /* Transaction Error */
        } else {
            event->status = 1u << 24; /* Success */
        }
        event->control = (32u << 10) | (1u << 24) /* Slot ID 1 */ | 1u;
    }
}

static bool mock_delay_ms(void *ctx) {
    (void)ctx;
    return true;
}

static void setup_mock(mock_dev_hw_t *m, enum mock_dev_fault fault) {
    memset(m, 0, sizeof(*m));
    m->fault = fault;
    m->regs[0x00 / 4] = 0x01000040; /* CAPLENGTH = 0x40 */
    m->regs[0x04 / 4] = (8u << 24) | (1u << 8) | 32; /* HCSPARAMS1: 32 slots */
    m->regs[0x08 / 4] = 0;           /* HCSPARAMS2: 0 scratchpad */
    m->regs[0x10 / 4] = 0;           /* HCCPARAMS1: CSZ = 0 (32-byte contexts) */
    m->regs[0x14 / 4] = 0x2000;      /* DBOFF */
    m->regs[0x18 / 4] = 0x1000;      /* RTSOFF */
    m->regs[0x40 / 4] = 0;           /* USBCMD = 0 */
    m->regs[0x44 / 4] = 1u;          /* USBSTS.HCH = 1 (halted) */

    /* Allocate DMA buffers */
    static xhci_trb_t s_cmd_ring[256] __attribute__((aligned(4096)));
    static xhci_trb_t s_event_ring[256] __attribute__((aligned(4096)));
    static xhci_erst_entry_t s_erst[1] __attribute__((aligned(4096)));

    static uint64_t s_dcbaa[64] __attribute__((aligned(4096)));
    static uint32_t s_input_ctx[1024] __attribute__((aligned(4096)));
    static uint32_t s_output_ctx[1024] __attribute__((aligned(4096)));
    static xhci_trb_t s_ep0_ring[256] __attribute__((aligned(4096)));
    static uint8_t  s_bounce_buf[4096] __attribute__((aligned(4096)));

    memset(s_cmd_ring, 0, sizeof(s_cmd_ring));
    memset(s_event_ring, 0, sizeof(s_event_ring));
    memset(s_erst, 0, sizeof(s_erst));
    memset(s_dcbaa, 0, sizeof(s_dcbaa));
    memset(s_input_ctx, 0, sizeof(s_input_ctx));
    memset(s_output_ctx, 0, sizeof(s_output_ctx));
    memset(s_ep0_ring, 0, sizeof(s_ep0_ring));
    memset(s_bounce_buf, 0, sizeof(s_bounce_buf));

    m->ring_dma.cmd_ring_virt = s_cmd_ring;
    m->ring_dma.cmd_ring_phys = 0x10000;
    m->ring_dma.event_ring_virt = s_event_ring;
    m->ring_dma.event_ring_phys = 0x20000;
    m->ring_dma.erst_virt = s_erst;
    m->ring_dma.erst_phys = 0x30000;
    m->ring_dma.cmd_enqueue_idx = 0;
    m->ring_dma.cmd_cycle = 1;
    m->ring_dma.event_dequeue_idx = 0;
    m->ring_dma.event_cycle = 1;

    m->dev_dma.dcbaa_virt = s_dcbaa;
    m->dev_dma.dcbaa_phys = 0x40000;
    m->dev_dma.input_ctx_virt = s_input_ctx;
    m->dev_dma.input_ctx_phys = 0x50000;
    m->dev_dma.output_ctx_virt = s_output_ctx;
    m->dev_dma.output_ctx_phys = 0x60000;
    m->dev_dma.ep0_ring_virt = s_ep0_ring;
    m->dev_dma.ep0_ring_phys = 0x70000;
    m->dev_dma.bounce_buf_virt = s_bounce_buf;
    m->dev_dma.bounce_buf_phys = 0x80000;
}

int main(void) {
    mock_dev_hw_t m;
    xhci_rings_io_t io = {
        .mmio_ctx = &m,
        .mmio_size = sizeof(m.regs),
        .read32 = mock_read32,
        .write32 = mock_write32,
        .delay_ms = mock_delay_ms,
    };
    xhci_bot_device_t dev;

    /* 1. Normal success path */
    setup_mock(&m, DEV_FAULT_NONE);
    bool ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(ok);
    assert(dev.is_valid_bot_storage);
    assert(dev.slot_id == 1);
    assert(dev.vendor_id == 0x0781);
    assert(dev.product_id == 0x5583);
    assert(dev.ep0_max_packet == 64);
    assert(dev.bulk_in_ep == 0x81);
    assert(dev.bulk_in_max_packet == 512);
    assert(dev.bulk_out_ep == 0x02);
    assert(dev.bulk_out_max_packet == 512);
    printf("PASS: xHCI 9G.1e host enumeration and descriptor validation\n");

    /* 2. Enable slot fault */
    setup_mock(&m, DEV_FAULT_ENABLE_SLOT);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Enable Slot failure handled cleanly\n");

    /* 3. Address device fault */
    setup_mock(&m, DEV_FAULT_ADDRESS_DEVICE);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Address Device failure handled cleanly\n");

    /* 4. Bad descriptor header */
    setup_mock(&m, DEV_FAULT_BAD_DESC_HEADER);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Bad descriptor header rejected cleanly\n");

    /* 5. Malformed device descriptor */
    setup_mock(&m, DEV_FAULT_BAD_DEV_DESC);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Malformed device descriptor rejected cleanly\n");

    /* 6. Non-mass-storage class device */
    setup_mock(&m, DEV_FAULT_NOT_MASS_STORAGE);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Non-mass-storage device rejected cleanly\n");

    /* 7. Missing bulk endpoints */
    setup_mock(&m, DEV_FAULT_NO_BULK_ENDPOINTS);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e Device without bulk endpoints rejected cleanly\n");

    /* 8. SET_CONFIGURATION failure */
    setup_mock(&m, DEV_FAULT_SET_CONFIG);
    ok = xhci_enumerate_device(&io, &m.ring_dma, &m.dev_dma, 2, XHCI_SPEED_HIGH, &dev);
    assert(!ok);
    assert(!dev.is_valid_bot_storage);
    printf("PASS: xHCI 9G.1e SET_CONFIGURATION failure handled cleanly\n");

    printf("ALL 9G.1e HOST UNIT TESTS PASSED!\n");
    return 0;
}
