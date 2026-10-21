#include "xhci_rings.h"
#include "string.h"

#define RUN       (1u << 0)
#define HALTED    (1u << 0)
#define HSE       (1u << 2)
#define CNR       (1u << 11)

static bool fits(const xhci_rings_io_t *io, uint32_t off, uint32_t len) {
    if (!io->mmio_size) return !(off & 3);
    return !(off & 3) && off <= io->mmio_size && len <= io->mmio_size - off;
}

static inline void xhci_clflush_range(const void *p, size_t len) {
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

static bool fail(xhci_dump_record_t *d, const char *msg) {
    if (d) d->error_msg = msg;
    return false;
}

static bool poll_reg(const xhci_rings_io_t *io, uint32_t off, uint32_t mask,
                     uint32_t wanted, unsigned ms) {
    for (unsigned i = 0; i <= ms; ++i) {
        uint32_t val = io->read32(io->mmio_ctx, off);
        if (val == UINT32_MAX) return false;
        if ((val & mask) == wanted) return true;
        if (i == ms) return false;
        if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    return false;
}

bool xhci_verify_rings(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *dma,
                       xhci_dump_record_t *dump) {
    if (dump) {
        *dump = (xhci_dump_record_t){0};
        dump->valid = true;
    }

    if (!fits(io, 0, 0x20)) return fail(dump, "capability aperture too short");
    uint32_t cap = io->read32(io->mmio_ctx, 0);
    uint32_t op = cap & 0xff;
    if (cap == UINT32_MAX || op < 0x20 || !fits(io, op, 0x30))
        return fail(dump, "invalid operational register offset");

    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    if ((dboff & 3) || !fits(io, dboff, 4))
        return fail(dump, "invalid doorbell offset");
    if ((rtsoff & 31) || !fits(io, rtsoff, 0x40))
        return fail(dump, "invalid runtime register offset");

    /* Controller must currently be halted before ring configuration */
    uint32_t usbsts = io->read32(io->mmio_ctx, op + 0x04);
    if (usbsts == UINT32_MAX || !(usbsts & HALTED))
        return fail(dump, "controller not halted prior to ring setup");

    /* Validate alignment of physical DMA buffers (64-byte minimum) */
    if ((dma->cmd_ring_phys & 0x3f) || (dma->event_ring_phys & 0x3f) || (dma->erst_phys & 0x3f))
        return fail(dump, "unaligned DMA buffer physical address");

    /* Initialize Command Ring: 255 command TRBs + 1 Link TRB at end */
    for (uint32_t i = 0; i < XHCI_RING_TRB_COUNT; ++i) {
        dma->cmd_ring_virt[i] = (xhci_trb_t){0};
    }
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)dma->cmd_ring_phys;
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(dma->cmd_ring_phys >> 32);
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].status = 0;
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    /* Initialize Event Ring (all zeroed) */
    for (uint32_t i = 0; i < XHCI_RING_TRB_COUNT; ++i) {
        dma->event_ring_virt[i] = (xhci_trb_t){0};
    }

    /* Initialize ERST: 1 segment pointing to Event Ring */
    dma->erst_virt[0].ring_segment_base_address = dma->event_ring_phys;
    dma->erst_virt[0].ring_segment_size = XHCI_RING_TRB_COUNT;
    dma->erst_virt[0].reserved = 0;

    /* Program Primary Interrupter (Interrupter 0) */
    uint32_t intr0 = rtsoff + 0x20;
    io->write32(io->mmio_ctx, intr0 + 0x00, 1u); /* Clear IP, IE=0 (polled) */
    io->write32(io->mmio_ctx, intr0 + 0x04, 0u); /* IMOD = 0 */
    io->write32(io->mmio_ctx, intr0 + 0x08, 1u); /* ERSTSZ = 1 */
    io->write32(io->mmio_ctx, intr0 + 0x10, (uint32_t)dma->erst_phys);
    io->write32(io->mmio_ctx, intr0 + 0x14, (uint32_t)(dma->erst_phys >> 32));
    io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)dma->event_ring_phys & ~0xfu) | (1u << 3));
    io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(dma->event_ring_phys >> 32));

    /* Program Command Ring Control Register (CRCR) */
    io->write32(io->mmio_ctx, op + 0x18, ((uint32_t)dma->cmd_ring_phys & ~0x3fu) | 1u); /* RCS=1 */
    io->write32(io->mmio_ctx, op + 0x1c, (uint32_t)(dma->cmd_ring_phys >> 32));

    /* Start Controller: set USBCMD.RS = 1 */
    uint32_t usbcmd = io->read32(io->mmio_ctx, op + 0x00);
    io->write32(io->mmio_ctx, op + 0x00, usbcmd | RUN);

    if (!poll_reg(io, op + 0x04, HALTED, 0, 100)) {
        return fail(dump, "controller failed to start (HCH stuck at 1)");
    }

    /* Prepare No-Op Command TRB at index 0 */
    dma->cmd_ring_virt[0].parameter_low = 0;
    dma->cmd_ring_virt[0].parameter_high = 0;
    dma->cmd_ring_virt[0].status = 0;
    dma->cmd_ring_virt[0].control = (XHCI_TRB_TYPE_NOOP_CMD << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;
    __asm__ volatile("" ::: "memory");

    if (dump) {
        dump->last_submitted_trb = dma->cmd_ring_virt[0];
        dump->cmd_enqueue_idx = 1;
        dump->cmd_cycle_state = 1;
    }

    /* Ring Doorbell 0 for Host Controller Command */
    io->write32(io->mmio_ctx, dboff + 0, 0);

    /* Poll Event Ring for Command Completion Event */
    bool completed = false;
    uint32_t event_idx = 0;
    uint32_t event_cycle = 1;

    for (unsigned ms = 0; ms <= 500; ++ms) {
        while (1) {
            volatile const xhci_trb_t *event = &dma->event_ring_virt[event_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (event_cycle ? XHCI_TRB_C : 0)) {
                break; /* No more events currently pending */
            }

            /* Found an event posted by hardware */
            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            uint64_t cmd_ptr = ((uint64_t)event->parameter_high << 32) | event->parameter_low;

            if (dump) {
                dump->last_completed_trb = *(const xhci_trb_t *)event;
                dump->event_dequeue_idx = event_idx;
                dump->event_cycle_state = event_cycle;
                dump->completion_code = comp_code;
            }

            /* Advance dequeue pointer and acknowledge to controller via ERDP */
            event_idx = (event_idx + 1) % XHCI_RING_TRB_COUNT;
            if (event_idx == 0) event_cycle ^= 1;
            uint64_t new_erdp = dma->event_ring_phys + (event_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                if (cmd_ptr == dma->cmd_ring_phys && comp_code == XHCI_COMP_SUCCESS) {
                    completed = true;
                } else {
                    if (dump) dump->error_msg = "mismatched or failed completion event";
                }
                goto poll_done;
            } else if (trb_type == XHCI_TRB_TYPE_PORT_STATUS_EVENT) {
                /* Acknowledge and consume port status change events so they do not clog the ring */
                continue;
            } else {
                /* Other events - consume and continue */
                continue;
            }
        }

        if (ms == 500) {
            fail(dump, "command completion timeout");
            break;
        }
        if (!io->delay_ms(io->mmio_ctx)) {
            fail(dump, "PIT wait failed during poll");
            break;
        }
    }
poll_done:
    if (!completed) {
        /* Stop Controller on failure */
        usbcmd = io->read32(io->mmio_ctx, op + 0x00);
        io->write32(io->mmio_ctx, op + 0x00, usbcmd & ~RUN);
        poll_reg(io, op + 0x04, HALTED, HALTED, 100);
    } else {
        dma->cmd_enqueue_idx = 1;
        dma->cmd_cycle = 1;
        dma->event_dequeue_idx = event_idx;
        dma->event_cycle = event_cycle;
    }

    /* Capture final register state */
    if (dump) {
        dump->usbcmd = io->read32(io->mmio_ctx, op + 0x00);
        dump->usbsts = io->read32(io->mmio_ctx, op + 0x04);
        dump->pagesize = io->read32(io->mmio_ctx, op + 0x08);
    }

    return completed;
}

/* ============================================================================
 * Unified Event Ring Pump & Per-Endpoint Mailbox Architecture
 * ============================================================================ */

spinlock_t g_xhci_ring_lock = SPINLOCK_RANKED(2, "xhci_ring");
spinlock_t g_xhci_cmd_lock  = SPINLOCK_RANKED(2, "xhci_cmd");

static xhci_mailbox_t        s_mailboxes[XHCI_MAX_SLOTS][XHCI_MAX_DCIS];
static xhci_mailbox_t        s_cmd_mailbox;
static xhci_hid_report_fifo_t s_hid_fifo;
static xhci_hid_mouse_fifo_t  s_hid_mouse_fifo;
static uint32_t              s_port_change_events = 0;
static volatile uint32_t     s_hid_reports_pending = 0;
static volatile uint32_t     s_hid_mouse_reports_pending = 0;

#define XHCI_MAX_HID_ENDPOINTS 8

typedef enum {
    XHCI_HID_TYPE_NONE = 0,
    XHCI_HID_TYPE_KEYBOARD = 1,
    XHCI_HID_TYPE_MOUSE = 2,
} xhci_hid_type_t;

typedef struct {
    bool active;
    uint8_t slot_id;
    uint8_t dci;
    xhci_hid_type_t type;
    const uint8_t *report_virt;
} xhci_hid_ep_t;

static xhci_hid_ep_t         s_hid_endpoints[XHCI_MAX_HID_ENDPOINTS];
static uint8_t               s_hid_slot_id = 0;
static uint8_t               s_hid_dci = 0;
static const uint8_t        *s_hid_report_virt = NULL;

void xhci_rings_reset_mailboxes(void) {
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    memset(s_mailboxes, 0, sizeof(s_mailboxes));
    memset(&s_cmd_mailbox, 0, sizeof(s_cmd_mailbox));
    memset(&s_hid_fifo, 0, sizeof(s_hid_fifo));
    memset(&s_hid_mouse_fifo, 0, sizeof(s_hid_mouse_fifo));
    memset(s_hid_endpoints, 0, sizeof(s_hid_endpoints));
    s_port_change_events = 0;
    s_hid_reports_pending = 0;
    s_hid_mouse_reports_pending = 0;
    s_hid_slot_id = 0;
    s_hid_dci = 0;
    s_hid_report_virt = NULL;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
}

void xhci_hid_register_endpoint(uint8_t slot_id, uint8_t dci, uintptr_t report_buf_phys, const uint8_t *report_buf_virt) {
    (void)report_buf_phys;
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    s_hid_slot_id = slot_id;
    s_hid_dci = dci;
    s_hid_report_virt = report_buf_virt;
    int free_idx = -1;
    for (int i = 0; i < XHCI_MAX_HID_ENDPOINTS; i++) {
        if (s_hid_endpoints[i].active && s_hid_endpoints[i].slot_id == slot_id && s_hid_endpoints[i].dci == dci) {
            free_idx = i;
            break;
        }
        if (!s_hid_endpoints[i].active && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) {
        s_hid_endpoints[free_idx].active = true;
        s_hid_endpoints[free_idx].slot_id = slot_id;
        s_hid_endpoints[free_idx].dci = dci;
        s_hid_endpoints[free_idx].type = XHCI_HID_TYPE_KEYBOARD;
        s_hid_endpoints[free_idx].report_virt = report_buf_virt;
    }
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
}

void xhci_hid_register_mouse_endpoint(uint8_t slot_id, uint8_t dci, uintptr_t report_buf_phys, const uint8_t *report_buf_virt) {
    (void)report_buf_phys;
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    int free_idx = -1;
    for (int i = 0; i < XHCI_MAX_HID_ENDPOINTS; i++) {
        if (s_hid_endpoints[i].active && s_hid_endpoints[i].slot_id == slot_id && s_hid_endpoints[i].dci == dci) {
            free_idx = i;
            break;
        }
        if (!s_hid_endpoints[i].active && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) {
        s_hid_endpoints[free_idx].active = true;
        s_hid_endpoints[free_idx].slot_id = slot_id;
        s_hid_endpoints[free_idx].dci = dci;
        s_hid_endpoints[free_idx].type = XHCI_HID_TYPE_MOUSE;
        s_hid_endpoints[free_idx].report_virt = report_buf_virt;
    }
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
}

void xhci_event_ring_pump(const xhci_rings_io_t *io, xhci_dma_buffers_t *dma) {
    if (!io || !dma || !dma->event_ring_virt) return;
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    if ((rtsoff & 31) || !fits(io, rtsoff, 0x40)) return;
    uint32_t intr0 = rtsoff + 0x20;

    for (unsigned drained = 0; drained < XHCI_PUMP_EVENT_BUDGET; ++drained) {
        volatile const xhci_trb_t *event = &dma->event_ring_virt[dma->event_dequeue_idx];
        uint32_t control = __atomic_load_n(&event->control, __ATOMIC_ACQUIRE);
        uint32_t c_bit = control & XHCI_TRB_C;
        if (c_bit != (dma->event_cycle ? XHCI_TRB_C : 0)) break;
        __asm__ volatile("lfence" ::: "memory"); /* Read barrier after cycle-bit check */

        uint32_t trb_type = (control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
        uint32_t comp_code = (event->status >> 24) & 0xff;
        uint8_t ev_slot = (control >> 24) & 0xff;
        uint8_t ev_dci  = (control >> 16) & 0x1f;
        uint64_t completed_phys = ((uint64_t)event->parameter_high << 32) | event->parameter_low;
        uint32_t residual = event->status & 0xffffff;
        bool event_data = (control & (1u << 2)) != 0;

        /* Advance event dequeue index and acknowledge via ERDP (EHB bit 3 = 1 to clear busy) */
        uint32_t next_deq = (dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
        if (next_deq == 0) dma->event_cycle ^= 1;
        __atomic_store_n(&dma->event_dequeue_idx, next_deq, __ATOMIC_RELEASE);
        uint64_t new_erdp = dma->event_ring_phys + (next_deq * sizeof(xhci_trb_t));
        io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
        io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

        /* Demux into mailboxes */
        if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
            if (s_cmd_mailbox.count < XHCI_MBOX_DEPTH) {
                xhci_event_t *slot = &s_cmd_mailbox.events[s_cmd_mailbox.tail];
                slot->comp_code = comp_code;
                slot->residual = residual;
                slot->trb_param = completed_phys;
                slot->slot_id = ev_slot;
                slot->dci = ev_dci;
                slot->event_data = event_data;
                slot->valid = true;
                s_cmd_mailbox.tail = (s_cmd_mailbox.tail + 1) % XHCI_MBOX_DEPTH;
                s_cmd_mailbox.count++;
            } else {
                s_cmd_mailbox.drops++;
            }
        } else if (trb_type == XHCI_TRB_TYPE_PORT_STATUS_EVENT) {
            s_port_change_events++;
        } else if (trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            xhci_hid_ep_t *hid_ep = NULL;
            for (int i = 0; i < XHCI_MAX_HID_ENDPOINTS; i++) {
                if (s_hid_endpoints[i].active &&
                    s_hid_endpoints[i].slot_id == ev_slot &&
                    s_hid_endpoints[i].dci == ev_dci) {
                    hid_ep = &s_hid_endpoints[i];
                    break;
                }
            }
            if (!hid_ep && s_hid_slot_id && ev_slot == s_hid_slot_id && ev_dci == s_hid_dci && s_hid_report_virt) {
                static xhci_hid_ep_t legacy_ep;
                legacy_ep.active = true;
                legacy_ep.slot_id = s_hid_slot_id;
                legacy_ep.dci = s_hid_dci;
                legacy_ep.type = XHCI_HID_TYPE_KEYBOARD;
                legacy_ep.report_virt = s_hid_report_virt;
                hid_ep = &legacy_ep;
            }

            if (hid_ep && hid_ep->report_virt) {
                if (comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET) {
                    xhci_clflush_range(hid_ep->report_virt, 8);
                    __asm__ volatile("mfence" ::: "memory");
                    if (hid_ep->type == XHCI_HID_TYPE_KEYBOARD) {
                        if (s_hid_fifo.count == XHCI_HID_FIFO_CAP) {
                            /* Circular buffer overflow: drop oldest entry so newest key state always lands */
                            s_hid_fifo.head = (s_hid_fifo.head + 1) % XHCI_HID_FIFO_CAP;
                            s_hid_fifo.count--;
                            s_hid_fifo.drops++;
                        }
                        for (int b = 0; b < 8; ++b) {
                            s_hid_fifo.reports[s_hid_fifo.tail][b] = hid_ep->report_virt[b];
                        }
                        s_hid_fifo.tail = (s_hid_fifo.tail + 1) % XHCI_HID_FIFO_CAP;
                        s_hid_fifo.count++;
                        __atomic_fetch_add(&s_hid_reports_pending, 1, __ATOMIC_RELEASE);
                    } else if (hid_ep->type == XHCI_HID_TYPE_MOUSE) {
                        if (s_hid_mouse_fifo.count == XHCI_HID_FIFO_CAP) {
                            /* Circular buffer overflow: drop oldest entry */
                            s_hid_mouse_fifo.head = (s_hid_mouse_fifo.head + 1) % XHCI_HID_FIFO_CAP;
                            s_hid_mouse_fifo.count--;
                            s_hid_mouse_fifo.drops++;
                        }
                        uint8_t tlen = 8;
                        if (residual < 8) {
                            tlen = (uint8_t)(8 - residual);
                        }
                        if (tlen < 3) tlen = 3;
                        for (int b = 0; b < 8; ++b) {
                            s_hid_mouse_fifo.reports[s_hid_mouse_fifo.tail][b] = hid_ep->report_virt[b];
                        }
                        s_hid_mouse_fifo.lens[s_hid_mouse_fifo.tail] = tlen;
                        s_hid_mouse_fifo.slot_ids[s_hid_mouse_fifo.tail] = ev_slot;
                        s_hid_mouse_fifo.dcis[s_hid_mouse_fifo.tail] = ev_dci;
                        s_hid_mouse_fifo.tail = (s_hid_mouse_fifo.tail + 1) % XHCI_HID_FIFO_CAP;
                        s_hid_mouse_fifo.count++;
                        __atomic_fetch_add(&s_hid_mouse_reports_pending, 1, __ATOMIC_RELEASE);
                    }
                }
            } else if (ev_slot < XHCI_MAX_SLOTS && ev_dci < XHCI_MAX_DCIS) {
                xhci_mailbox_t *mb = &s_mailboxes[ev_slot][ev_dci];
                if (mb->count < XHCI_MBOX_DEPTH) {
                    xhci_event_t *slot = &mb->events[mb->tail];
                    slot->comp_code = comp_code;
                    slot->residual = residual;
                    slot->trb_param = completed_phys;
                    slot->slot_id = ev_slot;
                    slot->dci = ev_dci;
                    slot->event_data = event_data;
                    slot->valid = true;
                    mb->tail = (mb->tail + 1) % XHCI_MBOX_DEPTH;
                    mb->count++;
                } else {
                    mb->drops++;
                }
            }
        }
    }
}

static inline bool xhci_ring_poll_delay(const xhci_rings_io_t *io, unsigned *elapsed_us) {
    unsigned step = (io->delay_us && *elapsed_us < 2000) ? 10 : 1000;
    *elapsed_us += step;
    return (step == 10) ? io->delay_us(io->mmio_ctx, step) : io->delay_ms(io->mmio_ctx);
}

bool xhci_mbox_wait_transfer(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *dma,
                             uint8_t slot_id,
                             uint8_t dci,
                             uintptr_t submitted_phys,
                             uint32_t *out_residual,
                             uint8_t *out_code,
                             unsigned timeout_us) {
    if (slot_id >= XHCI_MAX_SLOTS || dci >= XHCI_MAX_DCIS) return false;
    if (out_code) *out_code = 0;
    xhci_mailbox_t *mb = &s_mailboxes[slot_id][dci];

    for (unsigned elapsed_us = 0; elapsed_us <= timeout_us;) {
        uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
        xhci_event_ring_pump(io, dma);

        if (mb->count > 0) {
            xhci_event_t ev = mb->events[mb->head];
            mb->head = (mb->head + 1) % XHCI_MBOX_DEPTH;
            mb->count--;
            spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

            if (out_code) *out_code = (uint8_t)ev.comp_code;
            if (out_residual) *out_residual = ev.residual;
            if (ev.event_data || (submitted_phys != 0 && ev.trb_param != submitted_phys)) return false;
            return ev.comp_code == XHCI_COMP_SUCCESS || ev.comp_code == XHCI_COMP_SHORT_PACKET;
        }
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

        if (!xhci_ring_poll_delay(io, &elapsed_us)) return false;
    }
    return false;
}

bool xhci_mbox_wait_command(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *dma,
                            uintptr_t submitted_phys,
                            xhci_trb_t *out_event,
                            unsigned timeout_us) {
    for (unsigned elapsed_us = 0; elapsed_us <= timeout_us;) {
        uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
        xhci_event_ring_pump(io, dma);

        if (s_cmd_mailbox.count > 0) {
            xhci_event_t ev = s_cmd_mailbox.events[s_cmd_mailbox.head];
            s_cmd_mailbox.head = (s_cmd_mailbox.head + 1) % XHCI_MBOX_DEPTH;
            s_cmd_mailbox.count--;
            spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

            if (out_event) {
                out_event->parameter_low = (uint32_t)ev.trb_param;
                out_event->parameter_high = (uint32_t)(ev.trb_param >> 32);
                out_event->status = ev.residual | (ev.comp_code << 24);
                out_event->control = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << 10) | ((uint32_t)ev.slot_id << 24);
            }
            return (submitted_phys == 0 || ev.trb_param == submitted_phys) && ev.comp_code == XHCI_COMP_SUCCESS;
        }
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

        if (!xhci_ring_poll_delay(io, &elapsed_us)) return false;
    }
    return false;
}

bool xhci_hid_fifo_pop(uint8_t out_report[8]) {
    if (!out_report) return false;
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    if (s_hid_fifo.count == 0) {
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
        return false;
    }

    for (int b = 0; b < 8; ++b) {
        out_report[b] = s_hid_fifo.reports[s_hid_fifo.head][b];
    }
    s_hid_fifo.head = (s_hid_fifo.head + 1) % XHCI_HID_FIFO_CAP;
    s_hid_fifo.count--;

    if (__atomic_load_n(&s_hid_reports_pending, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&s_hid_reports_pending, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return true;
}

bool xhci_hid_mouse_fifo_pop(uint8_t out_report[8], uint8_t *out_len, uint8_t *out_slot, uint8_t *out_dci) {
    if (!out_report) return false;
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    if (s_hid_mouse_fifo.count == 0) {
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
        return false;
    }

    for (int b = 0; b < 8; ++b) {
        out_report[b] = s_hid_mouse_fifo.reports[s_hid_mouse_fifo.head][b];
    }
    if (out_len) *out_len = s_hid_mouse_fifo.lens[s_hid_mouse_fifo.head];
    if (out_slot) *out_slot = s_hid_mouse_fifo.slot_ids[s_hid_mouse_fifo.head];
    if (out_dci) *out_dci = s_hid_mouse_fifo.dcis[s_hid_mouse_fifo.head];

    s_hid_mouse_fifo.head = (s_hid_mouse_fifo.head + 1) % XHCI_HID_FIFO_CAP;
    s_hid_mouse_fifo.count--;

    if (__atomic_load_n(&s_hid_mouse_reports_pending, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&s_hid_mouse_reports_pending, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return true;
}

uint32_t xhci_hid_reports_pending(void) {
    return __atomic_load_n(&s_hid_reports_pending, __ATOMIC_ACQUIRE);
}

uint32_t xhci_hid_mouse_reports_pending(void) {
    return __atomic_load_n(&s_hid_mouse_reports_pending, __ATOMIC_ACQUIRE);
}

uint32_t xhci_port_change_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    uint32_t cnt = s_port_change_events;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return cnt;
}

uint32_t xhci_mbox_drop_count(uint8_t slot_id, uint8_t dci) {
    if (slot_id >= XHCI_MAX_SLOTS || dci >= XHCI_MAX_DCIS) return 0;
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    uint32_t d = s_mailboxes[slot_id][dci].drops;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return d;
}

uint32_t xhci_cmd_mbox_drop_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    uint32_t d = s_cmd_mailbox.drops;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return d;
}

uint32_t xhci_hid_fifo_drop_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    uint32_t d = s_hid_fifo.drops;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return d;
}

uint32_t xhci_hid_mouse_fifo_drop_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    uint32_t d = s_hid_mouse_fifo.drops;
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);
    return d;
}
