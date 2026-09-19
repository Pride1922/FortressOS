#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xhci_ports.h"

typedef struct {
    uint32_t regs[0x8000 / 4];
    unsigned reset_count;
    unsigned rw1c_cleared;
} mock_ports_hw_t;

static uint32_t mock_read32(void *ctx, uint32_t off) {
    mock_ports_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    return m->regs[off / 4];
}

static void mock_write32(void *ctx, uint32_t off, uint32_t val) {
    mock_ports_hw_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));

    /* Operational port registers start at 0x400 + 0x400 = 0x800 (for cap=0x40) */
    if (off >= 0x440 && off < 0x640) {
        uint32_t old = m->regs[off / 4];
        /* Check write safety: PED (bit 1) should not be written as 1 */
        assert(!(val & XHCI_PORTSC_PED));

        /* If Port Reset written */
        if (val & XHCI_PORTSC_PR) {
            ++m->reset_count;
            /* Reset completes: clear PR, set PED=1, set PRC=1 */
            m->regs[off / 4] = (old & ~XHCI_PORTSC_PR) | XHCI_PORTSC_PED | XHCI_PORTSC_PRC;
            return;
        }

        /* If acknowledging PRC or CSC (RW1C) */
        if (val & (XHCI_PORTSC_PRC | XHCI_PORTSC_CSC)) {
            ++m->rw1c_cleared;
            m->regs[off / 4] = old & ~(val & XHCI_PORTSC_RW1C_MASK);
            return;
        }

        m->regs[off / 4] = val;
    } else {
        m->regs[off / 4] = val;
    }
}

static bool mock_delay_ms(void *ctx) {
    (void)ctx;
    return true;
}

static void setup_mock(mock_ports_hw_t *m) {
    memset(m, 0, sizeof(*m));
    m->regs[0x00 / 4] = 0x01000040; /* CAPLENGTH = 0x40 */
    m->regs[0x04 / 4] = (8u << 24) | (1u << 8) | 32; /* 8 ports */
    m->regs[0x10 / 4] = 0x1000u << 16; /* Extended cap pointer at 0x4000 */
    m->regs[0x18 / 4] = 0x2000; /* RTSOFF */

    /* Ext Cap 1: Supported Protocol USB 2.0 (ports 1..4) at 0x4000 */
    m->regs[0x4000 / 4] = (2u << 24) | (0x04u << 8) | 2u; /* Major=2, Next=4 dwords (0x4010), CapID=2 */
    m->regs[0x4004 / 4] = 0x20425355; /* "USB " */
    m->regs[0x4008 / 4] = (4u << 8) | 1u; /* Count=4, Offset=1 */

    /* Ext Cap 2: Supported Protocol USB 3.0 (ports 5..8) at 0x4010 */
    m->regs[0x4010 / 4] = (3u << 24) | (0u << 8) | 2u; /* Major=3, Next=0 (end), CapID=2 */
    m->regs[0x4014 / 4] = 0x20425355; /* "USB " */
    m->regs[0x4018 / 4] = (4u << 8) | 5u; /* Count=4, Offset=5 */

    /* Ports start at op (0x40) + 0x400 = 0x440 */
    /* Port 1 (USB 2.0): Disconnected */
    m->regs[(0x440 + 0 * 0x10) / 4] = XHCI_PORTSC_PP;

    /* Port 2 (USB 2.0): Connected High-Speed device (CCS=1, PP=1, Speed=3) */
    m->regs[(0x440 + 1 * 0x10) / 4] = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (XHCI_SPEED_HIGH << 10);

    /* Port 5 (USB 3.0): Connected SuperSpeed device (CCS=1, PP=1, Speed=4) */
    m->regs[(0x440 + 4 * 0x10) / 4] = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (XHCI_SPEED_SUPER << 10);
}

int main(void) {
    mock_ports_hw_t m;
    setup_mock(&m);

    xhci_rings_io_t io = {
        .mmio_ctx = &m,
        .mmio_size = sizeof(m.regs),
        .read32 = mock_read32,
        .write32 = mock_write32,
        .delay_ms = mock_delay_ms,
    };

    xhci_port_report_t report;
    bool ok = xhci_discover_and_reset_ports(&io, NULL, &report);
    assert(ok);

    /* Verify port protocol mappings */
    assert(report.total_ports == 8);
    assert(report.usb2_port_count == 4);
    assert(report.usb3_port_count == 4);
    for (int p = 1; p <= 4; ++p) {
        assert(report.ports[p - 1].protocol_major == 2);
    }
    for (int p = 5; p <= 8; ++p) {
        assert(report.ports[p - 1].protocol_major == 3);
    }

    /* Verify connections */
    assert(report.connected_count == 2);

    /* Port 1 was disconnected */
    assert(!report.ports[0].connected);

    /* Port 2 was USB 2.0 connected -> should have been reset */
    assert(report.ports[1].connected);
    assert(report.ports[1].enabled);
    assert(report.ports[1].speed == XHCI_SPEED_HIGH);
    assert(report.selected_usb2_port == 2);
    assert(report.selected_speed == XHCI_SPEED_HIGH);
    assert(m.reset_count == 1);
    assert(m.rw1c_cleared == 1);

    /* Port 5 was SuperSpeed connected -> should NOT have been reset */
    assert(report.ports[4].connected);
    assert(report.ports[4].speed == XHCI_SPEED_SUPER);
    assert(report.ports[4].protocol_major == 3);

    puts("PASS xHCI ports: protocol discovery (USB2/USB3), PORTSC inspection, USB2 port reset, speed negotiation, SuperSpeed isolation, write safety");
    return 0;
}
