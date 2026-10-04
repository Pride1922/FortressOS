/* Actual gated PCH path, deterministic register/time/PCI mocks. No claim
 * about real PHY, firmware, DMA, timing or wire behavior. */
#include <stdint.h>
#include <stdbool.h>
static uint32_t e1000_mock_read(uintptr_t base, uint32_t reg);
static void e1000_mock_write(uintptr_t base, uint32_t reg, uint32_t value);
static bool e1000_mock_delay(unsigned ms);
static uintptr_t e1000_mock_vtd_map(uintptr_t phys, unsigned index, unsigned bytes);
static void e1000_mock_log(const char *s);
static unsigned test_lock_depth;
static bool test_refuse_master_disable;
#define E1000_PCH_HOST_TEST 1
#define main net_rings_regression_main
#include "net_rings_host.c"
#undef main

static bool deny_ownership, reset_stuck, pending_stuck, post_timer_fail;
static bool reset_waiting;
static bool complete_tdt;
static bool lose_link_after_reset;
static bool cold_timer_fail;
static uint32_t ignored_write;
static unsigned reset_writes, elapsed_ms, log_calls;
static char captured_log[8192];
static size_t captured_length;
static uint32_t mock_vtd[0x6000 / 4];
static bool vtd_map_fail;
static unsigned tx_waits, complete_after;
static bool tx_timer_fail;
static unsigned rx_waits;
static bool rx_inject, rx_bad;
static uint32_t reset_tctl_default;
static uint16_t mock_phy[3][32], mock_phy_page;
static bool mdic_stuck, mdic_error, phy_ignore_write;
static uintptr_t e1000_mock_vtd_map(uintptr_t phys, unsigned index, unsigned bytes) {
    (void)phys; (void)index; assert(bytes <= sizeof(mock_vtd));
    if (vtd_map_fail) return 0;
    return (uintptr_t)mock_vtd;
}

static uint32_t e1000_mock_read(uintptr_t base, uint32_t reg) {
    assert(!reset_waiting); /* No MMIO reads during the 20ms reset blackout. */
    assert(reg < sizeof(registers));
    return *(uint32_t *)(base + reg);
}
static void e1000_mock_write(uintptr_t base, uint32_t reg, uint32_t value) {
    assert(!reset_waiting);
    assert(reg < sizeof(registers));
    if (reg == ignored_write) return;
    if (is_i219() && reg == E1000_REG_TCTL && (value & (1u << 1)) &&
        ignored_write != PCH_FEXTNVM11)
        assert(registers[PCH_FEXTNVM11 / 4] & (1u << 13));
    if (reg == PCH_EXTCNF && deny_ownership) value &= ~PCH_SWFLAG;
    if (reg == PCH_MDIC) {
        assert(registers[PCH_EXTCNF / 4] & PCH_SWFLAG);
        assert(((value >> 21) & 31) == 1);
        unsigned phy_reg = (value >> 16) & 31;
        if (!mdic_stuck) {
            if (mdic_error) value |= 0x40000000u;
            else if (phy_reg == 31) {
                if (value & 0x04000000u) mock_phy_page = (uint16_t)value;
                else value = (value & ~65535u) | mock_phy_page;
            } else {
                unsigned page = mock_phy_page >> 5;
                unsigned index = page == 770 ? 0 : page == 772 ? 1 : 2;
                assert(page == 770 || page == 772 || page == 776);
                if (value & 0x04000000u) {
                    if (!phy_ignore_write) mock_phy[index][phy_reg] = (uint16_t)value;
                } else value = (value & ~65535u) | mock_phy[index][phy_reg];
            }
            value |= 0x10000000u;
        }
    }
    *(uint32_t *)(base + reg) = value;
    if (reg == REG_TDT && complete_tdt && s_tx) {
        unsigned slot = (value + NET_RING_COUNT - 1) % NET_RING_COUNT;
        assert(s_tx[slot].buffer_addr == s_tx_pages[slot]);
        assert(s_tx[slot].length == 60 && s_tx[slot].cmd == 0x0b);
        assert(s_tx[slot].status == 0);
        s_tx[slot].status = DESC_DD;
    }
    if (reg == E1000_REG_CTRL && (value & PCH_MASTER_DISABLE) && !pending_stuck)
        registers[E1000_REG_STATUS / 4] &= ~PCH_MASTER_ACTIVE;
    if (reg == E1000_REG_CTRL && (value & PCH_RESET)) {
        assert(!(value & PCH_PHY_RESET));
        ++reset_writes;
        reset_waiting = true;
    }
}
static bool e1000_mock_delay(unsigned ms) {
    if (cold_timer_fail && ms==1 && !s_pch_attempted) return false;
    if (ms == 1 && (s_net_dev.flags & NET_UP) && !s_tx_pending) {
        ++rx_waits;
        if (rx_inject && rx_waits == 3) {
            unsigned char *frame = s_rx_packets[s_rx_slot[0]]->data;
            memset(frame, 0xa5, 60);
            memcpy(frame, s_net_dev.mac_addr, 6);
            frame[12] = 0x88; frame[13] = 0xb5;
            memcpy(frame + 14, "FORTRESS-NET-2B-RX", 18);
            s_rx[0].length = 60; s_rx[0].errors = rx_bad ? 1 : 0;
            s_rx[0].status = DESC_DD | RX_EOP;
            registers[REG_RDH / 4] = 1;
        }
    }
    if (ms == 1 && (s_net_dev.flags & NET_UP) && s_tx_pending) {
        assert(!test_lock_depth);
        ++tx_waits;
        if (tx_timer_fail) return false;
        if (complete_after && tx_waits == complete_after) s_tx[0].status = DESC_DD;
    }
    elapsed_ms += ms;
    if (reset_waiting) {
        assert(ms == 20);
        if (post_timer_fail) return false;
        reset_waiting = false;
        if (!reset_stuck) {
            registers[E1000_REG_CTRL / 4] &= ~PCH_RESET;
            registers[E1000_REG_TCTL / 4] = reset_tctl_default;
            /* Dell log shows the pre-reset MULR workaround is lost. */
            registers[PCH_FEXTNVM11 / 4] &= ~(1u << 13);
        }
        if (lose_link_after_reset) registers[E1000_REG_STATUS / 4] &= ~E1000_STATUS_LU;
    }
    return true;
}
static void e1000_mock_log(const char *s) {
    assert(!test_lock_depth); /* Reports must follow device-lock release. */
    size_t length = strlen(s);
    if (captured_length + length < sizeof(captured_log)) {
        memcpy(captured_log + captured_length, s, length + 1);
        captured_length += length;
    }
    ++log_calls;
}
static void pch_fixture(uint16_t id) {
    fixture();
    s_e1000_dev.pci.device_id = id;
    s_e1000_dev.aperture_size = sizeof(registers);
    const uint8_t mac[6] = {0xc8, 0xf7, 0x50, 0x0e, 0x35, 0x80};
    memcpy(s_e1000_dev.mac_addr, mac, sizeof(mac));
    registers[E1000_REG_STATUS / 4] = E1000_STATUS_LU | E1000_STATUS_FD |
                                    E1000_STATUS_SPEED_1000 | PCH_MASTER_ACTIVE;
    memset(&s_pch_failure, 0, sizeof(s_pch_failure));
    memset(&s_pch_before_reset, 0, sizeof(s_pch_before_reset));
    s_pch_failed = s_pch_attempted = false;
    s_pch_mmio_safe = true;
    s_pch_reset_error = s_pch_stop_result = s_pch_stop_reset_error = NULL;
    deny_ownership = reset_stuck = pending_stuck = post_timer_fail = reset_waiting = false;
    ignored_write = UINT32_MAX;
    complete_tdt = false;
    test_refuse_master_disable = lose_link_after_reset = false;
    cold_timer_fail=false;
    reset_writes = elapsed_ms = log_calls = 0;
    captured_length = 0;
    captured_log[0] = 0;
    tx_waits = complete_after = 0;
    mock_poll_hz = mock_poll_now = 0;
    mock_poll_reads = mock_complete_read = 0;
    mock_poll_stalled = mock_poll_backward = false;
    tx_timer_fail = vtd_map_fail = false;
    rx_waits = 0; rx_inject = rx_bad = false;
    reset_tctl_default = 0;
    mdic_stuck = mdic_error = phy_ignore_write = s_pch_tx_trial = false;
    s_tx_report_done = false; /* Each fixture represents a fresh boot. */
    s_tx_bringup_report = false;
    memset(mock_phy, 0, sizeof(mock_phy)); mock_phy_page = 0x120;
    memset(mock_vtd, 0, sizeof(mock_vtd));
    assert(!test_lock_depth);
}
static void stopped(unsigned expected_allocations) {
    assert(g_net_fatal && !e1000_get_net_device());
    assert(!(command_value & PCI_COMMAND_BUS_MASTER));
    assert(allocations == expected_allocations && s_pch_failed && log_calls);
    pch_snapshot_t saved = s_pch_failure;
    unsigned writes = reset_writes;
    assert(!e1000_i219_init());
    assert(!e1000_quiesce());
    assert(reset_writes == writes && !memcmp(&saved, &s_pch_failure, sizeof(saved)));
}
int main(void) {
    pch_fixture(E1000_DEV_I219_LM_15D7);
    reset_tctl_default = 0x30000000u;
    registers[0x3940 / 4] = 1u << 28;
    assert(e1000_i219_init());
    assert(registers[E1000_REG_TCTL / 4] == 0x3103f0fa);
    assert(!(registers[0x3940 / 4] & (1u << 28)));
    assert((registers[0x3940 / 4] & 0x45000000u) == 0x45000000u);
    puts("[PASS] post-reset TCTL bits28/29 retained and TARC1 consistent with MULR");
    fixture();
    uintptr_t tables[4];
    for (unsigned i = 0; i < 4; ++i) tables[i] = pmm_alloc_page();
    uintptr_t frame_page = pmm_alloc_page();
    uintptr_t frame_alias = (uintptr_t)vmm_phys_to_virt(frame_page);
    uint64_t *root = vmm_phys_to_virt(tables[0]);
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t *table = vmm_phys_to_virt(tables[i]);
        table[(frame_alias >> (39 - 9 * i)) & 511] = tables[i + 1] | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *leaf = vmm_phys_to_virt(tables[3]);
    unsigned leaf_index = (frame_alias >> 12) & 511;
    leaf[leaf_index] = frame_page | PTE_PRESENT | PTE_WRITABLE | PTE_NX;
    pch_page_view_t view = pch_page_view(root, frame_alias);
    assert(view.mapped && view.level == 3 && view.phys == frame_page);
    view = pch_page_view(root, frame_alias + 4095);
    assert(view.mapped && view.phys == frame_page + 4095);
    leaf[leaf_index] &= ~PTE_PRESENT;
    assert(!pch_page_view(root, frame_alias).mapped);
    uint64_t *pd = vmm_phys_to_virt(tables[2]);
    pd[(frame_alias >> 21) & 511] = 0x200000 | PTE_PRESENT | PTE_HUGE;
    view = pch_page_view(root, frame_alias);
    assert(view.mapped && view.level == 2 && view.phys == (0x200000 | (frame_alias & 0x1fffff)));
    puts("[PASS] diagnostic CPU page walk: 4KiB first/last, absent leaf and 2MiB leaf");
    pch_fixture(E1000_DEV_I219_LM_15D7);
    mock_phy[1][28] = 0xf800; mock_phy[0][17] = 0x4000;
    mock_phy[2][20] = 0xa003; registers[0xf18 / 4] = 0x80000000u;
    assert(e1000_i219_init());
    assert(mock_phy[1][28] == 0xf8fa && mock_phy[0][17] == 0x4200);
    assert(mock_phy[2][20] == 0xa063 && mock_phy_page == 0x120);
    assert(!(registers[PCH_EXTCNF / 4] & PCH_SWFLAG));
    assert((registers[0x10 / 4] & 0x80000000u) && (registers[0x24 / 4] & 7) == 7);
    complete_after = 1;
    e1000_raw_selftest("net_test=rings net_tx_trial=1");
    assert(strstr(captured_log, "TX PASS") && !diagnostic_dmar_reads);
    assert(strstr(captured_log, "TX descriptor 00000000 physical-derived CPU PRE-TDT bytes:"));
    assert(strstr(captured_log, "TX descriptor 00000001 physical-derived CPU PRE-TDT bytes:"));
    assert(!strstr(captured_log, "PCI/VT-d"));
    assert(strstr(captured_log, "TARC0 SPT request field (expected 20000000)=20000000"));
    assert(strstr(captured_log, "tx-reg 00000410 = 00602008"));
    assert(strstr(captured_log, "TIPG IPGT/IPGR1/IPGR2 (hex)=00000008/00000008/00000006"));
    captured_length = 0; captured_log[0] = 0;
    complete_tdt = true;
    uint8_t quiet_frame[60] = {0};
    for (unsigned i = 0; i < NET_RING_COUNT; ++i)
        assert(e1000_send_raw(&s_net_dev, quiet_frame, sizeof(quiet_frame)) == 0);
    assert(!strstr(captured_log, "[NET 2b]"));
    pch_fixture(E1000_DEV_I219_LM_15D7); assert(e1000_i219_init());
    captured_length = 0; captured_log[0] = 0;
    complete_tdt = true;
    for (unsigned i = 0; i <= NET_RING_COUNT; ++i)
        assert(e1000_send_raw(&s_net_dev, quiet_frame, sizeof(quiet_frame)) == 0);
    assert(!strstr(captured_log, "[NET 2b]") && !s_tx_report_done);
    puts("[PASS] normal first TX and ring wrap are quiet; selftest retains one-time TX evidence");
    pch_fixture(E1000_DEV_I219_LM_15D7); assert(e1000_i219_init());
    e1000_raw_selftest("net_test=rings net_tx_trial=1");
    const char *timeout_core = strstr(captured_log, "TX TIMEOUT, captured BEFORE containment");
    assert(timeout_core && strstr(timeout_core, "tx-reg 00000410 = 00602008"));
    assert(strstr(timeout_core, "tx-reg 00000400 = 0103F0FA"));
    assert(strstr(timeout_core, "tx-reg 00003828 = 0141011F"));
    assert(!diagnostic_dmar_reads); stopped(146);
    pch_fixture(E1000_DEV_I219_LM_15D7); mock_phy[2][20] = 0xa0a3;
    assert(e1000_i219_init() && mock_phy[2][20] == 0xa0a3);
    pch_fixture(E1000_DEV_I219_LM_15D7);
    registers[E1000_REG_STATUS / 4] = E1000_STATUS_LU | E1000_STATUS_FD |
                                    E1000_STATUS_SPEED_100 | PCH_MASTER_ACTIVE;
    mock_phy[0][17] = 0x4000;
    assert(e1000_i219_init() && mock_phy[1][28] == 0x03e8);
    assert(mock_phy[0][17] == 0x4000 && mock_phy[2][20] == 0xc023);
    pch_fixture(E1000_DEV_I219_LM_15D7); mdic_stuck = true;
    assert(!e1000_i219_init() && mock_phy_page == 0x120); stopped(0);
    pch_fixture(E1000_DEV_I219_LM_15D7); mdic_error = true;
    assert(!e1000_i219_init()); stopped(0);
    pch_fixture(E1000_DEV_I219_LM_15D7); phy_ignore_write = true;
    assert(!e1000_i219_init() && mock_phy_page == 0x120);
    assert(!(registers[PCH_EXTCNF / 4] & PCH_SWFLAG)); stopped(0);
    puts("[PASS] SPT PLL/K1/FIFO setup, page/ownership restore, minimum gap, MDIC failures and isolated TX trial");
    const uint16_t ids[] = {E1000_DEV_I219_LM_15D7, E1000_DEV_I219_LM_15BD,
                            E1000_DEV_I219_LM_15BB};
    for (unsigned i = 0; i < 3; ++i) {
        pch_fixture(ids[i]);
        assert(e1000_i219_init() && reset_writes == 1 && allocations == 146);
        assert(command_value & PCI_COMMAND_BUS_MASTER);
        assert(registers[REG_RFCTL / 4] == 0xc0);
        assert(registers[E1000_REG_TCTL / 4] == ((1u << 1) | (1u << 3) |
               (15u << 4) | (63u << 12) | (1u << 24)));
        assert(registers[PCH_ECC / 4] & (1u << 16));
        assert(registers[PCH_FEXTNVM11 / 4] & (1u << 13));
        assert(registers[E1000_REG_CTRL / 4] & (1u << 19));
        assert(!(registers[E1000_REG_CTRL / 4] & PCH_MASTER_DISABLE));
        assert(registers[0x3828 / 4] == 0x0141011f);
        assert(registers[0x3928 / 4] == 0x0141011f);
        assert((registers[PCH_CTRL_EXT / 4] & 0x10420000) == 0x10420000);
        assert((registers[0x3840 / 4] & (3u << 28)) == (i == 0 ? 2u << 28 : 0));
        assert(registers[PCH_IOSFPC / 4] == (i == 0 ? 1u << 16 : 0));
        assert(registers[REG_TIPG / 4] == (8u | (8u << 10) | (6u << 20)));
        assert(registers[E1000_REG_RAL0 / 4] == 0x0e50f7c8);
        assert(registers[E1000_REG_RAH0 / 4] == 0x80008035);
        assert(!e1000_i219_init() && allocations == 146);
        /* DD absence -> actual TX timeout -> PCH fatal containment. */
        unsigned char frame[60] = {0};
        assert(e1000_send_raw(&s_net_dev, frame, sizeof(frame)) == -1);
        assert(tx_waits == 100);
        assert(strstr(captured_log, "TX TIMEOUT, captured BEFORE containment"));
        assert(reset_writes == 2);
        stopped(146);
    }
    puts("[PASS] SPT/CNP DMA configuration, address restore, one-shot init and TX fatal reset/quarantine");
    const uint16_t recovery_ids[] = {E1000_DEV_I219_LM_15D7, E1000_DEV_I219_LM_1A1E};
    for (unsigned i=0; i<2; ++i) {
        pch_fixture(recovery_ids[i]); registers[E1000_REG_STATUS / 4] = 0;
        assert(e1000_i219_init() && reset_writes==0 && elapsed_ms==500);
        assert(!allocations && !g_net_fatal && !s_pch_attempted && !s_pch_failed);
        assert(e1000_get_net_device()==&s_net_dev && s_net_dev.mtu==1500);
        unsigned char frame[60]={0};
        assert(!e1000_poll_rx(&s_net_dev));
        assert(e1000_send_raw(&s_net_dev,frame,sizeof(frame))==-1);
        assert(!e1000_service_link(&s_net_dev,0,100));
        registers[E1000_REG_STATUS / 4] |= E1000_STATUS_LU;
        assert(!e1000_service_link(&s_net_dev,24,100) && !allocations);
        assert(e1000_service_link(&s_net_dev,25,100));
        assert(allocations==146 && reset_writes==1);
        uintptr_t rx=s_rx_phys, tx=s_tx_phys;
        for (unsigned flap=0; flap<100; ++flap) {
            registers[E1000_REG_STATUS / 4] &= ~E1000_STATUS_LU;
            assert(!e1000_service_link(&s_net_dev,26+2*flap,100));
            assert(s_link_state==LINK_DOWN && !e1000_network_online(&s_net_dev));
            assert(e1000_send_raw(&s_net_dev,frame,sizeof(frame))==-1);
            registers[E1000_REG_STATUS / 4] |= E1000_STATUS_LU;
            assert(e1000_service_link(&s_net_dev,27+2*flap,100));
            assert(allocations==146 && reset_writes==1 && s_rx_phys==rx && s_tx_phys==tx);
        }
        (void)e1000_quiesce();
        assert(!e1000_service_link(&s_net_dev,1000,100)); stopped(146);
    }
    for (unsigned oom=0; oom<146; ++oom) {
        pch_fixture(ids[0]); registers[E1000_REG_STATUS / 4]=0;
        assert(e1000_i219_init()); fail_at=oom;
        registers[E1000_REG_STATUS / 4] |= E1000_STATUS_LU;
        assert(!e1000_service_link(&s_net_dev,0,100)); stopped(oom);
        assert(!e1000_service_link(&s_net_dev,1000,100) && allocations==oom);
    }
    pch_fixture(ids[0]); registers[E1000_REG_STATUS / 4]=0; cold_timer_fail=true;
    assert(!e1000_i219_init() && s_link_state==LINK_FAILED);
    assert(strstr(s_pch_failure.reason,"observation timer failed")); stopped(0);
    puts("[PASS] cold cable activation, 100 flaps retain rings, deferred OOM and terminal quarantine (5590/5530 mocks)");
    pch_fixture(ids[0]); pending_stuck = true;
    assert(!e1000_i219_init() && !reset_writes && elapsed_ms == 100);
    stopped(0);
    pch_fixture(ids[0]); registers[PCH_EXTCNF / 4] = PCH_SWFLAG;
    assert(!e1000_i219_init() && !reset_writes);
    assert(registers[PCH_EXTCNF / 4] & PCH_SWFLAG); /* Never steal existing flag. */
    stopped(0);
    pch_fixture(ids[0]); deny_ownership = true;
    assert(!e1000_i219_init() && !reset_writes);
    stopped(0);
    pch_fixture(ids[0]); reset_stuck = true;
    assert(!e1000_i219_init() && reset_writes == 1);
    stopped(0);
    pch_fixture(ids[0]); lose_link_after_reset = true;
    assert(!e1000_i219_init() && reset_writes == 1);
    assert(strstr(s_pch_failure.reason, "link lost"));
    stopped(0);
    puts("[PASS] link, pending-transaction, ownership and reset timeouts are bounded and never retried");
    pch_fixture(ids[0]); post_timer_fail = true;
    assert(!e1000_i219_init() && reset_waiting && !s_pch_mmio_safe);
    assert(strstr(s_pch_failure.reason, "PRE-RESET"));
    stopped(0); /* Mock forbids MMIO after unsuccessful reset delay. */
    pch_fixture(ids[0]); ignored_write = PCH_ECC;
    assert(!e1000_i219_init()); stopped(146);
    pch_fixture(ids[0]); ignored_write = PCH_FEXTNVM11;
    assert(!e1000_i219_init()); stopped(146);
    pch_fixture(ids[0]); ignored_write = 0x3828;
    assert(!e1000_i219_init()); stopped(146);
    pch_fixture(ids[0]); ignored_write = 0x3928;
    assert(!e1000_i219_init()); stopped(146);
    for (unsigned oom = 0; oom < 146; ++oom) {
        pch_fixture(ids[0]); fail_at = oom;
        assert(!e1000_i219_init()); stopped(oom);
    }
    pch_fixture(ids[0]);
    /* pci_read_config16 mock shares command and desc-status values. */
    command_value |= PCH_FLUSH_REQUIRED;
    registers[REG_TDLEN / 4] = 1024;
    assert(!e1000_i219_init() && !reset_writes);
    assert(strstr(s_pch_failure.reason, "flush required"));
    stopped(0);
    puts("[PASS] timer blackout, failed engine setup/OOM and descriptor-flush stop retain immutable snapshots and DMA");
    pch_fixture(ids[0]);
    assert(e1000_i219_init());
    e1000_raw_selftest("verbose");
    e1000_raw_selftest("net_test=rings-extra");
    assert(s_tx_next == 0); /* Exact opt-in; no packets during ordinary boot. */
    complete_tdt = true;
    registers[0x1000 / 4] = 0x0006001a;
    registers[0x3430 / 4] = 0x123;
    e1000_raw_selftest("verbose net_test=rings");
    assert(s_tx_next == 1 && s_tx[0].length == 60 && !s_tx_pending);
    assert(strstr(captured_log, "TX descriptor 0 PRE-TDT bytes:"));
    assert(strstr(captured_log, "physical-derived HHDM PRE-TDT bytes:"));
    assert(strstr(captured_log, "(same CPU mapping)"));
    assert(strstr(captured_log, "TX frame readback MATCH submitted bytes"));
    assert(strstr(captured_log, " FF FF FF FF FF FF C8 F7 50 0E 35 80 88 B5 46 4F"));
    assert(strstr(captured_log, " A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5\n"));
    assert(strstr(captured_log, "GCR: no-snoop bits clear"));
    assert(strstr(captured_log, "IOSFPC: SPT workaround bit 16 set"));
    assert(strstr(captured_log, "00003410=TDFH, 00003418=TDFT"));
    assert(strstr(captured_log, "tx-reg 00001000 = 0006001A"));
    assert(strstr(captured_log, "tx-reg 00003430 = 00000123"));
    assert(!strstr(captured_log, "TX TIMEOUT"));
    /* Even immediate mock completion must not alter the saved pre-TDT DD. */
    assert(strstr(captured_log, " 3C 00 00 0B 00 00 00 00\n"));
    unsigned char *sent = vmm_phys_to_virt(s_tx_pages[0]);
    const unsigned char broadcast[6] = {255,255,255,255,255,255};
    assert(!memcmp(sent, broadcast, 6));
    assert(!memcmp(sent + 6, s_e1000_dev.mac_addr, 6));
    assert(sent[12] == 0x88 && sent[13] == 0xb5);
    assert(!memcmp(sent + 14, "FORTRESS-NET-2B-TX", 18));
    for (unsigned i = 32; i < 60; ++i) assert(sent[i] == 0xa5);
    puts("[PASS] normal boot sends nothing; exact opt-in emits the documented 60-byte 2B frame and observes mock DD");
    test_refuse_master_disable = true;
    assert(!e1000_quiesce());
    assert(g_net_fatal && !e1000_get_net_device() && allocations == 146);
    assert(command_value & PCI_COMMAND_BUS_MASTER);
    assert(strstr(s_pch_stop_result, "disable FAILED"));
    puts("[PASS] failed PCI mastering-disable readback is reported honestly; fatal latch and DMA retention remain");
    pch_fixture(E1000_DEV_82540EM);
    assert(!is_i219() && e1000_init_rings() && !reset_writes);
    assert(registers[REG_RFCTL / 4] == 0 && registers[PCH_ECC / 4] == 0);
    assert(registers[REG_TIPG / 4] == (10u | (8u << 10) | (6u << 20)));
    pch_fixture(E1000_DEV_82574L);
    assert(!is_i219() && e1000_init_rings() && !reset_writes);
    pch_fixture(E1000_DEV_I219_LM);
    assert(!is_i219());
    puts("[PASS] QEMU models bypass PCH operations; legacy I219 IDs remain discovery-only");
    pch_fixture(ids[0]);
    assert(e1000_i219_init());
    complete_after = 90;
    unsigned char timed_frame[60] = {0};
    assert(e1000_send_raw(&s_net_dev, timed_frame, 60) == 0 && tx_waits == 90);
    assert(!g_net_fatal && !s_tx_pending);
    pch_fixture(ids[0]); assert(e1000_i219_init()); tx_timer_fail = true;
    assert(e1000_send_raw(&s_net_dev, timed_frame, 60) == -1 && tx_waits == 1);
    assert(strstr(captured_log, "timer failed; stopped early")); stopped(146);
    puts("[PASS] PIT budget: late completion at 90ms, 100ms timeout, early timer failure; waits outside lock");
    pch_fixture(ids[0]); assert(e1000_i219_init());
    mock_poll_hz = 1000000000; mock_complete_read = 5;
    assert(e1000_send_raw(&s_net_dev, timed_frame, 60) == 0);
    assert(tx_waits == 0 && !s_tx_pending && !g_net_fatal);
    for (unsigned mode = 0; mode < 3; ++mode) {
        pch_fixture(ids[0]); assert(e1000_i219_init());
        mock_poll_hz = 1000000000;
        mock_poll_stalled = mode == 1; mock_poll_backward = mode == 2;
        assert(e1000_send_raw(&s_net_dev, timed_frame, 60) == -1);
        assert(tx_waits == 100 && mock_poll_reads <= 4097);
        stopped(146);
    }
    puts("[PASS] bounded 20us TX fast completion; expiry/stalled/backward clock retain 100ms PIT timeout and quarantine");

    pch_fixture(ids[0]);
    uint8_t dmar[64] = {0};
    memcpy(dmar, "DMAR", 4);
    ((acpi_sdt_header_t *)dmar)->length = sizeof(dmar);
    dmar[50] = 16; /* DRHD structure length */
    uint64_t drhd_phys = 0xfee00000;
    memcpy(dmar + 56, &drhd_phys, 8);
    uint8_t sum = 0;
    for (unsigned n = 0; n < sizeof(dmar); ++n) sum += dmar[n];
    dmar[9] -= sum;
    diagnostic_dmar = (acpi_sdt_header_t *)dmar;
    s_e1000_dev.pci.device = 31; s_e1000_dev.pci.function = 6;
    mock_vtd[0] = 0x10;
    mock_vtd[8 / 4] = 0x10000000; /* CAP.FRO=0x10 -> offset 0x100, NFR=1 */
    mock_vtd[0x1c / 4] = 1u << 31;
    mock_vtd[0x34 / 4] = 2;
    mock_vtd[0x100 / 4] = 0x0424d000;
    mock_vtd[0x108 / 4] = (31u << 3) | 6;
    mock_vtd[0x10c / 4] = 0x80000006;
    uint32_t before_vtd[sizeof(mock_vtd) / 4];
    memcpy(before_vtd, mock_vtd, sizeof(mock_vtd));
    net_vtd_prepare(); assert(s_vtd_count == 1);
    diagnostic_config[0x34 / 4] = 0x40;
    diagnostic_config[0x40 / 4] = 0x5001;
    diagnostic_config[0x44 / 4] = 3;
    diagnostic_config[0x50 / 4] = 0x10;
    diagnostic_config[0x58 / 4] = 0xf0000;
    diagnostic_ecam = true;
    diagnostic_config[0x100 / 4] = 1;
    diagnostic_config[0x104 / 4] = 0x1000;
    diagnostic_config[0x110 / 4] = 0x40;
    net_diag_t record;
    net_diag_copy(&record); net_diag_report("TEST", &record);
    assert(record.pm && record.pmcsr == 3 && record.pcie && record.pcie_status == 15);
    assert(record.aer && record.ue == 0x1000 && record.ce == 0x40);
    assert(record.unit[0].stable && strstr(captured_log, "matches NIC requester"));
    assert(!memcmp(before_vtd, mock_vtd, sizeof(mock_vtd)));
    diagnostic_ecam = false; net_diag_copy(&record); assert(!record.aer);
    diagnostic_config[0x40 / 4] = 0x4001; /* cyclic standard capability list */
    net_diag_copy(&record); /* bounded termination */
    vtd_map_fail = true; net_vtd_prepare(); assert(!s_vtd_count);
    vtd_map_fail = false; dmar[50] = 0; /* bad checksum/zero length rejected */
    net_vtd_prepare(); assert(!s_vtd_count);
    sum = 0; for (unsigned n = 0; n < sizeof(dmar); ++n) sum += dmar[n];
    dmar[9] -= sum; net_vtd_prepare(); assert(!s_vtd_count);
    diagnostic_dmar = NULL; net_vtd_prepare(); assert(!s_vtd_count);
    puts("[PASS] read-only DMAR/fault requester capture, absent/malformed/map-failed tables and bounded PCI capability/AER walks");
    pch_fixture(ids[0]); assert(e1000_i219_init());
    registers[PCH_FWSM / 4] = 0xe001c25c;
    rx_inject = true;
    e1000_raw_selftest("net_test=rings net_rx_first=1");
    assert(strstr(captured_log, "RX PASS: peer frame byte-checked and recycled"));
    assert(strstr(captured_log, "WLOCK_MAC=00000004"));
    assert(strstr(captured_log, "MODE(raw)=00000006"));
    assert(!diagnostic_dmar_reads && !s_tx_next && !g_net_fatal && rx_waits == 3);
    assert(s_rx_next == 1 && !s_rx[0].status);
    pch_fixture(ids[0]); assert(e1000_i219_init());
    rx_inject = rx_bad = true;
    e1000_raw_selftest("net_test=rings net_rx_first=1");
    assert(strstr(captured_log, "RX peer frame NOT observed"));
    assert(rx_waits == 30000 && s_rx_next == 1 && !s_tx_next && !g_net_fatal);
    pch_fixture(ids[0]); assert(e1000_i219_init());
    e1000_raw_selftest("net_test=rings net_rx_first=1");
    assert(rx_waits == 30000 && !s_rx_next && !s_tx_next && !g_net_fatal);
    assert(!strstr(captured_log, "PCI/VT-d checkpoint"));
    puts("[PASS] RX-only exact peer-byte match, malformed/no-traffic outcomes, recycle, FWSM decode and no TX/DMAR probe");
    puts("NET I219 host ASan/UBSan PASS (mock hardware; physical acceptance pending)");
    return 0;
}
