#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xhci_rings.h"

enum mock_fault {
    FAULT_NONE,
    FAULT_START_TIMEOUT,
    FAULT_NO_EVENT,
    FAULT_WRONG_TRB_PTR,
    FAULT_COMPLETION_ERR,
    FAULT_NOT_HALTED,
    FAULT_PORT_STATUS_FIRST
};

typedef struct {
    uint32_t regs[0x4000 / 4];
    enum mock_fault fault;
    unsigned db0_rings;
    xhci_dma_buffers_t dma;
} mock_hw_t;

static uint32_t mock_read32(void *ctx, uint32_t off) {
    mock_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    return m->regs[off / 4];
}

static void mock_write32(void *ctx, uint32_t off, uint32_t val) {
    mock_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    m->regs[off / 4] = val;

    if (off == 0x40) { /* USBCMD */
        if (val & 1) { /* RUN */
            if (m->fault != FAULT_START_TIMEOUT) {
                m->regs[0x44 / 4] &= ~1u; /* HCH = 0 (running) */
            }
        } else { /* STOP */
            m->regs[0x44 / 4] |= 1u; /* HCH = 1 (halted) */
        }
    } else if (off == 0x2000) { /* Doorbell 0 */
        assert(val == 0); /* Host controller command */
        ++m->db0_rings;
        if (m->fault == FAULT_NO_EVENT) return;

        /* Hardware processes Command Ring TRB 0 */
        xhci_trb_t cmd_trb = m->dma.cmd_ring_virt[0];
        assert(((cmd_trb.control >> 10) & 0x3f) == XHCI_TRB_TYPE_NOOP_CMD);
        assert(cmd_trb.control & XHCI_TRB_C);

        if (m->fault == FAULT_PORT_STATUS_FIRST) {
            /* Port status event at index 0 */
            xhci_trb_t *p_event = &m->dma.event_ring_virt[0];
            p_event->parameter_low = 0x5000000;
            p_event->parameter_high = 0;
            p_event->status = 0x1000000;
            p_event->control = (XHCI_TRB_TYPE_PORT_STATUS_EVENT << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;

            /* Command completion event at index 1 */
            xhci_trb_t *c_event = &m->dma.event_ring_virt[1];
            c_event->parameter_low = (uint32_t)m->dma.cmd_ring_phys;
            c_event->parameter_high = (uint32_t)(m->dma.cmd_ring_phys >> 32);
            c_event->status = XHCI_COMP_SUCCESS << 24;
            c_event->control = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;
            return;
        }

        /* Write Command Completion Event to Event Ring */
        xhci_trb_t *event = &m->dma.event_ring_virt[0];
        uint64_t cmd_ptr = (m->fault == FAULT_WRONG_TRB_PTR) ?
                           0xdeadbeefULL : m->dma.cmd_ring_phys;
        uint32_t comp_code = (m->fault == FAULT_COMPLETION_ERR) ?
                             XHCI_COMP_TRB_ERROR : XHCI_COMP_SUCCESS;

        event->parameter_low = (uint32_t)cmd_ptr;
        event->parameter_high = (uint32_t)(cmd_ptr >> 32);
        event->status = comp_code << 24;
        event->control = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;
    }
}

static bool mock_delay_ms(void *ctx) {
    (void)ctx;
    return true;
}

static void init_mock(mock_hw_t *m, enum mock_fault fault) {
    memset(m, 0, sizeof(*m));
    m->fault = fault;
    m->regs[0x00 / 4] = 0x01000040; /* CAPLENGTH = 0x40, Version = 0x0100 */
    m->regs[0x04 / 4] = (4u << 24) | (1u << 8) | 32; /* HCSPARAMS1 */
    m->regs[0x14 / 4] = 0x2000;      /* DBOFF */
    m->regs[0x18 / 4] = 0x1000;      /* RTSOFF */
    m->regs[0x40 / 4] = 0;           /* USBCMD = 0 */
    m->regs[0x44 / 4] = (fault == FAULT_NOT_HALTED) ? 0 : 1u; /* USBSTS.HCH = 1 */
    m->regs[0x48 / 4] = 1;           /* PAGESIZE = 4K */
}

int main(void) {
    mock_hw_t m;
    xhci_trb_t cmd_ring[XHCI_RING_TRB_COUNT];
    xhci_trb_t event_ring[XHCI_RING_TRB_COUNT];
    xhci_erst_entry_t erst[1];
    xhci_dump_record_t dump;

    xhci_dma_buffers_t dma = {
        .cmd_ring_virt = cmd_ring,
        .event_ring_virt = event_ring,
        .erst_virt = erst,
        .cmd_ring_phys = 0x100000,
        .event_ring_phys = 0x101000,
        .erst_phys = 0x102000,
    };

    xhci_rings_io_t io = {
        .mmio_ctx = &m,
        .mmio_size = sizeof(m.regs),
        .read32 = mock_read32,
        .write32 = mock_write32,
        .delay_ms = mock_delay_ms,
    };

    /* 1. Success case */
    init_mock(&m, FAULT_NONE);
    m.dma = dma;
    bool ok = xhci_verify_rings(&io, &dma, &dump);
    assert(ok);
    assert(m.db0_rings == 1);
    assert(dump.completion_code == XHCI_COMP_SUCCESS);
    assert(dump.cmd_enqueue_idx == 1);
    assert(dma.cmd_enqueue_idx == 1);

    /* Verify Link TRB in Command Ring */
    assert(((cmd_ring[XHCI_RING_TRB_COUNT - 1].control >> 10) & 0x3f) == XHCI_TRB_TYPE_LINK);
    assert(cmd_ring[XHCI_RING_TRB_COUNT - 1].control & XHCI_TRB_TC);
    assert(cmd_ring[XHCI_RING_TRB_COUNT - 1].parameter_low == (uint32_t)dma.cmd_ring_phys);

    /* 2. Timeout case (no event posted) */
    init_mock(&m, FAULT_NO_EVENT);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(!ok);
    assert(dump.error_msg != NULL);
    assert(!strcmp(dump.error_msg, "command completion timeout"));

    /* 3. Controller start timeout */
    init_mock(&m, FAULT_START_TIMEOUT);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(!ok);
    assert(!strcmp(dump.error_msg, "controller failed to start (HCH stuck at 1)"));

    /* 4. Wrong TRB pointer returned */
    init_mock(&m, FAULT_WRONG_TRB_PTR);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(!ok);
    assert(!strcmp(dump.error_msg, "mismatched or failed completion event"));

    /* 5. Completion error status code */
    init_mock(&m, FAULT_COMPLETION_ERR);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(!ok);
    assert(dump.completion_code == XHCI_COMP_TRB_ERROR);

    /* 6. Controller not halted prior to ring setup */
    init_mock(&m, FAULT_NOT_HALTED);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(!ok);
    assert(!strcmp(dump.error_msg, "controller not halted prior to ring setup"));

    /* 7. Hardware posts port status change event before command completion event (Dell hardware case) */
    init_mock(&m, FAULT_PORT_STATUS_FIRST);
    m.dma = dma;
    ok = xhci_verify_rings(&io, &dma, &dump);
    assert(ok);
    assert(dump.completion_code == XHCI_COMP_SUCCESS);
    assert(dump.last_completed_trb.parameter_low == (uint32_t)dma.cmd_ring_phys);

    puts("PASS xHCI rings: success path, link TRB, ERST setup, No-Op completion, timeout, start failure, TRB mismatch, error code, unhalted rejection, port status event consumption");
    return 0;
}
