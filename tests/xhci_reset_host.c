/* Actual reset state machine, mocked register transport and 1 ms clock. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xhci_reset.h"

enum fault { NONE, OWNERSHIP, HALT, RESET_STUCK, CNR_STUCK, TIMER, GONE, BAD_PAGE };
typedef struct {
    uint32_t regs[0x4000 / 4];
    enum fault fault;
    unsigned writes, reset_writes, waits;
    bool reset_seen;
} mock_t;

static uint32_t read_reg(void *ctx, uint32_t off) {
    mock_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    if (m->fault == GONE && off == 0x44) return UINT32_MAX;
    return m->regs[off / 4];
}
static void write_reg(void *ctx, uint32_t off, uint32_t val) {
    mock_t *m = ctx;
    assert(!(off & 3) && off < sizeof(m->regs));
    ++m->writes;
    if (off == 0x80) {
        assert(val & (1u << 24));
        m->regs[off / 4] = val;
        if (m->fault != OWNERSHIP) m->regs[off / 4] &= ~(1u << 16);
    } else if (off == 0x84) {
        assert(!(val & 0xe011));
        m->regs[off / 4] = val & ~0xe0000000u;
    } else if (off == 0x40) {
        assert(!(m->regs[0x80 / 4] & (1u << 16)));
        assert(!(m->regs[0x44 / 4] & (1u << 11)));
        m->regs[off / 4] = val;
        if (val & 2) {
            assert(m->regs[0x44 / 4] & 1); /* reset only after halt */
            ++m->reset_writes;
            m->reset_seen = true;
            m->regs[0x44 / 4] = 1 | (1u << 11);
        } else if (!(val & 1) && m->fault != HALT) {
            m->regs[0x44 / 4] |= 1;
        }
    } else {
        assert(!"unexpected register write");
    }
}
static bool delay(void *ctx) {
    mock_t *m = ctx;
    ++m->waits;
    assert(m->waits <= 3000);
    if (m->fault == TIMER) return false;
    if (m->reset_seen && m->fault != RESET_STUCK) {
        m->regs[0x40 / 4] = 0;
        if (m->fault != CNR_STUCK) m->regs[0x44 / 4] = 1;
    }
    return true;
}
static void init(mock_t *m, bool legacy, enum fault f) {
    memset(m, 0, sizeof(*m));
    m->fault = f;
    m->regs[0] = 0x01000040;
    m->regs[1] = (4u << 24) | (1u << 8) | 32;
    m->regs[0x10 / 4] = legacy ? (0x20u << 16) : 0;
    m->regs[0x14 / 4] = 0x2000;
    m->regs[0x18 / 4] = 0x1000;
    m->regs[0x40 / 4] = 0xd; /* running, IRQs enabled */
    m->regs[0x48 / 4] = f == BAD_PAGE ? 0 : 1;
    if (legacy) {
        m->regs[0x80 / 4] = 1 | (1u << 16);
        m->regs[0x84 / 4] = 0xe011;
    }
}
static bool run(mock_t *m, xhci_reset_result_t *r) {
    xhci_reset_io_t io = {m, sizeof(m->regs), read_reg, write_reg, delay};
    return xhci_reset_controller(&io, r);
}
int main(void) {
    mock_t m;
    xhci_reset_result_t r;
    for (unsigned legacy = 0; legacy <= 1; ++legacy) {
        init(&m, legacy, NONE);
        assert(run(&m, &r) && !r.error && m.reset_writes == 1);
        assert(r.command == 0 && r.status == 1 && r.pagesize == 1);
    }
    const char *errors[] = {NULL, "BIOS ownership timeout", "halt timeout", "HCRST timeout",
                            "post-reset CNR timeout", "PIT wait failed", "MMIO unavailable",
                            "invalid reset readback"};
    for (enum fault f = OWNERSHIP; f <= BAD_PAGE; ++f) {
        init(&m, true, f);
        assert(!run(&m, &r) && !strcmp(r.error, errors[f]));
        if (f == OWNERSHIP || f == HALT || f == GONE) assert(m.reset_writes == 0);
    }
    init(&m, false, NONE);
    m.regs[0] = UINT32_MAX;
    assert(!run(&m, &r) && m.writes == 0);
    init(&m, false, NONE);
    m.regs[0x14 / 4] = 0x4000;
    assert(!run(&m, &r) && m.writes == 0);
    init(&m, false, NONE);
    m.regs[0x10 / 4] = 0x1000u << 16;
    assert(!run(&m, &r) && m.writes == 0);
    init(&m, true, NONE);
    m.regs[0x80 / 4] |= 1u << 8; /* next overlaps legacy control */
    assert(!run(&m, &r) && m.writes == 0);
    init(&m, true, NONE);
    m.regs[0x80 / 4] |= 2u << 8;
    m.regs[0x88 / 4] = 1; /* duplicate legacy */
    assert(!run(&m, &r) && m.writes == 0);
    init(&m, false, NONE);
    m.regs[0x44 / 4] = 1u << 11;
    assert(!run(&m, &r) && m.writes == 0 && !strcmp(r.error, "initial CNR timeout"));
    puts("PASS xHCI reset: absent/present handoff, bounds, malformed capabilities, ownership/halt/reset/CNR/timeouts, missing MMIO, unsupported page size (mock transport)");
}
