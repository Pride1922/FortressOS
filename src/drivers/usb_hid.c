#include "usb_hid.h"
#include "input.h"
#include "serial.h"
#include <string.h>

#ifndef USB_HID_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "apic.h"
#include "time.h"
#endif

#define XHCI_TRB_TYPE_NORMAL          1u
#define XHCI_TRB_TYPE_LINK            6u
#define XHCI_TRB_TYPE_CONFIG_EP_CMD   12u

static usb_hid_kbd_t s_keyboards[USB_HID_MAX_KEYBOARDS];
static bool s_hid_initialized = false;
static bool s_worker_started = false;
static const xhci_rings_io_t *s_active_io = NULL;
static xhci_dma_buffers_t *s_active_ring_dma = NULL;

static inline void hid_clflush(const void *p, size_t len) {
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

static uint8_t calc_interval(uint8_t speed, uint8_t bInterval) {
    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        if (bInterval == 0) return 3;
        uint32_t val = (uint32_t)bInterval * 8;
        int bit = 31 - __builtin_clz(val);
        if (bit < 3) bit = 3;
        if (bit > 15) bit = 15;
        return (uint8_t)bit;
    } else {
        if (bInterval == 0) return 0;
        uint8_t exp = bInterval - 1;
        if (exp > 15) exp = 15;
        return exp;
    }
}

void usb_hid_init(void) {
    if (s_hid_initialized) return;
    memset(s_keyboards, 0, sizeof(s_keyboards));
    s_hid_initialized = true;
}

static void usb_hid_rearm(const xhci_rings_io_t *io, usb_hid_kbd_t *kbd) {
    uint32_t idx = kbd->ring_idx;
    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)kbd->report_buf_phys;
    trb.parameter_high = (uint32_t)(kbd->report_buf_phys >> 32);
    trb.status = 8; /* 8-byte boot keyboard report */
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | (1u << 5) /* IOC */ | (kbd->ring_cycle ? 1u : 0u);
    kbd->ring_virt[idx] = trb;
    hid_clflush(&kbd->ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    kbd->ring_idx++;
    if (kbd->ring_idx == XHCI_RING_TRB_COUNT - 1) {
        kbd->ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (kbd->ring_cycle ? 1u : 0u);
        hid_clflush(&kbd->ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        kbd->ring_idx = 0;
        kbd->ring_cycle ^= 1;
    }

    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    io->write32(io->mmio_ctx, dboff + kbd->slot_id * 4, kbd->dci);
}

bool usb_hid_configure_keyboard(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               xhci_dev_dma_t *dev_dma,
                               const xhci_bot_device_t *device) {
    if (!io || !ring_dma || !dev_dma || !device) return false;
    if (!device->is_hid_keyboard || !device->slot_id || !device->int_in_ep) return false;

    usb_hid_init();

    int slot_idx = -1;
    for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
        if (!s_keyboards[i].active) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        serial_puts("[USB HID] Maximum keyboards reached\n");
        return false;
    }

    uint8_t ep_num = device->int_in_ep & 0x0F;
    uint8_t dci = (ep_num * 2) + 1; /* Interrupt IN is odd DCI */

#ifndef USB_HID_HOST_TEST
    uintptr_t ring_phys = pmm_alloc_page();
    uintptr_t report_phys = pmm_alloc_page();
    if (!ring_phys || !report_phys) {
        if (ring_phys) pmm_free_page(ring_phys);
        if (report_phys) pmm_free_page(report_phys);
        serial_puts("[USB HID] OOM allocating keyboard DMA pages\n");
        return false;
    }
    xhci_trb_t *ring_virt = (xhci_trb_t *)vmm_phys_to_virt(ring_phys);
    uint8_t *report_virt = (uint8_t *)vmm_phys_to_virt(report_phys);
#else
    uintptr_t ring_phys = 0x100000;
    uintptr_t report_phys = 0x200000;
    static xhci_trb_t mock_ring[XHCI_RING_TRB_COUNT];
    static uint8_t mock_report[4096];
    xhci_trb_t *ring_virt = mock_ring;
    uint8_t *report_virt = mock_report;
#endif

    memset(ring_virt, 0, 4096);
    memset(report_virt, 0, 4096);

    /* Setup Link TRB at the end of the transfer ring */
    ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)ring_phys;
    ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(ring_phys >> 32);
    ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_C;
    hid_clflush(ring_virt, 4096);

    /* Setup Input Context for Configure Endpoint */
    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5; /* 64-byte vs 32-byte contexts */
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *ep_ctx   = &dev_dma->input_ctx_virt[(dci + 1) * ctx_dwords];

    /* Add Context Flags: A0 (Slot Context) + ADCI (Interrupt IN) */
    ctrl_ctx[1] = (1u << 0) | (1u << dci);

    /* Slot Context: Context Entries = dci */
    slot_ctx[0] = ((uint32_t)device->speed << 20) | ((uint32_t)dci << 27);
    slot_ctx[1] = (uint32_t)device->port_num << 16;

    uint8_t interval = calc_interval(device->speed, device->int_in_interval);
    uint16_t max_packet = device->int_in_max_packet ? device->int_in_max_packet : 8;

    /* Endpoint Context:
     * DW0: Interval
     * DW1: CErr = 3, EP Type = 7 (Interrupt IN), Max Packet Size
     * DW2-3: TR Dequeue Pointer (DCS = 1)
     * DW4: Average TRB Length = 8, Max ESIT Payload = max_packet */
    ep_ctx[0] = ((uint32_t)interval << 16);
    ep_ctx[1] = (3u << 1) | (7u << 3) | ((uint32_t)max_packet << 16);
    ep_ctx[2] = ((uint32_t)ring_phys & ~0x3fu) | 1u;
    ep_ctx[3] = (uint32_t)(ring_phys >> 32);
    ep_ctx[4] = 8 | ((uint32_t)max_packet << 16);

    hid_clflush(dev_dma->input_ctx_virt, 4096);
    __asm__ volatile("mfence" ::: "memory");

    /* Issue Configure Endpoint Command */
    xhci_trb_t cfg_cmd = {0};
    cfg_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    cfg_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    cfg_cmd.control = (XHCI_TRB_TYPE_CONFIG_EP_CMD << 10) | ((uint32_t)device->slot_id << 24);

    xhci_trb_t comp_event = {0};
    if (!xhci_dev_send_command(io, ring_dma, cfg_cmd, &comp_event)) {
        serial_puts("[USB HID] Configure Endpoint command failed\n");
        return false;
    }

    /* Send SET_PROTOCOL(0) (Boot Protocol) via Control Transfer on EP0 */
    usb_setup_pkt_t set_proto = {
        .bmRequestType = 0x21, /* Host to Device, Class, Interface */
        .bRequest = 0x0B,      /* SET_PROTOCOL */
        .wValue = 0,           /* 0 = Boot Protocol */
        .wIndex = device->hid_if_num,
        .wLength = 0,
    };
    if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, device->slot_id, set_proto, NULL, 0, false, NULL)) {
        serial_puts("[USB HID] SET_PROTOCOL(0) failed (continuing)\n");
    }

    /* Send SET_IDLE(0) (Duration = 0, Indefinite until state changes) */
    usb_setup_pkt_t set_idle = {
        .bmRequestType = 0x21,
        .bRequest = 0x0A,      /* SET_IDLE */
        .wValue = 0,           /* Duration = 0 */
        .wIndex = device->hid_if_num,
        .wLength = 0,
    };
    if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, device->slot_id, set_idle, NULL, 0, false, NULL)) {
        serial_puts("[USB HID] SET_IDLE(0) failed (continuing)\n");
    }

    /* Register active endpoint with unified event ring demux */
    xhci_hid_register_endpoint(device->slot_id, dci, report_phys, report_virt);

    usb_hid_kbd_t *kbd = &s_keyboards[slot_idx];
    kbd->active = true;
    kbd->slot_id = device->slot_id;
    kbd->dci = dci;
    kbd->if_num = device->hid_if_num;
    kbd->interval = interval;
    kbd->max_packet = max_packet;
    kbd->ring_phys = ring_phys;
    kbd->ring_virt = ring_virt;
    kbd->ring_idx = 0;
    kbd->ring_cycle = 1;
    kbd->report_buf_phys = report_phys;
    kbd->report_buf_virt = report_virt;
    memset(&kbd->prev_report, 0, sizeof(kbd->prev_report));

    /* Submit initial Interrupt IN Normal TRB and ring doorbell */
    usb_hid_rearm(io, kbd);

    serial_puts("[USB HID] PASS: Keyboard configured on Slot ");
    serial_print_hex(device->slot_id);
    serial_puts(" DCI=");
    serial_print_hex(dci);
    serial_puts(" (Boot Protocol)\n");

    /* Start worker thread if not started */
    s_active_io = io;
    s_active_ring_dma = ring_dma;
    usb_hid_start_worker(io, ring_dma);

    return true;
}

static void hid_scancode_sink(uint8_t scancode, bool extended, void *ctx) {
    (void)ctx;
    serial_puts("[USB HID] Scancode=");
    serial_print_hex(scancode);
    serial_puts(extended ? " (ext)\n" : "\n");
    if (extended) {
        uint8_t ext_prefix = 0xE0;
        input_inject_scancodes(&ext_prefix, 1);
    }
    input_inject_scancodes(&scancode, 1);
}

void usb_hid_poll(const xhci_rings_io_t *io, xhci_dma_buffers_t *ring_dma) {
    if (!io || !ring_dma) return;

    /* 1. Pump unified event ring (safely handles BOT, Commands, HID, and Ports) */
    xhci_event_ring_pump(io, ring_dma);

    /* 2. Drain HID circular FIFO */
    uint8_t raw_report[8];
    while (xhci_hid_fifo_pop(raw_report)) {
        serial_puts("[USB HID] Report popped: mod=");
        serial_print_hex(raw_report[0]);
        serial_puts(" key0=");
        serial_print_hex(raw_report[2]);
        serial_puts("\n");

        /* In single keyboard setup (common), route to active keyboard */
        usb_hid_kbd_t *kbd = NULL;
        for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
            if (s_keyboards[i].active) {
                kbd = &s_keyboards[i];
                break;
            }
        }
        if (!kbd) continue;

        const hid_boot_kbd_report_t *curr = (const hid_boot_kbd_report_t *)raw_report;
        hid_kbd_process_report(curr, &kbd->prev_report, hid_scancode_sink, NULL);

        /* Re-arm the endpoint transfer ring so xHC can receive the next report */
        usb_hid_rearm(io, kbd);
    }
}

#ifndef USB_HID_HOST_TEST
static bool hid_deadline_reached(void *arg) {
    uint64_t now = apic_timer_get_bsp_ticks();
    uint64_t deadline = *(uint64_t *)arg;
    return xhci_hid_reports_pending() > 0 || now >= deadline;
}

static void usb_hid_worker_main(void *arg) {
    (void)arg;
    serial_puts("[USB HID] Ingress worker started on BSP; tick-bounded polling ready\n");
    while (1) {
        if (s_active_io && s_active_ring_dma) {
            usb_hid_poll(s_active_io, s_active_ring_dma);
        }

        uint64_t before = apic_timer_get_bsp_ticks();
        uint64_t deadline = before + 1; /* 10ms wait */
        sched_wait_until(time_get_sleep_channel(), hid_deadline_reached, &deadline);
    }
}
#endif

bool usb_hid_start_worker(const xhci_rings_io_t *io, xhci_dma_buffers_t *ring_dma) {
    s_active_io = io;
    s_active_ring_dma = ring_dma;
    if (s_worker_started) return true;

#ifndef USB_HID_HOST_TEST
    tcb_t *t = thread_create_on_cpu(0, "usb_hid_worker", usb_hid_worker_main, NULL);
    if (!t) {
        serial_puts("[USB HID] Failed to start BSP worker thread\n");
        return false;
    }
#endif
    s_worker_started = true;
    return true;
}

bool usb_hid_is_active(void) {
    for (int i = 0; i < USB_HID_MAX_KEYBOARDS; i++) {
        if (s_keyboards[i].active) return true;
    }
    return false;
}
