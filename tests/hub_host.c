#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "xhci_rings.h"
#include "xhci_hub.h"
#include "hid_kbd.h"
#include "keyboard.h"

uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!pthread_mutex_lock(&l->mutex));
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags;
    assert(!pthread_mutex_unlock(&l->mutex));
}

/* ============================================================================
 * 1. Hub Route String and Topology Tests
 * ============================================================================ */

static void test_route_strings(void) {
    printf("[TEST] Testing xHCI Route String calculation...\n");

    /* Direct root device has route string 0 */
    uint32_t root_route = 0;
    assert(root_route == 0);

    /* Tier 1 hub port 1 */
    uint32_t t1_p1 = xhci_hub_calc_route_string(root_route, 1, 1);
    assert(t1_p1 == 0x00001);

    /* Tier 1 hub port 4 (e.g. Dell WD19S dock port 4) */
    uint32_t t1_p4 = xhci_hub_calc_route_string(root_route, 1, 4);
    assert(t1_p4 == 0x00004);

    /* Ports > 14 must clamp to 0xF per xHCI spec 4.3 */
    uint32_t t1_p15 = xhci_hub_calc_route_string(root_route, 1, 15);
    assert(t1_p15 == 0x0000F);
    uint32_t t1_p20 = xhci_hub_calc_route_string(root_route, 1, 20);
    assert(t1_p20 == 0x0000F);

    /* Tier 2 device on port 2 of tier 1 hub on port 3: Route String = 0x00023 */
    uint32_t t1_p3 = xhci_hub_calc_route_string(root_route, 1, 3);
    uint32_t t2_p2 = xhci_hub_calc_route_string(t1_p3, 2, 2);
    assert(t2_p2 == 0x00023);

    /* Tier 3 device on port 1 of tier 2 hub on port 2 of tier 1 hub on port 5: 0x00125 */
    uint32_t t1_p5 = xhci_hub_calc_route_string(root_route, 1, 5);
    uint32_t t2_p2_b = xhci_hub_calc_route_string(t1_p5, 2, 2);
    uint32_t t3_p1 = xhci_hub_calc_route_string(t2_p2_b, 3, 1);
    assert(t3_p1 == 0x00125);

    /* Bounds check: tier > 5 or port == 0 should not alter string */
    assert(xhci_hub_calc_route_string(t3_p1, 6, 1) == t3_p1);
    assert(xhci_hub_calc_route_string(t3_p1, 1, 0) == t3_p1);

    printf("  [PASS] Route string calculation verified across multi-tier topologies\n");
}

static void test_tt_derivation(void) {
    printf("[TEST] Testing Transaction Translator (TT) inheritance...\n");

    /* Parent Hub: Slot 1, Root Port 1, High-Speed (USB 2.0 Hub on Dell WD19S dock) */
    xhci_device_topology_t dock_hub = {
        .slot_id = 1,
        .root_port = 1,
        .speed = 3, /* High-Speed */
        .is_hub = true,
        .is_hs_hub = true,
        .tier = 1,
        .route_string = 0x00001,
        .parent_slot_id = 0,
        .parent_port_num = 0,
        .tt_slot_id = 0,
        .tt_port_num = 0,
    };

    /* Peripheral 1: Low-Speed USB Keyboard (speed 2) on dock port 1 */
    xhci_device_topology_t kbd = {0};
    xhci_hub_derive_child_topology(&dock_hub, 1, 2, &kbd);
    assert(kbd.slot_id == 0);
    assert(kbd.root_port == 1);
    assert(kbd.parent_slot_id == 1);
    assert(kbd.parent_port_num == 1);
    assert(kbd.tier == 2);
    assert(kbd.route_string == 0x00011);
    /* Crucial: Low-speed child must have dock hub as TT */
    assert(kbd.tt_slot_id == 1);
    assert(kbd.tt_port_num == 1);

    /* Peripheral 2: Full-Speed USB Mouse (speed 1) on dock port 2 */
    xhci_device_topology_t mouse = {0};
    xhci_hub_derive_child_topology(&dock_hub, 2, 1, &mouse);
    assert(mouse.parent_slot_id == 1);
    assert(mouse.parent_port_num == 2);
    assert(mouse.tt_slot_id == 1);
    assert(mouse.tt_port_num == 2);

    /* Peripheral 3: SuperSpeed USB 3.0 Storage (speed 4) on dock port 3 */
    xhci_device_topology_t storage = {0};
    xhci_hub_derive_child_topology(&dock_hub, 3, 4, &storage);
    assert(storage.parent_slot_id == 1);
    assert(storage.parent_port_num == 3);
    /* SuperSpeed requires no TT */
    assert(storage.tt_slot_id == 0);
    assert(storage.tt_port_num == 0);

    /* Peripheral 4: Nested secondary hub behind dock */
    xhci_device_topology_t nested_hub = {
        .slot_id = 4,
        .root_port = 1,
        .speed = 3, /* High-speed */
        .is_hub = true,
        .is_hs_hub = true,
        .tier = 2,
        .route_string = 0x00041,
        .parent_slot_id = 1,
        .parent_port_num = 4,
        .tt_slot_id = 1, /* inherited */
        .tt_port_num = 4,
    };
    xhci_device_topology_t deep_kbd = {0};
    xhci_hub_derive_child_topology(&nested_hub, 2, 2, &deep_kbd);
    assert(deep_kbd.tier == 3);
    assert(deep_kbd.route_string == 0x00241);
    assert(deep_kbd.tt_slot_id == 1); /* TT remains root-tier HS hub */
    assert(deep_kbd.tt_port_num == 4);

    printf("  [PASS] TT derivation and multi-tier inheritance verified\n");
}

/* ============================================================================
 * 2. xHCI Interval Formula Tests
 * ============================================================================ */

/* Implementation of the corrected interval formula */
static uint8_t calc_xhci_interval(uint8_t speed, uint8_t bInterval) {
    if (speed == 4 || speed == 5) { /* SuperSpeed */
        return (bInterval > 0) ? bInterval - 1 : 0;
    } else if (speed == 3) { /* High-Speed */
        return (bInterval > 0) ? bInterval - 1 : 0;
    } else { /* Low-Speed (2) or Full-Speed (1) */
        if (!bInterval) return 3;
        /* Period in 125 us units = bInterval * 8 */
        uint32_t units = (uint32_t)bInterval * 8;
        /* floor(log2(units)) */
        uint8_t val = 0;
        while ((1u << (val + 1)) <= units) val++;
        if (val < 3) val = 3;
        if (val > 10) val = 10;
        return val;
    }
}

static void test_interval_formula(void) {
    printf("[TEST] Testing xHCI Interval calculation...\n");

    /* Low/Full Speed tests:
     * bInterval = 1 ms -> 8 units -> floor(log2(8)) = 3 (1 ms) */
    assert(calc_xhci_interval(1, 1) == 3);
    assert(calc_xhci_interval(2, 1) == 3);

    /* bInterval = 10 ms -> 80 units -> 2^6 = 64 <= 80 < 128 = 2^7 -> 6 (8 ms) */
    assert(calc_xhci_interval(1, 10) == 6);
    assert(calc_xhci_interval(2, 10) == 6);

    /* bInterval = 32 ms -> 256 units -> 2^8 = 256 -> 8 (32 ms) */
    assert(calc_xhci_interval(1, 32) == 8);

    /* High speed tests: bInterval - 1 */
    assert(calc_xhci_interval(3, 4) == 3);
    assert(calc_xhci_interval(3, 1) == 0);

    printf("  [PASS] Interval exponent calculation verified without off-by-one\n");
}

/* ============================================================================
 * 3. Scancode Mapping & Diff-Based Report Decoder Tests
 * ============================================================================ */

typedef struct {
    uint8_t scancodes[64];
    bool    extended[64];
    size_t  count;
} test_sink_t;

static void test_sink_callback(uint8_t scancode, bool extended, void *ctx) {
    test_sink_t *s = (test_sink_t *)ctx;
    if (s->count < 64) {
        s->scancodes[s->count] = scancode;
        s->extended[s->count] = extended;
        s->count++;
    }
}

static void test_diff_decoder_basic(void) {
    printf("[TEST] Testing diff-based boot keyboard decoder (make/break)...\n");

    hid_boot_kbd_report_t prev = {0};
    hid_boot_kbd_report_t curr = {0};
    test_sink_t sink = {0};

    /* 1. Press 'a' (Usage 0x04) */
    curr.keys[0] = 0x04;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == 0x1E); /* Set 1 Make for 'a' */
    assert(!sink.extended[0]);

    /* 2. Press 'b' while holding 'a' */
    sink.count = 0;
    curr.keys[1] = 0x05; /* Usage 0x05 */
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == 0x30); /* Set 1 Make for 'b' */
    assert(!sink.extended[0]);

    /* 3. Release 'a' while holding 'b' */
    sink.count = 0;
    curr.keys[0] = 0x00; /* 'a' released */
    /* Pack remaining key into keys[0] */
    curr.keys[0] = 0x05;
    curr.keys[1] = 0x00;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == (0x1E | 0x80)); /* Set 1 Break for 'a' */

    /* 4. Release 'b' */
    sink.count = 0;
    curr.keys[0] = 0x00;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == (0x30 | 0x80)); /* Set 1 Break for 'b' */

    printf("  [PASS] Make and break generation on key transitions verified\n");
}

static void test_diff_decoder_modifiers(void) {
    printf("[TEST] Testing modifier transitions and AltGr/GUI flags...\n");

    hid_boot_kbd_report_t prev = {0};
    hid_boot_kbd_report_t curr = {0};
    test_sink_t sink = {0};

    /* Press Left Shift */
    curr.modifiers = HID_MOD_LSHIFT;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == 0x2A); /* Set 1 Make LShift */
    assert(!sink.extended[0]);

    /* Press Right Alt (AltGr) */
    sink.count = 0;
    curr.modifiers |= HID_MOD_RALT;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == 0x38); /* Set 1 Alt */
    assert(sink.extended[0]);          /* Extended = true for AltGr! */

    /* Release Left Shift */
    sink.count = 0;
    curr.modifiers &= ~HID_MOD_LSHIFT;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == (0x2A | 0x80)); /* Set 1 Break LShift */

    /* Release AltGr */
    sink.count = 0;
    curr.modifiers &= ~HID_MOD_RALT;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    assert(sink.count == 1);
    assert(sink.scancodes[0] == (0x38 | 0x80));
    assert(sink.extended[0]);

    printf("  [PASS] Modifier make/break and AltGr extended bit verified\n");
}

static void test_phantom_rollover_rejection(void) {
    printf("[TEST] Testing phantom rollover (0x01) rejection...\n");

    hid_boot_kbd_report_t prev = {0};
    hid_boot_kbd_report_t curr = {0};
    test_sink_t sink = {0};

    /* Inject ErrorRollOver (0x01 in key list) */
    curr.keys[0] = 0x01;
    curr.keys[1] = 0x04;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);
    /* Must produce zero scancodes! */
    assert(sink.count == 0);
    /* Prev must not be updated */
    assert(prev.keys[0] == 0);

    printf("  [PASS] Rollover reports safely dropped without state corruption\n");
}

static void test_self_correcting_dropped_report(void) {
    printf("[TEST] Testing self-correction against dropped reports...\n");

    hid_boot_kbd_report_t prev = {0};
    hid_boot_kbd_report_t curr = {0};
    test_sink_t sink = {0};

    /* Key 'a' pressed */
    curr.keys[0] = 0x04;
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);

    /* Suppose 'a' was released and 'b' pressed in one step, or the release report was dropped */
    sink.count = 0;
    curr.keys[0] = 0x05; /* only 'b' now */
    hid_kbd_process_report(&curr, &prev, test_sink_callback, &sink);

    /* Must emit Break for 'a' AND Make for 'b' */
    assert(sink.count == 2);
    assert(sink.scancodes[0] == (0x1E | 0x80)); /* Break 'a' */
    assert(sink.scancodes[1] == 0x30);          /* Make 'b' */

    printf("  [PASS] Self-correction on dropped report verified (no stuck keys)\n");
}

static void test_layout_parity_belgian_azerty(void) {
    printf("[TEST] Testing layout parity with existing PS/2 Set 1 decoder (AZERTY / US)...\n");

    keyboard_decoder_t kbd_state = {0};

    /* Set layout to Belgian AZERTY (Dell Latitude 5590 default) */
    keyboard_set_layout(KBD_LAYOUT_AZERTY);

    /* 1. Test letter 'a' (HID 0x04 -> Set 1 0x1E)
     * In AZERTY, physical QWERTY 'a' key (Set 1 0x1E) is 'q'! */
    char buf[4] = {0};
    size_t n = keyboard_decode_bytes(&kbd_state, 0x1E, buf);
    assert(n == 1);
    assert(buf[0] == 'q');

    /* 2. Test ISO key (HID 0x64 -> Set 1 0x56)
     * In Belgian AZERTY, physical ISO key next to LShift is '<' (unshifted) and '>' (shifted) */
    n = keyboard_decode_bytes(&kbd_state, 0x56, buf);
    assert(n == 1);
    assert(buf[0] == '<');

    /* Press Shift and test ISO key again */
    keyboard_decode_bytes(&kbd_state, 0x2A, buf); /* LShift make */
    n = keyboard_decode_bytes(&kbd_state, 0x56, buf);
    assert(n == 1);
    assert(buf[0] == '>');
    keyboard_decode_bytes(&kbd_state, 0xAA, buf); /* LShift break */

    /* 3. Test AltGr + 2 ('@' in Belgian AZERTY) */
    kbd_state.extended = true;
    keyboard_decode_bytes(&kbd_state, 0x38, buf); /* AltGr make */
    n = keyboard_decode_bytes(&kbd_state, 0x03, buf); /* '2' key (Set 1 0x03) */
    assert(n == 1);
    assert(buf[0] == '@');
    kbd_state.extended = true;
    keyboard_decode_bytes(&kbd_state, 0xB8, buf); /* AltGr break */

    /* 4. Switch to US layout */
    keyboard_set_layout(KBD_LAYOUT_US);
    memset(&kbd_state, 0, sizeof(kbd_state));

    /* 'a' key in US layout is 'a' */
    n = keyboard_decode_bytes(&kbd_state, 0x1E, buf);
    assert(n == 1);
    assert(buf[0] == 'a');

    /* Shift + '1' in US layout is '!' */
    keyboard_decode_bytes(&kbd_state, 0x2A, buf); /* LShift make */
    n = keyboard_decode_bytes(&kbd_state, 0x02, buf); /* '1' key */
    assert(n == 1);
    assert(buf[0] == '!');
    keyboard_decode_bytes(&kbd_state, 0xAA, buf); /* LShift break */

    /* 5. Cursor Arrow: Up Arrow (Set 1 0x48 extended) -> \e[A */
    kbd_state.extended = true;
    n = keyboard_decode_bytes(&kbd_state, 0x48, buf);
    assert(n == 3);
    assert(buf[0] == 27 && buf[1] == '[' && buf[2] == 'A');

    printf("  [PASS] Full layout parity, Belgian ISO key, AltGr and ANSI escapes verified\n");
}





/* ============================================================================
 * 4. Slot Context Hardware Fields & DW2 TT Registers
 * ============================================================================ */

static void test_slot_context_dw2_tt(void) {
    printf("[TEST] Testing xHCI Slot Context DW0..DW3 register encoding...\n");

    xhci_device_topology_t topo = {
        .slot_id = 2,
        .root_port = 3,
        .speed = 2, /* Low-speed keyboard */
        .is_hub = false,
        .tier = 2,
        .route_string = 0x00013,
        .tt_slot_id = 1, /* Parent High-Speed Hub on Slot 1 */
        .tt_port_num = 1,
        .ttt = 2,
    };

    uint32_t slot_ctx[4] = {0};
    xhci_hub_populate_slot_context(slot_ctx, &topo, 31, 0);

    /* DW0 checks:
     * Route String bits [19:0] == 0x00013
     * Speed bits [23:20] == 2
     * MTT bit 25 == 0 (must be 0 for single-TT!)
     * Hub bit 26 == 0 */
    assert((slot_ctx[0] & 0x000FFFFFu) == 0x00013);
    assert(((slot_ctx[0] >> 20) & 0xFu) == 2);
    assert(((slot_ctx[0] >> 25) & 1u) == 0); /* MTT = 0 */
    assert(((slot_ctx[0] >> 26) & 1u) == 0);

    /* DW1 checks:
     * Root Port bits [23:16] == 3 */
    assert(((slot_ctx[1] >> 16) & 0xFFu) == 3);

    /* DW2 checks (CRITICAL: TT fields are in DW2, not DW3!):
     * TT Hub Slot ID bits [7:0] == 1
     * TT Port Number bits [15:8] == 1
     * TTT bits [17:16] == 2 */
    assert((slot_ctx[2] & 0xFFu) == 1);
    assert(((slot_ctx[2] >> 8) & 0xFFu) == 1);
    assert(((slot_ctx[2] >> 16) & 0x3u) == 2);

    /* Test Hub itself: Slot 1, 7 ports, Hub=1 */
    xhci_device_topology_t hub_topo = {
        .slot_id = 1,
        .root_port = 3,
        .speed = 3, /* High-speed */
        .is_hub = true,
        .tier = 1,
        .route_string = 0x00003,
    };
    uint32_t hub_ctx[4] = {0};
    xhci_hub_populate_slot_context(hub_ctx, &hub_topo, 1, 7);
    assert(((hub_ctx[0] >> 26) & 1u) == 1); /* Hub = 1 */
    assert(((hub_ctx[0] >> 25) & 1u) == 0); /* MTT = 0 */
    assert(((hub_ctx[1] >> 24) & 0xFFu) == 7); /* Number of Ports = 7 */
    assert(hub_ctx[2] == 0); /* Hub has no TT */

    printf("  [PASS] Verified TT fields in DW2, MTT=0, and Hub fields in DW0/DW1\n");
}

/* ============================================================================
 * 5. Real Kernel Rings Integration & Hardware Mock
 * ============================================================================ */

typedef struct {
    uint32_t regs[0x4000 / 4];
    _Atomic uint32_t erdp_writes;
    _Atomic uint32_t erdp_ehb_violations;
    _Atomic uint64_t last_erdp;
} mock_xhci_hw_t;

static uint32_t real_test_mock_read32(void *ctx, uint32_t off) {
    mock_xhci_hw_t *m = ctx;
    assert(!(off & 3) && (off / 4) < (sizeof(m->regs) / 4));
    return m->regs[off / 4];
}

static void real_test_mock_write32(void *ctx, uint32_t off, uint32_t val) {
    mock_xhci_hw_t *m = ctx;
    assert(!(off & 3) && (off / 4) < (sizeof(m->regs) / 4));
    m->regs[off / 4] = val;

    /* Primary Interrupter ERDP low (rtsoff 0x1000 + 0x20 + 0x18 = 0x1038) */
    if (off == 0x1038) {
        /* Check bit 3: Event Handler Busy (EHB) must be cleared by writing 1! */
        if ((val & (1u << 3)) == 0) {
            atomic_fetch_add_explicit(&m->erdp_ehb_violations, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&m->erdp_writes, 1, memory_order_relaxed);
        uint64_t cur = atomic_load_explicit(&m->last_erdp, memory_order_relaxed);
        uint64_t updated = (cur & 0xFFFFFFFF00000000ULL) | ((uint64_t)(val & ~0xFu));
        atomic_store_explicit(&m->last_erdp, updated, memory_order_release);
    } else if (off == 0x103C) {
        uint64_t cur = atomic_load_explicit(&m->last_erdp, memory_order_relaxed);
        uint64_t updated = (cur & 0x00000000FFFFFFFFULL) | (((uint64_t)val) << 32);
        atomic_store_explicit(&m->last_erdp, updated, memory_order_release);
    }
}

static bool real_test_mock_delay_us(void *ctx, unsigned us) {
    (void)ctx;
    (void)us;
    sched_yield();
    return true;
}

static bool real_test_mock_delay_ms(void *ctx) {
    (void)ctx;
    sched_yield();
    return true;
}

static void test_real_kernel_dispatcher_single_thread(void) {
    printf("[TEST] Testing REAL kernel xhci_event_ring_pump() demux & ERDP/EHB...\n");

    mock_xhci_hw_t hw;
    memset(&hw, 0, sizeof(hw));
    hw.regs[0x18 / 4] = 0x1000; /* RTSOFF */

    xhci_trb_t cmd_ring[XHCI_RING_TRB_COUNT];
    xhci_trb_t event_ring[XHCI_RING_TRB_COUNT];
    xhci_erst_entry_t erst[1];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));

    xhci_dma_buffers_t dma = {
        .cmd_ring_virt = cmd_ring,
        .event_ring_virt = event_ring,
        .erst_virt = erst,
        .cmd_ring_phys = 0x100000,
        .event_ring_phys = 0x200000,
        .erst_phys = 0x300000,
        .event_dequeue_idx = 0,
        .event_cycle = 1,
    };

    xhci_rings_io_t io = {
        .mmio_ctx = &hw,
        .mmio_size = sizeof(hw.regs),
        .read32 = real_test_mock_read32,
        .write32 = real_test_mock_write32,
        .delay_us = real_test_mock_delay_us,
        .delay_ms = real_test_mock_delay_ms,
    };

    uint8_t kbd_buf[8] = {0};
    xhci_hid_register_endpoint(3, 3, 0x3000, kbd_buf);

    /* 1. Command Completion Event (Slot 0, pointer 0x1050) */
    event_ring[0].parameter_low = 0x1050;
    event_ring[0].parameter_high = 0;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << 10) | XHCI_TRB_C;

    /* 2. BOT Storage Transfer Event (Slot 2, DCI 3, pointer 0x2050) */
    event_ring[1].parameter_low = 0x2050;
    event_ring[1].parameter_high = 0;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) | (3u << 16) | (2u << 24) | XHCI_TRB_C;

    /* 3. HID Keyboard Transfer Event (Slot 3, DCI 3, pointer 0x3050) */
    kbd_buf[2] = 0x04; /* 'a' pressed */
    event_ring[2].parameter_low = 0x3050;
    event_ring[2].parameter_high = 0;
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) | (3u << 16) | (3u << 24) | XHCI_TRB_C;

    /* 4. Port Status Change Event (Port 2) */
    event_ring[3].parameter_low = (2u << 24);
    event_ring[3].parameter_high = 0;
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (XHCI_TRB_TYPE_PORT_STATUS_EVENT << 10) | XHCI_TRB_C;

    /* Pump events using the real kernel function under g_xhci_ring_lock */
    uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
    xhci_event_ring_pump(&io, &dma);
    spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

    assert(dma.event_dequeue_idx == 4);
    assert(atomic_load_explicit(&hw.erdp_writes, memory_order_relaxed) == 4);
    assert(atomic_load_explicit(&hw.erdp_ehb_violations, memory_order_relaxed) == 0);

    /* Verify BOT transfer pop via real xhci_mbox_wait_transfer */
    uint32_t residual = 0;
    uint8_t comp_code = 0;
    bool bot_ok = xhci_mbox_wait_transfer(&io, &dma, 2, 3, 0x2050, &residual, &comp_code, 100);
    assert(bot_ok);
    assert(comp_code == XHCI_COMP_SUCCESS);
    assert(xhci_mbox_drop_count(2, 3) == 0);

    /* Verify Command pop via real xhci_mbox_wait_command */
    xhci_trb_t cmd_ev = {0};
    bool cmd_ok = xhci_mbox_wait_command(&io, &dma, 0x1050, &cmd_ev, 100);
    assert(cmd_ok);
    assert(cmd_ev.parameter_low == 0x1050);
    assert(xhci_cmd_mbox_drop_count() == 0);

    /* Verify HID report pop via real xhci_hid_fifo_pop */
    uint8_t out_rep[8] = {0};
    bool hid_ok = xhci_hid_fifo_pop(out_rep);
    assert(hid_ok);
    assert(out_rep[2] == 0x04);
    assert(xhci_hid_reports_pending() == 0);

    /* Verify Port Status count */
    assert(xhci_port_change_count() == 1);

    printf("  [PASS] Real kernel pump decoded all 4 event types with exact ERDP & EHB compliance\n");
}

/* ============================================================================
 * 6. Real Kernel Concurrency, ERDP Wrapping, Fault Timing & TSan Multi-Core
 * ============================================================================ */

static void test_sink_noop(uint8_t scancode, bool extended, void *ctx) {
    (void)scancode;
    (void)extended;
    (void)ctx;
}

typedef struct {
    mock_xhci_hw_t     *hw;
    xhci_dma_buffers_t *dma;
    xhci_rings_io_t    *io;
    uint8_t            *kbd_buf;
    uint32_t            total_events;

    _Atomic int         done;
    _Atomic uint32_t    enq_idx;
    uint32_t            enq_cycle;

    _Atomic uint32_t    produced_cnt;
    _Atomic uint32_t    produced_bot_cnt;
    _Atomic uint32_t    produced_cmd_cnt;
    _Atomic uint32_t    bot_consumed_cnt;
    _Atomic uint32_t    cmd_consumed_cnt;
    _Atomic uint32_t    hid_consumed_cnt;
    _Atomic uint32_t    stall_detected_cnt;
} stress_ctx_t;

static void *stress_producer_thread(void *arg) {
    stress_ctx_t *ctx = arg;
    xhci_dma_buffers_t *dma = ctx->dma;

    for (uint32_t i = 0; i < ctx->total_events; ++i) {
        uint32_t cur_enq = atomic_load_explicit(&ctx->enq_idx, memory_order_relaxed);

        /* Ring flow control: wait if ring is full */
        while (((cur_enq + 1) % XHCI_RING_TRB_COUNT) ==
               atomic_load_explicit((_Atomic uint32_t *)&dma->event_dequeue_idx, memory_order_acquire)) {
            sched_yield();
        }

        xhci_trb_t *t = &dma->event_ring_virt[cur_enq];
        uint32_t sel = i % 4;

        if (sel == 0) {
            /* Flow control: BOT in-flight requests bounded to 8 (mailbox depth is 32) */
            while (atomic_load_explicit(&ctx->produced_bot_cnt, memory_order_relaxed) -
                   atomic_load_explicit(&ctx->bot_consumed_cnt, memory_order_relaxed) >= 8) {
                sched_yield();
            }

            /* BOT Bulk-IN transfer event on Slot 2, DCI 3 */
            t->parameter_low = 0x20000000u + i;
            t->parameter_high = 0;
            if (i == 40) {
                /* Injected STALL error to verify fault routing isolation */
                t->status = (XHCI_COMP_STALL_ERROR << 24);
            } else {
                t->status = (XHCI_COMP_SUCCESS << 24);
            }
            uint32_t ctrl = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) | (3u << 16) | (2u << 24) |
                            (ctx->enq_cycle ? XHCI_TRB_C : 0);
            __atomic_store_n(&t->control, ctrl, __ATOMIC_RELEASE);
            atomic_fetch_add_explicit(&ctx->produced_bot_cnt, 1, memory_order_relaxed);
        } else if (sel == 1) {
            /* HID Keyboard transfer event on Slot 3, DCI 3 */
            uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
            if (i >= ctx->total_events - 4) {
                /* Final release report: all keys up */
                memset(ctx->kbd_buf, 0, 8);
            } else {
                ctx->kbd_buf[0] = 0;
                ctx->kbd_buf[2] = 0x04 + (uint8_t)(i % 10); /* cycling keys */
            }
            spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

            t->parameter_low = 0x30000000u + i;
            t->parameter_high = 0;
            t->status = (XHCI_COMP_SUCCESS << 24);
            uint32_t ctrl = (XHCI_TRB_TYPE_TRANSFER_EVENT << 10) | (3u << 16) | (3u << 24) |
                            (ctx->enq_cycle ? XHCI_TRB_C : 0);
            __atomic_store_n(&t->control, ctrl, __ATOMIC_RELEASE);
        } else if (sel == 2) {
            /* Flow control: Command completions serialized (bounded to 4) */
            while (atomic_load_explicit(&ctx->produced_cmd_cnt, memory_order_relaxed) -
                   atomic_load_explicit(&ctx->cmd_consumed_cnt, memory_order_relaxed) >= 4) {
                sched_yield();
            }

            /* Command Completion Event */
            t->parameter_low = 0x10000000u + i;
            t->parameter_high = 0;
            t->status = (XHCI_COMP_SUCCESS << 24);
            uint32_t ctrl = (XHCI_TRB_TYPE_CMD_COMPLETION_EVENT << 10) |
                            (ctx->enq_cycle ? XHCI_TRB_C : 0);
            __atomic_store_n(&t->control, ctrl, __ATOMIC_RELEASE);
            atomic_fetch_add_explicit(&ctx->produced_cmd_cnt, 1, memory_order_relaxed);
        } else {
            /* Port Status Change Event */
            t->parameter_low = ((1u + (i % 4)) << 24);
            t->parameter_high = 0;
            t->status = (XHCI_COMP_SUCCESS << 24);
            uint32_t ctrl = (XHCI_TRB_TYPE_PORT_STATUS_EVENT << 10) |
                            (ctx->enq_cycle ? XHCI_TRB_C : 0);
            __atomic_store_n(&t->control, ctrl, __ATOMIC_RELEASE);
        }

        cur_enq = (cur_enq + 1) % XHCI_RING_TRB_COUNT;
        if (cur_enq == 0) ctx->enq_cycle ^= 1;
        atomic_store_explicit(&ctx->enq_idx, cur_enq, memory_order_release);

        atomic_fetch_add_explicit(&ctx->produced_cnt, 1, memory_order_relaxed);
        if (i % 2 == 0) sched_yield();
    }

    atomic_store_explicit(&ctx->done, 1, memory_order_release);
    return NULL;
}

static void *stress_bot_thread(void *arg) {
    stress_ctx_t *ctx = arg;
    while (!atomic_load_explicit(&ctx->done, memory_order_acquire)) {
        uint32_t res = 0;
        uint8_t code = 0;
        if (xhci_mbox_wait_transfer(ctx->io, ctx->dma, 2, 3, 0 /* any */, &res, &code, 200)) {
            atomic_fetch_add_explicit(&ctx->bot_consumed_cnt, 1, memory_order_relaxed);
        } else if (code == XHCI_COMP_STALL_ERROR) {
            atomic_fetch_add_explicit(&ctx->bot_consumed_cnt, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&ctx->stall_detected_cnt, 1, memory_order_relaxed);
        }
        sched_yield();
    }

    /* Final drain */
    for (int attempts = 0; attempts < 50000; ++attempts) {
        uint32_t res = 0;
        uint8_t code = 0;
        bool got = false;
        if (xhci_mbox_wait_transfer(ctx->io, ctx->dma, 2, 3, 0, &res, &code, 200)) {
            atomic_fetch_add_explicit(&ctx->bot_consumed_cnt, 1, memory_order_relaxed);
            got = true;
        } else if (code == XHCI_COMP_STALL_ERROR) {
            atomic_fetch_add_explicit(&ctx->bot_consumed_cnt, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&ctx->stall_detected_cnt, 1, memory_order_relaxed);
            got = true;
        }
        if (!got &&
            atomic_load_explicit(&ctx->enq_idx, memory_order_acquire) ==
            atomic_load_explicit((_Atomic uint32_t *)&ctx->dma->event_dequeue_idx, memory_order_acquire)) {
            break;
        }
        sched_yield();
    }
    return NULL;
}

static void *stress_cmd_thread(void *arg) {
    stress_ctx_t *ctx = arg;
    while (!atomic_load_explicit(&ctx->done, memory_order_acquire)) {
        xhci_trb_t ev = {0};
        if (xhci_mbox_wait_command(ctx->io, ctx->dma, 0 /* any */, &ev, 200)) {
            atomic_fetch_add_explicit(&ctx->cmd_consumed_cnt, 1, memory_order_relaxed);
        }
        sched_yield();
    }

    /* Final drain */
    for (int attempts = 0; attempts < 50000; ++attempts) {
        xhci_trb_t ev = {0};
        bool got = false;
        if (xhci_mbox_wait_command(ctx->io, ctx->dma, 0, &ev, 200)) {
            atomic_fetch_add_explicit(&ctx->cmd_consumed_cnt, 1, memory_order_relaxed);
            got = true;
        }
        if (!got &&
            atomic_load_explicit(&ctx->enq_idx, memory_order_acquire) ==
            atomic_load_explicit((_Atomic uint32_t *)&ctx->dma->event_dequeue_idx, memory_order_acquire)) {
            break;
        }
        sched_yield();
    }
    return NULL;
}

static void *stress_hid_worker_thread(void *arg) {
    stress_ctx_t *ctx = arg;
    hid_boot_kbd_report_t prev_rep = {0};

    while (!atomic_load_explicit(&ctx->done, memory_order_acquire)) {
        uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
        xhci_event_ring_pump(ctx->io, ctx->dma);
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

        uint8_t rep[8];
        while (xhci_hid_fifo_pop(rep)) {
            atomic_fetch_add_explicit(&ctx->hid_consumed_cnt, 1, memory_order_relaxed);
            hid_kbd_process_report((const hid_boot_kbd_report_t *)rep, &prev_rep, test_sink_noop, NULL);
        }
        sched_yield();
    }

    /* Final drain */
    for (int attempts = 0; attempts < 50000; ++attempts) {
        uint64_t flags = spin_lock_irqsave(&g_xhci_ring_lock);
        xhci_event_ring_pump(ctx->io, ctx->dma);
        spin_unlock_irqrestore(&g_xhci_ring_lock, flags);

        uint8_t rep[8];
        bool got = false;
        while (xhci_hid_fifo_pop(rep)) {
            atomic_fetch_add_explicit(&ctx->hid_consumed_cnt, 1, memory_order_relaxed);
            hid_kbd_process_report((const hid_boot_kbd_report_t *)rep, &prev_rep, test_sink_noop, NULL);
            got = true;
        }

        if (!got &&
            atomic_load_explicit(&ctx->enq_idx, memory_order_acquire) ==
            atomic_load_explicit((_Atomic uint32_t *)&ctx->dma->event_dequeue_idx, memory_order_acquire)) {
            break;
        }
        sched_yield();
    }

    /* Verify final physical key state: all keys up */
    assert(prev_rep.modifiers == 0);
    for (int k = 0; k < 6; ++k) {
        assert(prev_rep.keys[k] == 0);
    }
    return NULL;
}

static void run_real_kernel_stress_step(uint32_t total_events) {
    mock_xhci_hw_t hw;
    memset(&hw, 0, sizeof(hw));
    hw.regs[0x18 / 4] = 0x1000; /* RTSOFF */

    xhci_trb_t cmd_ring[XHCI_RING_TRB_COUNT];
    xhci_trb_t event_ring[XHCI_RING_TRB_COUNT];
    xhci_erst_entry_t erst[1];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));

    xhci_dma_buffers_t dma = {
        .cmd_ring_virt = cmd_ring,
        .event_ring_virt = event_ring,
        .erst_virt = erst,
        .cmd_ring_phys = 0x100000,
        .event_ring_phys = 0x200000,
        .erst_phys = 0x300000,
        .event_dequeue_idx = 0,
        .event_cycle = 1,
    };

    xhci_rings_io_t io = {
        .mmio_ctx = &hw,
        .mmio_size = sizeof(hw.regs),
        .read32 = real_test_mock_read32,
        .write32 = real_test_mock_write32,
        .delay_us = real_test_mock_delay_us,
        .delay_ms = real_test_mock_delay_ms,
    };

    uint8_t kbd_buf[8] = {0};
    xhci_hid_register_endpoint(3, 3, 0x3000, kbd_buf);

    stress_ctx_t ctx = {
        .hw = &hw,
        .dma = &dma,
        .io = &io,
        .kbd_buf = kbd_buf,
        .total_events = total_events,
        .done = 0,
        .enq_idx = 0,
        .enq_cycle = 1,
        .produced_cnt = 0,
        .produced_bot_cnt = 0,
        .produced_cmd_cnt = 0,
        .bot_consumed_cnt = 0,
        .cmd_consumed_cnt = 0,
        .hid_consumed_cnt = 0,
        .stall_detected_cnt = 0,
    };

    pthread_t prod_th, bot_th, cmd_th, hid_th;
    pthread_create(&prod_th, NULL, stress_producer_thread, &ctx);
    pthread_create(&bot_th, NULL, stress_bot_thread, &ctx);
    pthread_create(&cmd_th, NULL, stress_cmd_thread, &ctx);
    pthread_create(&hid_th, NULL, stress_hid_worker_thread, &ctx);

    pthread_join(prod_th, NULL);
    pthread_join(bot_th, NULL);
    pthread_join(cmd_th, NULL);
    pthread_join(hid_th, NULL);

    uint32_t prod = atomic_load_explicit(&ctx.produced_cnt, memory_order_relaxed);
    uint32_t bot = atomic_load_explicit(&ctx.bot_consumed_cnt, memory_order_relaxed);
    uint32_t cmd = atomic_load_explicit(&ctx.cmd_consumed_cnt, memory_order_relaxed);
    uint32_t hid = atomic_load_explicit(&ctx.hid_consumed_cnt, memory_order_relaxed);
    uint32_t port = xhci_port_change_count();
    uint32_t stalls = atomic_load_explicit(&ctx.stall_detected_cnt, memory_order_relaxed);

    uint32_t bot_drops = xhci_mbox_drop_count(2, 3);
    uint32_t cmd_drops = xhci_cmd_mbox_drop_count();
    uint32_t hid_drops = xhci_hid_fifo_drop_count();

    uint32_t erdp_writes = atomic_load_explicit(&hw.erdp_writes, memory_order_relaxed);
    uint32_t ehb_viol = atomic_load_explicit(&hw.erdp_ehb_violations, memory_order_relaxed);

    printf("  [STATS: %u events] Produced=%u, BOT=%u (drops=%u), CMD=%u (drops=%u), HID=%u (drops=%u), Port=%u, Stalls=%u, ERDP_writes=%u\n",
           total_events, prod, bot, bot_drops, cmd, cmd_drops, hid, hid_drops, port, stalls, erdp_writes);
    fflush(stdout);

    /* HARD INVARIANTS */
    assert(prod == total_events);
    assert(bot_drops == 0); /* BOT storage must NEVER drop! */
    assert(cmd_drops == 0); /* Commands must NEVER drop! */
    assert(ehb_viol == 0);  /* ERDP write bit 3 (EHB=1) must never be violated! */
    assert(erdp_writes > 0);
    assert(stalls == 1);    /* Injected STALL error correctly detected by BOT */
}

static void test_real_kernel_concurrency_stress(void) {
    printf("[TEST] Testing REAL kernel xhci_rings.c multi-threaded concurrency, EHB, & STALL fault timing...\n");
    printf("  --> Step 1: 200 events\n");
    run_real_kernel_stress_step(200);
    printf("  --> Step 2: 500 events\n");
    run_real_kernel_stress_step(500);
    printf("  --> Step 3: 2000 events (multiple ring wraps)\n");
    run_real_kernel_stress_step(2000);
    printf("  [PASS] Real kernel rings concurrency, ERDP wrapping, EHB compliance, and zero-drop storage verified\n");
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int main(void) {
    printf("========================================================\n");
    printf("FortressOS USB Hub & HID Host Verification Suite\n");
    printf("========================================================\n");

    test_route_strings();
    test_tt_derivation();
    test_slot_context_dw2_tt();
    test_interval_formula();
    test_diff_decoder_basic();
    test_diff_decoder_modifiers();
    test_phantom_rollover_rejection();
    test_self_correcting_dropped_report();
    test_layout_parity_belgian_azerty();
    test_real_kernel_dispatcher_single_thread();
    test_real_kernel_concurrency_stress();

    printf("========================================================\n");
    printf("ALL USB HUB & HID HOST VERIFICATION TESTS PASSED (100%%)\n");
    printf("========================================================\n");
    return 0;
}
