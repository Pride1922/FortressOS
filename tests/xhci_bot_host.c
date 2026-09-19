#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xhci_bot.h"

enum mock_bot_fault {
    BOT_FAULT_NONE,
    BOT_FAULT_CONFIG_EP,
    BOT_FAULT_CBW_FAIL,
    BOT_FAULT_DATA_FAIL,
    BOT_FAULT_CSW_FAIL,
    BOT_FAULT_CSW_BAD_SIG,
    BOT_FAULT_CSW_TAG_MISMATCH,
    BOT_FAULT_CSW_STATUS_FAIL,
    BOT_FAULT_TIMEOUT,
    BOT_FAULT_SHORT_CBW,
    BOT_FAULT_SHORT_DATA,
    BOT_FAULT_SHORT_CSW,
    BOT_FAULT_CSW_RESIDUE,
    BOT_FAULT_PHASE_ERROR,
    BOT_FAULT_WRONG_TRB
};

typedef struct {
    uint32_t regs[0x8000 / 4];
    enum mock_bot_fault fault;
    xhci_dma_buffers_t ring_dma;
    xhci_dev_dma_t dev_dma;
    xhci_bot_rings_t bot_rings;
    uint8_t bounce[4096];
    uint8_t bounce_guard[128];
    uint8_t bulk_in_ring[4096];
    uint8_t bulk_out_ring[4096];

    /* Emulated SCSI disk */
    uint32_t disk_sectors;
    uint32_t disk_sector_size;
    uint8_t  disk_data[16 * 4096];
    uint8_t  tur_count;
    unsigned flush_count, sense_count, write_count;
    bool reject_flush, unit_attention_once, repeated_unit_attention;
    bool sense_fails, invalid_sense, descriptor_sense, deferred_sense;
    bool sense_pending;
    usb_bot_cbw_t last_cbw;
} mock_bot_hw_t;

static uint32_t mock_read32(void *ctx, uint32_t off) {
    mock_bot_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    return m->regs[off / 4];
}

static inline uint32_t bswap32(uint32_t val) {
    return ((val >> 24) & 0x000000ffu) |
           ((val >> 8)  & 0x0000ff00u) |
           ((val << 8)  & 0x00ff0000u) |
           ((val << 24) & 0xff000000u);
}

static void mock_write32(void *ctx, uint32_t off, uint32_t val) {
    mock_bot_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    m->regs[off / 4] = val;

    uint32_t dboff = m->regs[0x14 / 4];
    /* Command ring doorbell */
    if (off == dboff + 0) {
        uint32_t enq = (m->ring_dma.cmd_enqueue_idx > 0) ? m->ring_dma.cmd_enqueue_idx - 1 : XHCI_RING_TRB_COUNT - 2;
        xhci_trb_t cmd = m->ring_dma.cmd_ring_virt[enq];
        uint32_t type = (cmd.control >> 10) & 0x3f;

        xhci_trb_t ev = {0};
        ev.parameter_low = (uint32_t)(uintptr_t)&m->ring_dma.cmd_ring_virt[enq];
        ev.control = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << 10) | (m->ring_dma.event_cycle ? 1u : 0);

        if (type == XHCI_TRB_TYPE_CONFIG_EP_CMD) {
            if (m->fault == BOT_FAULT_CONFIG_EP) {
                ev.status = (XHCI_COMP_RESOURCE_ERROR << 24);
            } else {
                ev.status = (XHCI_COMP_SUCCESS << 24);
            }
        }
        m->ring_dma.event_ring_virt[m->ring_dma.event_dequeue_idx] = ev;
    }

    /* Bulk OUT doorbell (Target 4 = DCI 4) */
    if (off == dboff + (m->bot_rings.slot_id * 4) && val == m->bot_rings.out_dci) {
        if (m->fault == BOT_FAULT_TIMEOUT) return;
        uint32_t enq = (m->bot_rings.out_idx > 0) ? m->bot_rings.out_idx - 1 : XHCI_RING_TRB_COUNT - 2;
        xhci_trb_t trb = m->bot_rings.bulk_out_ring_virt[enq];
        uint32_t len = trb.status & 0x1ffff;

        xhci_trb_t ev = {0};
        ev.control = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) |
                     ((uint32_t)m->bot_rings.slot_id << 24) |
                     ((uint32_t)m->bot_rings.out_dci << 16) |
                     (m->ring_dma.event_cycle ? 1u : 0);
        uintptr_t completed = m->bot_rings.bulk_out_ring_phys + enq * sizeof(xhci_trb_t);
        if (m->fault == BOT_FAULT_WRONG_TRB) completed += sizeof(xhci_trb_t);
        ev.parameter_low = (uint32_t)completed;
        ev.parameter_high = (uint32_t)(completed >> 32);

        if (m->fault == BOT_FAULT_CBW_FAIL) {
            ev.status = (XHCI_COMP_STALL_ERROR << 24);
        } else {
            ev.status = (XHCI_COMP_SUCCESS << 24);
            if (len == sizeof(usb_bot_cbw_t)) {
                memcpy(&m->last_cbw, m->dev_dma.bounce_buf_virt, sizeof(usb_bot_cbw_t));
                if (m->last_cbw.CBWCB[0] == SCSI_CMD_TEST_UNIT_READY) {
                    m->tur_count++;
                }
                if (m->last_cbw.CBWCB[0] == SCSI_CMD_SYNCHRONIZE_CACHE_10) {
                    m->flush_count++;
                    assert(m->last_cbw.bCBWCBLength == 10);
                    assert(m->last_cbw.dCBWDataTransferLength == 0);
                    for (unsigned i = 1; i < 10; ++i) assert(m->last_cbw.CBWCB[i] == 0);
                }
                if (m->last_cbw.CBWCB[0] == SCSI_CMD_REQUEST_SENSE) m->sense_count++;
            } else if (m->last_cbw.CBWCB[0] == SCSI_CMD_WRITE_10) {
                uint32_t lba = ((uint32_t)m->last_cbw.CBWCB[2] << 24) |
                               ((uint32_t)m->last_cbw.CBWCB[3] << 16) |
                               ((uint32_t)m->last_cbw.CBWCB[4] << 8) | m->last_cbw.CBWCB[5];
                assert(lba < 16 && len == m->disk_sector_size);
                assert(m->last_cbw.bCBWCBLength == 10 && m->last_cbw.CBWCB[8] == 1);
                memcpy(&m->disk_data[lba * m->disk_sector_size], m->bounce, len);
                m->write_count++;
            }
        }
        if (m->fault == BOT_FAULT_SHORT_CBW && len == sizeof(usb_bot_cbw_t))
            ev.status = (XHCI_COMP_SHORT_PACKET << 24) | 1;
        m->ring_dma.event_ring_virt[m->ring_dma.event_dequeue_idx] = ev;
    }

    /* Bulk IN doorbell (Target 3 = DCI 3) */
    if (off == dboff + (m->bot_rings.slot_id * 4) && val == m->bot_rings.in_dci) {
        uint32_t in_enq = (m->bot_rings.in_idx > 0) ? m->bot_rings.in_idx - 1 : XHCI_RING_TRB_COUNT - 2;
        uint32_t in_len = m->bot_rings.bulk_in_ring_virt[in_enq].status & 0x1ffff;

        xhci_trb_t ev = {0};
        ev.control = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) |
                     ((uint32_t)m->bot_rings.slot_id << 24) |
                     ((uint32_t)m->bot_rings.in_dci << 16) |
                     (m->ring_dma.event_cycle ? 1u : 0);
        uintptr_t completed = m->bot_rings.bulk_in_ring_phys + in_enq * sizeof(xhci_trb_t);
        ev.parameter_low = (uint32_t)completed;
        ev.parameter_high = (uint32_t)(completed >> 32);

        if (m->fault == BOT_FAULT_DATA_FAIL || m->fault == BOT_FAULT_CSW_FAIL) {
            ev.status = (XHCI_COMP_STALL_ERROR << 24);
        } else {
            ev.status = (XHCI_COMP_SUCCESS << 24);
            if (in_len == sizeof(usb_bot_csw_t)) {
                /* CSW stage */
                usb_bot_csw_t csw = {0};
                csw.dCSWSignature = (m->fault == BOT_FAULT_CSW_BAD_SIG) ? 0xDEADBEEF : USB_BOT_CSW_SIGNATURE;
                csw.dCSWTag = (m->fault == BOT_FAULT_CSW_TAG_MISMATCH) ? (m->last_cbw.dCBWTag ^ 0xFF) : m->last_cbw.dCBWTag;
                csw.bCSWStatus = (m->fault == BOT_FAULT_CSW_STATUS_FAIL) ? USB_BOT_CSW_STATUS_FAILED : USB_BOT_CSW_STATUS_PASSED;
                if (m->last_cbw.CBWCB[0] == SCSI_CMD_TEST_UNIT_READY && m->tur_count == 1) {
                    csw.bCSWStatus = USB_BOT_CSW_STATUS_FAILED;
                }
                uint8_t opcode = m->last_cbw.CBWCB[0];
                if (opcode == SCSI_CMD_SYNCHRONIZE_CACHE_10 &&
                    (m->reject_flush || m->repeated_unit_attention ||
                     (m->unit_attention_once && m->flush_count == 1))) {
                    csw.bCSWStatus = USB_BOT_CSW_STATUS_FAILED;
                    m->sense_pending = true;
                }
                if (opcode == SCSI_CMD_REQUEST_SENSE) {
                    if (m->sense_fails) csw.bCSWStatus = USB_BOT_CSW_STATUS_FAILED;
                    else m->sense_pending = false;
                    if (m->descriptor_sense) csw.dCSWDataResidue = 10;
                }
                if (opcode == SCSI_CMD_READ_10 && m->sense_pending)
                    csw.bCSWStatus = USB_BOT_CSW_STATUS_FAILED;
                if (m->fault == BOT_FAULT_CSW_RESIDUE) csw.dCSWDataResidue = 1;
                if (m->fault == BOT_FAULT_PHASE_ERROR) csw.bCSWStatus = USB_BOT_CSW_STATUS_PHASE_ERROR;
                if (m->fault == BOT_FAULT_SHORT_CSW) ev.status = (XHCI_COMP_SHORT_PACKET << 24) | 1;
                memcpy(m->dev_dma.bounce_buf_virt + 64, &csw, sizeof(csw));
            } else {
                /* Data IN stage */
                uint8_t opcode = m->last_cbw.CBWCB[0];
                if (opcode == SCSI_CMD_INQUIRY) {
                    scsi_inquiry_data_t inq = {0};
                    inq.pdt = 0; /* Direct access disk */
                    inq.removable = 0x80;
                    memcpy(inq.vendor, "FORTRESS", 8);
                    memcpy(inq.product, "VIRTUAL DISK    ", 16);
                    memcpy(inq.revision, "1.00", 4);
                    memcpy(m->dev_dma.bounce_buf_virt, &inq, sizeof(inq));
                } else if (opcode == SCSI_CMD_REQUEST_SENSE) {
                    scsi_sense_data_t sense = {0};
                    sense.response_code = 0x70;
                    sense.additional_sense_len = 10;
                    sense.sense_key = 0x06; /* Unit Attention */
                    if (m->reject_flush) { sense.sense_key = 5; sense.asc = 0x20; }
                    if (m->invalid_sense) sense.additional_sense_len = 0;
                    if (m->deferred_sense) sense.response_code = 0x71;
                    memcpy(m->dev_dma.bounce_buf_virt, &sense, sizeof(sense));
                    if (m->descriptor_sense) {
                        uint8_t descriptor[8] = {0x72, sense.sense_key, sense.asc, sense.ascq, 0, 0, 0, 0};
                        memcpy(m->bounce, descriptor, sizeof(descriptor));
                        ev.status = (XHCI_COMP_SHORT_PACKET << 24) | 10;
                    }
                } else if (opcode == SCSI_CMD_READ_CAPACITY_10) {
                    scsi_read_capacity_data_t cap = {0};
                    cap.last_lba_be = bswap32(m->disk_sectors - 1);
                    cap.block_size_be = bswap32(m->disk_sector_size);
                    memcpy(m->dev_dma.bounce_buf_virt, &cap, sizeof(cap));
                } else if (opcode == SCSI_CMD_READ_10) {
                    uint32_t lba = ((uint32_t)m->last_cbw.CBWCB[2] << 24) |
                                   ((uint32_t)m->last_cbw.CBWCB[3] << 16) |
                                   ((uint32_t)m->last_cbw.CBWCB[4] << 8) |
                                   ((uint32_t)m->last_cbw.CBWCB[5]);
                    if (lba < 16) {
                        memcpy(m->dev_dma.bounce_buf_virt, &m->disk_data[lba * m->disk_sector_size], m->disk_sector_size);
                    }
                }
                if (m->fault == BOT_FAULT_SHORT_DATA) ev.status = (XHCI_COMP_SHORT_PACKET << 24) | 1;
            }
        }
        m->ring_dma.event_ring_virt[m->ring_dma.event_dequeue_idx] = ev;
    }
}

static bool mock_delay(void *ctx) {
    (void)ctx;
    return true;
}

static void init_mock_hw(mock_bot_hw_t *m, enum mock_bot_fault fault) {
    memset(m, 0, sizeof(*m));
    m->fault = fault;
    memset(m->bounce_guard, 0xa5, sizeof(m->bounce_guard));
    m->regs[0x10 / 4] = (1 << 2); /* 64-byte contexts */
    m->regs[0x14 / 4] = 0x2000;   /* DBOFF */
    m->regs[0x18 / 4] = 0x1000;   /* RTSOFF */

    static xhci_trb_t cmd_ring[256];
    static xhci_trb_t event_ring[256];
    static uint32_t input_ctx[1024];

    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(input_ctx, 0, sizeof(input_ctx));

    m->ring_dma.cmd_ring_virt = cmd_ring;
    m->ring_dma.cmd_cycle = 1;
    m->ring_dma.event_ring_virt = event_ring;
    m->ring_dma.event_cycle = 1;

    m->dev_dma.input_ctx_virt = input_ctx;
    m->dev_dma.bounce_buf_virt = m->bounce;
    m->dev_dma.bounce_buf_phys = (uintptr_t)m->bounce;

    m->bot_rings.bulk_in_ring_virt = (xhci_trb_t *)m->bulk_in_ring;
    m->bot_rings.bulk_in_ring_phys = (uintptr_t)m->bulk_in_ring;
    m->bot_rings.bulk_out_ring_virt = (xhci_trb_t *)m->bulk_out_ring;
    m->bot_rings.bulk_out_ring_phys = (uintptr_t)m->bulk_out_ring;

    m->disk_sectors = 2048; /* 1 MiB disk */
    m->disk_sector_size = 512;
    for (int i = 0; i < 16; i++) {
        memset(&m->disk_data[i * 512], 0x40 + i, 512);
    }
}

static void test_flush_and_writes(void) {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = {.mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay};
        m.bot_rings.sector_count = 2048;
        m.bot_rings.sector_size = 512;
        xhci_bot_device_t dev = {.slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02};
        assert(xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings));
        m.reject_flush = scenario == 1 || scenario == 3 || scenario == 4 || scenario == 5;
        m.unit_attention_once = scenario == 2 || scenario == 7;
        m.sense_fails = scenario == 3;
        m.invalid_sense = scenario == 4;
        m.descriptor_sense = scenario == 5;
        m.repeated_unit_attention = scenario == 6;
        m.deferred_sense = scenario == 7;
        bool ok = xhci_scsi_sync_cache(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings);
        assert(ok == (scenario == 0 || scenario == 2));
        assert(m.flush_count == ((scenario == 2 || scenario == 6) ? 2u : 1u));
        assert(m.sense_count == ((scenario == 0) ? 0u : (scenario == 6 ? 2u : 1u)));
        assert(m.write_count == 0);
        if (scenario == 1 || scenario == 5) {
            assert(m.bot_rings.last_error.sense_valid);
            assert(m.bot_rings.last_error.opcode == SCSI_CMD_SYNCHRONIZE_CACHE_10);
            assert(m.bot_rings.last_error.sense_key == 5 && m.bot_rings.last_error.asc == 0x20);
            uint8_t data[512];
            assert(xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 0, data));
            assert(data[0] == 0x40); /* Sense consumed; RO fallback can still read. */
        }
        if (scenario == 3 || scenario == 4) assert(!m.bot_rings.last_error.sense_valid);
    }
    printf("PASS: flush success, unsupported opcode, bounded Unit Attention retry, invalid/failed/deferred/descriptor sense\n");

    for (unsigned size = 512; size <= 4096; size *= 8) {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = {.mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay};
        xhci_bot_device_t dev = {.slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02};
        assert(xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings));
        m.bot_rings.sector_count = 16;
        m.bot_rings.sector_size = m.disk_sector_size = size;
        uint8_t written[4096], readback[4096];
        for (unsigned i = 0; i < size; ++i) written[i] = (uint8_t)(i * 17 + 5);
        for (unsigned lba = 0; lba <= 15; lba += 15) {
            assert(xhci_scsi_write_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, lba, written));
            assert(xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, lba, readback));
            assert(memcmp(written, readback, size) == 0);
        }
        assert(!xhci_scsi_write_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 16, written));
        assert(m.write_count == 2);
        for (unsigned i = 0; i < sizeof(m.bounce_guard); ++i) assert(m.bounce_guard[i] == 0xa5);
    }
    printf("PASS: 512/4096-byte writes, full readback, first/last/out-of-range and bounce guard\n");

    enum mock_bot_fault faults[] = {BOT_FAULT_TIMEOUT, BOT_FAULT_CBW_FAIL,
        BOT_FAULT_SHORT_CBW, BOT_FAULT_SHORT_DATA, BOT_FAULT_SHORT_CSW,
        BOT_FAULT_CSW_RESIDUE, BOT_FAULT_PHASE_ERROR, BOT_FAULT_WRONG_TRB};
    for (unsigned i = 0; i < sizeof(faults)/sizeof(faults[0]); ++i) {
        mock_bot_hw_t m;
        init_mock_hw(&m, faults[i]);
        xhci_rings_io_t io = {.mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay};
        xhci_bot_device_t dev = {.slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02};
        assert(xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings));
        m.bot_rings.sector_count = 2048;
        m.bot_rings.sector_size = 512;
        uint8_t data[512];
        assert(!xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 0, data));
        if (faults[i] != BOT_FAULT_SHORT_DATA && faults[i] != BOT_FAULT_CSW_RESIDUE) {
            assert(m.bot_rings.transport_failed);
            uint32_t tag = m.bot_rings.tag;
            m.fault = BOT_FAULT_NONE;
            assert(!xhci_scsi_sync_cache(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings));
            assert(m.bot_rings.tag == tag); /* No DMA buffer/ring reuse or sense attempt. */
        }
    }
    printf("PASS: short transfers/residue rejected and uncertain transport never reused\n");
}

int main(void) {
    printf("Running Phase 9G.2 host unit tests...\n");

    /* Test 1: Configure Bulk Endpoints */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };

        xhci_bot_device_t dev = {
            .slot_id = 3,
            .port_num = 9,
            .speed = XHCI_SPEED_HIGH,
            .bulk_in_ep = 0x81,
            .bulk_in_max_packet = 512,
            .bulk_out_ep = 0x02,
            .bulk_out_max_packet = 512
        };

        bool ok = xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);
        assert(ok);
        assert(m.bot_rings.slot_id == 3);
        assert(m.bot_rings.in_dci == 3);
        assert(m.bot_rings.out_dci == 4);
        printf("PASS: xHCI 9G.2 Configure Bulk Endpoints\n");
    }

    /* Test 2: SCSI INQUIRY */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        scsi_inquiry_data_t inq = {0};
        bool ok = xhci_scsi_inquiry(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &inq);
        assert(ok);
        assert(inq.pdt == 0);
        assert(strcmp(m.bot_rings.vendor, "FORTRESS") == 0);
        assert(strncmp(m.bot_rings.product, "VIRTUAL DISK", 12) == 0);
        printf("PASS: xHCI 9G.2 SCSI INQUIRY\n");
    }

    /* Test 3: SCSI TEST UNIT READY with Unit Attention sense recovery */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        bool ok = xhci_scsi_test_unit_ready(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings);
        assert(ok);
        assert(m.tur_count == 2); /* Retried after clearing Unit Attention */
        printf("PASS: xHCI 9G.2 SCSI TEST UNIT READY with sense recovery\n");
    }

    /* Test 4: SCSI READ CAPACITY (10) */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        uint64_t sectors = 0;
        uint32_t sector_size = 0;
        bool ok = xhci_scsi_read_capacity(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &sectors, &sector_size);
        assert(ok);
        assert(sectors == 2048);
        assert(sector_size == 512);
        printf("PASS: xHCI 9G.2 SCSI READ CAPACITY (10)\n");
    }

    /* Test 5: SCSI READ (10) sector read & bounds checks */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_NONE);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        uint64_t sectors = 0;
        uint32_t sector_size = 0;
        xhci_scsi_read_capacity(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &sectors, &sector_size);

        uint8_t sector_buf[512];
        /* Read sector 0 */
        bool ok = xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 0, sector_buf);
        assert(ok);
        assert(sector_buf[0] == 0x40);

        /* Read sector 1 */
        ok = xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 1, sector_buf);
        assert(ok);
        assert(sector_buf[0] == 0x41);

        /* Out of bounds LBA */
        ok = xhci_scsi_read_sector(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, 2048, sector_buf);
        assert(!ok);

        printf("PASS: xHCI 9G.2 SCSI READ (10) sector reads and bounds check\n");
    }

    /* Test 6: Fault injection - CSW Bad Signature */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_CSW_BAD_SIG);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        scsi_inquiry_data_t inq = {0};
        bool ok = xhci_scsi_inquiry(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &inq);
        assert(!ok);
        printf("PASS: xHCI 9G.2 CSW Bad Signature rejected cleanly\n");
    }

    /* Test 7: Fault injection - CSW Tag Mismatch */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_CSW_TAG_MISMATCH);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        scsi_inquiry_data_t inq = {0};
        bool ok = xhci_scsi_inquiry(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &inq);
        assert(!ok);
        printf("PASS: xHCI 9G.2 CSW Tag Mismatch rejected cleanly\n");
    }

    /* Test 8: Fault injection - CSW Status Failed */
    {
        mock_bot_hw_t m;
        init_mock_hw(&m, BOT_FAULT_CSW_STATUS_FAIL);
        xhci_rings_io_t io = { .mmio_ctx = &m, .read32 = mock_read32, .write32 = mock_write32, .delay_ms = mock_delay };
        xhci_bot_device_t dev = { .slot_id = 3, .bulk_in_ep = 0x81, .bulk_out_ep = 0x02 };
        xhci_configure_bulk_endpoints(&io, &m.ring_dma, &m.dev_dma, &dev, &m.bot_rings);

        scsi_inquiry_data_t inq = {0};
        bool ok = xhci_scsi_inquiry(&io, &m.ring_dma, &m.dev_dma, &m.bot_rings, &inq);
        assert(!ok);
        printf("PASS: xHCI 9G.2 CSW Status Failed rejected cleanly\n");
    }

    test_flush_and_writes();
    test_durability_and_flush_barrier();
    printf("ALL BOT/SCSI HOST UNIT TESTS PASSED!\n");
    return 0;
}
