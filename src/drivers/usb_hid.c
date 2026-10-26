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
#include "console.h"
#include "mouse_dev.h"
#endif

#define XHCI_TRB_TYPE_NORMAL          1u
#define XHCI_TRB_TYPE_LINK            6u
#define XHCI_TRB_TYPE_CONFIG_EP_CMD   12u

static usb_hid_kbd_t s_keyboards[USB_HID_MAX_KEYBOARDS];
static usb_hid_mouse_t s_mice[USB_HID_MAX_MICE];
static hid_mouse_event_fn_t s_mouse_custom_cb = NULL;
static void *s_mouse_custom_ctx = NULL;
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
    memset(s_mice, 0, sizeof(s_mice));
    s_hid_initialized = true;
}

void usb_hid_set_mouse_callback(hid_mouse_event_fn_t callback, void *ctx) {
    s_mouse_custom_cb = callback;
    s_mouse_custom_ctx = ctx;
}

static void usb_hid_mouse_rearm(const xhci_rings_io_t *io, usb_hid_mouse_t *mouse) {
    uint32_t idx = mouse->ring_idx;
    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)mouse->report_buf_phys;
    trb.parameter_high = (uint32_t)(mouse->report_buf_phys >> 32);
    trb.status = (mouse->max_packet >= 4) ? mouse->max_packet : 8;
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | (1u << 5) /* IOC */ | (mouse->ring_cycle ? 1u : 0u);
    mouse->ring_virt[idx] = trb;
    hid_clflush(&mouse->ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    mouse->ring_idx++;
    if (mouse->ring_idx == XHCI_RING_TRB_COUNT - 1) {
        mouse->ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (mouse->ring_cycle ? 1u : 0u);
        hid_clflush(&mouse->ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        mouse->ring_idx = 0;
        mouse->ring_cycle ^= 1;
    }

    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    io->write32(io->mmio_ctx, dboff + mouse->slot_id * 4, mouse->dci);
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
    if (dev_dma->slots_output_ctx_virt[device->slot_id]) {
        uint32_t *out_slot = dev_dma->slots_output_ctx_virt[device->slot_id];
        slot_ctx[0] = (out_slot[0] & ~0xF8000000u) | ((uint32_t)dci << 27);
        slot_ctx[1] = out_slot[1];
        slot_ctx[2] = out_slot[2];
        slot_ctx[3] = out_slot[3];
    } else {
        slot_ctx[0] = ((uint32_t)device->speed << 20) | ((uint32_t)dci << 27);
        slot_ctx[1] = (uint32_t)device->port_num << 16;
    }

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

bool usb_hid_configure_mouse(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             xhci_dev_dma_t *dev_dma,
                             const xhci_bot_device_t *device) {
    if (!io || !ring_dma || !dev_dma || !device) return false;
    if (!device->is_hid_mouse || !device->slot_id || !device->int_in_ep) return false;

    usb_hid_init();

    int slot_idx = -1;
    for (int i = 0; i < USB_HID_MAX_MICE; i++) {
        if (!s_mice[i].active) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        serial_puts("[USB HID] Maximum mice reached\n");
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
        serial_puts("[USB HID] OOM allocating mouse DMA pages\n");
        return false;
    }
    xhci_trb_t *ring_virt = (xhci_trb_t *)vmm_phys_to_virt(ring_phys);
    uint8_t *report_virt = (uint8_t *)vmm_phys_to_virt(report_phys);
#else
    uintptr_t ring_phys = 0x300000;
    uintptr_t report_phys = 0x400000;
    static xhci_trb_t mock_mouse_ring[XHCI_RING_TRB_COUNT];
    static uint8_t mock_mouse_report[4096];
    xhci_trb_t *ring_virt = mock_mouse_ring;
    uint8_t *report_virt = mock_mouse_report;
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
    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5;
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *ep_ctx   = &dev_dma->input_ctx_virt[(dci + 1) * ctx_dwords];

    /* Add Context Flags: A0 (Slot Context) + ADCI (Interrupt IN) */
    ctrl_ctx[1] = (1u << 0) | (1u << dci);

    /* Slot Context: Context Entries = dci, preserve topology if slot output context exists */
    if (dev_dma->slots_output_ctx_virt[device->slot_id]) {
        uint32_t *out_slot = dev_dma->slots_output_ctx_virt[device->slot_id];
        slot_ctx[0] = (out_slot[0] & ~0xF8000000u) | ((uint32_t)dci << 27);
        slot_ctx[1] = out_slot[1];
        slot_ctx[2] = out_slot[2];
        slot_ctx[3] = out_slot[3];
    } else {
        slot_ctx[0] = ((uint32_t)device->speed << 20) | ((uint32_t)dci << 27);
        slot_ctx[1] = (uint32_t)device->port_num << 16;
    }

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
        serial_puts("[USB HID] Mouse Configure Endpoint command failed\n");
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
        serial_puts("[USB HID] Mouse SET_PROTOCOL(0) failed (continuing)\n");
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
        serial_puts("[USB HID] Mouse SET_IDLE(0) failed (continuing)\n");
    }

    /* Register active endpoint with unified event ring demux */
    xhci_hid_register_mouse_endpoint(device->slot_id, dci, report_phys, report_virt);

    usb_hid_mouse_t *mouse = &s_mice[slot_idx];
    mouse->active = true;
    mouse->slot_id = device->slot_id;
    mouse->dci = dci;
    mouse->if_num = device->hid_if_num;
    mouse->interval = interval;
    mouse->max_packet = max_packet;
    mouse->ring_phys = ring_phys;
    mouse->ring_virt = ring_virt;
    mouse->ring_idx = 0;
    mouse->ring_cycle = 1;
    mouse->report_buf_phys = report_phys;
    mouse->report_buf_virt = report_virt;
    memset(&mouse->state, 0, sizeof(mouse->state));
#ifndef USB_HID_HOST_TEST
    uint64_t cols = 80, rows = 25;
    console_get_dimensions(&cols, &rows);
    if (!cols) cols = 80;
    if (!rows) rows = 25;
    mouse->state.max_x = (int32_t)(cols * 8 - 1);
    mouse->state.max_y = (int32_t)(rows * 16 - 1);
    mouse->state.x = (int32_t)(cols * 8 / 2);
    mouse->state.y = (int32_t)(rows * 16 / 2);
#else
    mouse->state.max_x = 1024;
    mouse->state.max_y = 768;
    mouse->state.x = 512;
    mouse->state.y = 384;
#endif

    /* Submit initial Interrupt IN Normal TRB and ring doorbell */
    usb_hid_mouse_rearm(io, mouse);

    serial_puts("[USB HID] PASS: Mouse configured on Slot ");
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

#ifndef USB_HID_HOST_TEST
static size_t format_sgr_mouse(char *buf, size_t cap, uint32_t btn, uint32_t col, uint32_t row, char final_ch) {
    if (cap < 32) return 0;
    size_t pos = 0;
    buf[pos++] = '\x1b';
    buf[pos++] = '[';
    buf[pos++] = '<';

    /* Append btn */
    char num[12];
    size_t npos = 0;
    if (btn == 0) {
        buf[pos++] = '0';
    } else {
        uint32_t v = btn;
        while (v > 0) { num[npos++] = (char)('0' + (v % 10)); v /= 10; }
        while (npos > 0) buf[pos++] = num[--npos];
    }
    buf[pos++] = ';';

    /* Append col */
    if (col == 0) {
        buf[pos++] = '1';
    } else {
        uint32_t v = col;
        while (v > 0) { num[npos++] = (char)('0' + (v % 10)); v /= 10; }
        while (npos > 0) buf[pos++] = num[--npos];
    }
    buf[pos++] = ';';

    /* Append row */
    if (row == 0) {
        buf[pos++] = '1';
    } else {
        uint32_t v = row;
        while (v > 0) { num[npos++] = (char)('0' + (v % 10)); v /= 10; }
        while (npos > 0) buf[pos++] = num[--npos];
    }
    buf[pos++] = final_ch;
    buf[pos] = '\0';
    return pos;
}
#endif

static void hid_mouse_event_sink(const hid_mouse_event_t *ev, void *ctx) {
    (void)ctx;
#ifdef USB_MOUSE_TRACE
    /* Per-packet traces must not paint/scroll a userspace-owned framebuffer.
     * Keep these diagnostics on COM1; normal device lifecycle logs use dmesg. */
    serial_raw_puts("[USB MOUSE] Event: btn=");
    serial_raw_print_hex(ev->buttons);
    serial_raw_puts(" dx=");
    serial_raw_print_hex((uint8_t)ev->dx);
    serial_raw_puts(" dy=");
    serial_raw_print_hex((uint8_t)ev->dy);
    serial_raw_puts(" dz=");
    serial_raw_print_hex((uint8_t)ev->dz);
    serial_raw_puts(" pos=(");
    serial_raw_print_hex((uint32_t)ev->x);
    serial_raw_puts(",");
    serial_raw_print_hex((uint32_t)ev->y);
    serial_raw_puts(")\n");

#endif

#ifndef USB_HID_HOST_TEST
    mouse_dev_push(ev->buttons, ev->dx, ev->dy, ev->dz);

    if (console_is_mouse_reporting()) {
        uint64_t cols = 80, rows = 25;
        console_get_dimensions(&cols, &rows);
        if (!cols) cols = 80;
        if (!rows) rows = 25;

        uint32_t col = (uint32_t)(ev->x / 8) + 1;
        uint32_t row = (uint32_t)(ev->y / 16) + 1;
        if (col > cols) col = (uint32_t)cols;
        if (row > rows) row = (uint32_t)rows;

        char sgr[32];
        /* Left button press */
        if (ev->pressed & HID_MOUSE_BTN_LEFT) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 0, col, row, 'M');
            input_inject_bytes(sgr, len);
        }
        /* Left button release */
        if (ev->released & HID_MOUSE_BTN_LEFT) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 0, col, row, 'm');
            input_inject_bytes(sgr, len);
        }
        /* Right button press */
        if (ev->pressed & HID_MOUSE_BTN_RIGHT) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 2, col, row, 'M');
            input_inject_bytes(sgr, len);
        }
        /* Right button release */
        if (ev->released & HID_MOUSE_BTN_RIGHT) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 2, col, row, 'm');
            input_inject_bytes(sgr, len);
        }
        /* Scroll wheel UP */
        if (ev->dz > 0) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 64, col, row, 'M');
            input_inject_bytes(sgr, len);
        }
        /* Scroll wheel DOWN */
        if (ev->dz < 0) {
            size_t len = format_sgr_mouse(sgr, sizeof(sgr), 65, col, row, 'M');
            input_inject_bytes(sgr, len);
        }
    }
#endif
}

void usb_hid_poll(const xhci_rings_io_t *io, xhci_dma_buffers_t *ring_dma) {
    if (!io || !ring_dma) return;

    /* 1. Pump unified event ring (safely handles BOT, Commands, HID, and Ports) */
    uint64_t rflags = spin_lock_irqsave(&g_xhci_ring_lock);
    xhci_event_ring_pump(io, ring_dma);
    spin_unlock_irqrestore(&g_xhci_ring_lock, rflags);

    /* 2. Drain HID keyboard circular FIFO */
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

    /* 3. Drain HID mouse circular FIFO */
    uint8_t mouse_report[8];
    uint8_t m_len = 0;
    uint8_t m_slot = 0;
    uint8_t m_dci = 0;
    while (xhci_hid_mouse_fifo_pop(mouse_report, &m_len, &m_slot, &m_dci)) {
#ifdef USB_MOUSE_TRACE
        serial_raw_puts("[USB MOUSE] Report popped: btn=");
        serial_raw_print_hex(mouse_report[0]);
        serial_raw_puts(" dx=");
        serial_raw_print_hex((uint8_t)mouse_report[1]);
        serial_raw_puts(" dy=");
        serial_raw_print_hex((uint8_t)mouse_report[2]);
        serial_raw_puts("\n");
#endif

        usb_hid_mouse_t *mouse = NULL;
        for (int i = 0; i < USB_HID_MAX_MICE; i++) {
            if (s_mice[i].active && (s_mice[i].slot_id == m_slot || m_slot == 0)) {
                mouse = &s_mice[i];
                break;
            }
        }
        if (!mouse) {
            for (int i = 0; i < USB_HID_MAX_MICE; i++) {
                if (s_mice[i].active) {
                    mouse = &s_mice[i];
                    break;
                }
            }
        }
        if (!mouse) continue;

        hid_mouse_event_fn_t sink = s_mouse_custom_cb ? s_mouse_custom_cb : hid_mouse_event_sink;
        void *sink_ctx = s_mouse_custom_cb ? s_mouse_custom_ctx : mouse;
        hid_mouse_process_report(mouse_report, m_len ? m_len : 3, &mouse->state, sink, sink_ctx);

        /* Re-arm the endpoint transfer ring so xHC can receive the next report */
        usb_hid_mouse_rearm(io, mouse);
    }
}

#ifndef USB_HID_HOST_TEST
static bool hid_deadline_reached(void *arg) {
    uint64_t now = apic_timer_get_bsp_ticks();
    uint64_t deadline = *(uint64_t *)arg;
    return xhci_hid_reports_pending() > 0 || xhci_hid_mouse_reports_pending() > 0 || now >= deadline;
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
    for (int i = 0; i < USB_HID_MAX_MICE; i++) {
        if (s_mice[i].active) return true;
    }
    return false;
}

bool usb_hid_mouse_is_active(void) {
    for (int i = 0; i < USB_HID_MAX_MICE; i++) {
        if (s_mice[i].active) return true;
    }
    return false;
}

bool usb_hid_get_mouse_state(uint8_t mouse_idx, hid_mouse_state_t *out_state) {
    if (mouse_idx >= USB_HID_MAX_MICE || !out_state) return false;
    if (!s_mice[mouse_idx].active) return false;
    *out_state = s_mice[mouse_idx].state;
    return true;
}
