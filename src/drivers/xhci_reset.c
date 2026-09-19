#include "xhci_reset.h"

#define RUN       (1u << 0)
#define RESET     (1u << 1)
#define INTE      (1u << 2)
#define HSEE      (1u << 3)
#define HALTED    (1u << 0)
#define CNR       (1u << 11)
#define BIOS_OWN  (1u << 16)
#define OS_OWN    (1u << 24)
#define SMI_ENABLE 0x0000e011u
#define SMI_W1C    0xe0000000u

static bool fits(const xhci_reset_io_t *io, uint32_t off, uint32_t len) {
    return !(off & 3) && off <= io->size && len <= io->size - off;
}

static bool fail(xhci_reset_result_t *r, const char *why, uint32_t off, uint32_t val) {
    r->error = why;
    r->failed_offset = off;
    r->last_value = val;
    return false;
}

static bool poll(const xhci_reset_io_t *io, xhci_reset_result_t *r,
                 uint32_t off, uint32_t mask, uint32_t wanted, unsigned ms,
                 const char *why) {
    for (unsigned i = 0; i <= ms; ++i) {
        uint32_t value = io->read(io->context, off);
        if (value == UINT32_MAX) return fail(r, "MMIO unavailable", off, value);
        if ((value & mask) == wanted) return true;
        if (i == ms) return fail(r, why, off, value);
        if (!io->delay_ms(io->context)) return fail(r, "PIT wait failed", off, value);
    }
    return false;
}

bool xhci_reset_controller(const xhci_reset_io_t *io, xhci_reset_result_t *r) {
    *r = (xhci_reset_result_t){0};
    if (!fits(io, 0, 0x20)) return fail(r, "short capability aperture", 0, io->size);
    r->cap = io->read(io->context, 0);
    uint32_t op = r->cap & 0xff;
    uint32_t version = r->cap >> 16;
    if (r->cap == UINT32_MAX || op < 0x20 || !fits(io, op, 0x400) ||
        version < 0x0090 || version > 0x0120)
        return fail(r, "unsupported capability header", 0, r->cap);
    r->hcs1 = io->read(io->context, 4);
    r->hcs2 = io->read(io->context, 8);
    r->hcc1 = io->read(io->context, 0x10);
    r->dboff = io->read(io->context, 0x14);
    r->rtsoff = io->read(io->context, 0x18);
    uint32_t slots = r->hcs1 & 0xff;
    uint32_t ports = r->hcs1 >> 24;
    uint32_t intrs = (r->hcs1 >> 8) & 0x7ff;
    if (!slots || !ports || !intrs || r->hcs1 == UINT32_MAX ||
        r->hcs2 == UINT32_MAX || r->hcc1 == UINT32_MAX ||
        !fits(io, op + 0x400, ports * 16) ||
        (r->dboff & 3) || r->dboff < op + 0x400 + ports * 16 ||
        !fits(io, r->dboff, (slots + 1) * 4) ||
        (r->rtsoff & 31) || r->rtsoff < op + 0x400 + ports * 16 ||
        !fits(io, r->rtsoff, 0x20 + intrs * 0x20))
        return fail(r, "register layout outside BAR", 4, r->hcs1);

    /* Validate the entire forward-only extended capability chain before
     * changing ownership. An absent legacy capability requires no semaphore. */
    uint32_t ext = (r->hcc1 >> 16) * 4;
    unsigned count = 0;
    while (ext) {
        if (++count > 256 || ext < 0x20 || !fits(io, ext, 4))
            return fail(r, "invalid extended capability chain", ext, count);
        uint32_t cap = io->read(io->context, ext);
        if (cap == UINT32_MAX || !(cap & 0xff))
            return fail(r, "invalid extended capability", ext, cap);
        if ((cap & 0xff) == 1) {
            if (r->legacy || !fits(io, ext, 8))
                return fail(r, "invalid legacy capability", ext, cap);
            r->legacy = ext;
        }
        uint32_t next = ((cap >> 8) & 0xff) * 4;
        if (next && (cap & 0xff) == 1 && next < 8)
            return fail(r, "overlapping legacy capability", ext, cap);
        ext = next ? ext + next : 0;
    }
    if (r->legacy) {
        uint32_t legacy = io->read(io->context, r->legacy);
        io->write(io->context, r->legacy, legacy | OS_OWN);
        if (!poll(io, r, r->legacy, BIOS_OWN | OS_OWN, OS_OWN, 1000,
                  "BIOS ownership timeout")) return false;
        uint32_t control = io->read(io->context, r->legacy + 4);
        if (control == UINT32_MAX)
            return fail(r, "legacy control unavailable", r->legacy + 4, control);
        /* Preserve reserved fields, disable SMI enables, acknowledge only W1C. */
        io->write(io->context, r->legacy + 4, (control & ~SMI_ENABLE) | SMI_W1C);
        if (io->read(io->context, r->legacy + 4) & SMI_ENABLE)
            return fail(r, "SMI disable failed", r->legacy + 4, control);
    }

    if (!poll(io, r, op + 4, CNR, 0, 1000, "initial CNR timeout")) return false;
    if (!poll(io, r, op, RESET, 0, 1000, "initial HCRST timeout")) return false;
    r->command = io->read(io->context, op);
    io->write(io->context, op, r->command & ~(RUN | INTE | HSEE));
    if (!poll(io, r, op + 4, HALTED, HALTED, 100, "halt timeout")) return false;
    io->write(io->context, op, (r->command & ~(RUN | INTE | HSEE)) | RESET);
    /* Allow a bounded settling interval before the first reset readback. */
    if (!io->delay_ms(io->context)) return fail(r, "PIT wait failed", op, RESET);
    if (!poll(io, r, op, RESET, 0, 1000, "HCRST timeout") ||
        !poll(io, r, op + 4, CNR, 0, 1000, "post-reset CNR timeout")) return false;
    r->command = io->read(io->context, op);
    r->status = io->read(io->context, op + 4);
    r->pagesize = io->read(io->context, op + 8);
    if (r->command == UINT32_MAX || r->status == UINT32_MAX ||
        r->pagesize == UINT32_MAX || (r->command & (RUN | RESET | INTE | HSEE)) ||
        !(r->status & HALTED) || (r->status & ((1u << 2) | (1u << 12))) ||
        !(r->pagesize & 1))
        return fail(r, "invalid reset readback", op + 4, r->status);
    return true;
}
