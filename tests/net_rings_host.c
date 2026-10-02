/* Actual e1000.c descriptor/ownership paths, mocked PMM/MMIO/PCI and locks.
 * No physical DMA, IRQ, timing or SMP hardware claim. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#define TEST_SMP_MEMORY 1
static void spin_debug_assert_unheld(void) {}
#include "../src/drivers/e1000.c"

static unsigned char memory[160][4096] __attribute__((aligned(4096)));
static uint32_t registers[0x6000 / 4];
static unsigned allocations, fail_at;
static uint16_t command_value;
static bool high_enabled = true;
static uint64_t *diagnostic_page_root;
uint64_t *vmm_get_kernel_pml4_virt(void) { return diagnostic_page_root; }
static uint32_t diagnostic_config[1024];
static bool diagnostic_ecam;
static acpi_sdt_header_t *diagnostic_dmar;
static unsigned diagnostic_dmar_reads;
acpi_sdt_header_t *acpi_find_table(const char *name) {
    assert(!strcmp(name, "DMAR")); ++diagnostic_dmar_reads; return diagnostic_dmar;
}
bool pci_is_mcfg_available(void) { return diagnostic_ecam; }
uint32_t pci_read_config32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg) {
    (void)seg; (void)bus; (void)dev; (void)fn;
    assert(reg < sizeof(diagnostic_config)); return diagnostic_config[reg / 4];
}

uintptr_t pmm_alloc_page(void) {
    if (allocations == fail_at) return 0;
    assert(allocations < 160);
    return (uintptr_t)(++allocations) * 4096;
}
bool pmm_high_memory_enabled(void) { return high_enabled; }
void *vmm_phys_to_virt(uintptr_t phys) {
    assert(phys >= 4096 && phys / 4096 <= allocations);
    return memory[phys / 4096 - 1] + phys % 4096;
}
uint16_t pci_read_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg) {
    (void)seg; (void)bus; (void)dev; (void)fn;
    if (reg == PCI_REG_COMMAND || reg == PCH_DESC_STATUS) return command_value;
    assert(reg < sizeof(diagnostic_config));
    return diagnostic_config[reg / 4] >> ((reg & 2) * 8);
}
void pci_write_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint16_t value) {
    (void)seg; (void)bus; (void)dev; (void)fn; (void)reg;
#ifdef E1000_PCH_HOST_TEST
    if (test_refuse_master_disable && (command_value & PCI_COMMAND_BUS_MASTER) &&
        !(value & PCI_COMMAND_BUS_MASTER)) return;
#endif
    command_value = value;
}
void serial_puts(const char *s) {
#ifdef E1000_PCH_HOST_TEST
    e1000_mock_log(s);
#else
    (void)s;
#endif
}
void console_puts(const char *s) { (void)s; }
void console_set_quiet(bool quiet) { (void)quiet; }
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!pthread_mutex_lock(&lock->mutex));
#ifdef E1000_PCH_HOST_TEST
    ++test_lock_depth;
#endif
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t irq) {
    (void)irq;
#ifdef E1000_PCH_HOST_TEST
    --test_lock_depth;
#endif
    assert(!pthread_mutex_unlock(&lock->mutex));
}

static void fixture(void) {
    diagnostic_page_root = NULL;
    memset(diagnostic_config, 0, sizeof(diagnostic_config));
    diagnostic_ecam = false; diagnostic_dmar = NULL;
    diagnostic_dmar_reads = 0;
    s_vtd_count = 0;
    allocations = 0; fail_at = 160; command_value = PCI_COMMAND_MEMORY_SPACE;
    high_enabled = true;
    memset(registers, 0, sizeof(registers));
    registers[E1000_REG_STATUS / 4] = E1000_STATUS_LU;
    memset(&s_e1000_dev, 0, sizeof(s_e1000_dev));
    s_e1000_dev.mmio_virt = (uintptr_t)registers; /* CPU-side mock MMIO, not physical */
    s_e1000_dev.link_up = true;
    s_e1000_found = true;
    s_dma_attempted = g_net_fatal = s_rx_discard = false;
    s_link_state = LINK_UNINITIALIZED; s_link_check = 0;
    s_rx_next = s_tx_next = s_tx_pending = 0;
    s_rx = NULL; s_tx = NULL;
    memset(&s_net_dev, 0, sizeof(s_net_dev));
    memset(s_rx_pages, 0, sizeof(s_rx_pages));
    memset(s_tx_pages, 0, sizeof(s_tx_pages));
    memset(s_rx_packets, 0, sizeof(s_rx_packets));
    memset(s_rx_state, 0, sizeof(s_rx_state));
    memset(s_tx_busy, 0, sizeof(s_tx_busy));
}

static void inject(uint16_t length, uint8_t status, uint8_t errors) {
    unsigned slot = s_rx_next;
    memset(s_rx_packets[s_rx_slot[slot]]->data, 0x5a, PBUF_CAPACITY);
    s_rx[slot].length = length;
    s_rx[slot].errors = errors;
    s_rx[slot].status = status;
}

static unsigned completed_slot;
static void *complete_tx(void *unused) {
    (void)unused;
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
        unsigned slot = completed_slot;
        if (s_tx_busy[slot] && !s_tx[slot].status) {
            assert(s_tx[slot].buffer_addr == s_tx_pages[slot]);
            assert(s_tx[slot].length == 60 && s_tx[slot].cmd == 11);
            unsigned char *payload = vmm_phys_to_virt(s_tx_pages[slot]);
            for (unsigned j = 0; j < 60; ++j) assert(payload[j] == 0x7c);
            s_tx[slot].status = DESC_DD;
            spin_unlock_irqrestore(&g_net_dev_lock, irq);
            return NULL;
        }
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
    }
}

int main(void) {
    fixture();
    high_enabled = false;
    assert(!e1000_init_rings() && allocations == 0);
    for (unsigned oom = 0; oom < 146; ++oom) {
        fixture(); fail_at = oom;
        assert(!e1000_init_rings());
        assert(g_net_fatal && !e1000_get_net_device());
        assert(!(command_value & PCI_COMMAND_BUS_MASTER));
        /* Allocated frames retained: no allocator rollback or reuse occurs. */
        assert(allocations == oom);
    }
    puts("[PASS] high-memory guard and all 146 allocation-failure boundaries retain DMA and disable mastering");
    fixture();
    assert(e1000_init_rings() && allocations == 146);
    assert(command_value & PCI_COMMAND_BUS_MASTER);
    assert(registers[REG_RDLEN / 4] == 1024 && registers[REG_TDLEN / 4] == 1024);
    assert(registers[REG_RDT / 4] == 63 && registers[REG_TDT / 4] == 0);
    assert(s_net_dev.poll_rx == e1000_poll_rx && s_net_dev.recycle_rx == e1000_recycle_rx);
    for (unsigned i = 0; i < NET_RING_COUNT; ++i) {
        assert(s_rx[i].buffer_addr == s_rx_pages[i] + offsetof(pbuf_t, data));
        assert(s_tx[i].status == DESC_DD);
    }
    assert(!e1000_init_rings() && allocations == 146);
    assert(!e1000_poll_rx(&s_net_dev));
    puts("[PASS] ring geometry, physical addresses, callback publication and no repeated allocation");
    pbuf_t *held[16];
    for (unsigned i = 0; i < 16; ++i) {
        unsigned old = s_rx_slot[s_rx_next];
        inject(60, DESC_DD | RX_EOP, 0);
        held[i] = e1000_poll_rx(&s_net_dev);
        assert(held[i] == s_rx_packets[old] && s_rx_state[old] == 2);
        assert(held[i]->length == 60 && held[i]->data[0] == 0x5a);
        assert(s_rx[s_rx_next ? s_rx_next - 1 : 63].buffer_addr != s_rx_pages[old] + offsetof(pbuf_t, data));
    }
    inject(60, DESC_DD | RX_EOP, 0);
    assert(!e1000_poll_rx(&s_net_dev)); /* bounded spare exhaustion drops */
    for (unsigned i = 0; i < 16; ++i) {
        e1000_recycle_rx(&s_net_dev, held[i]);
        e1000_recycle_rx(&s_net_dev, held[i]); /* duplicate rejected */
    }
    pbuf_t foreign;
    e1000_recycle_rx(&s_net_dev, &foreign);
    e1000_recycle_rx(&s_net_dev, s_rx_packets[s_rx_slot[s_rx_next]]);
    for (unsigned i = 0; i < 130; ++i) {
        inject(60, DESC_DD | RX_EOP, 0);
        pbuf_t *p = e1000_poll_rx(&s_net_dev);
        assert(p);
        e1000_recycle_rx(&s_net_dev, p);
    }
    inject(13, DESC_DD | RX_EOP, 0); assert(!e1000_poll_rx(&s_net_dev));
    inject(1515, DESC_DD | RX_EOP, 0); assert(!e1000_poll_rx(&s_net_dev));
    inject(60, DESC_DD | RX_EOP, 1); assert(!e1000_poll_rx(&s_net_dev));
    inject(60, DESC_DD, 0); assert(!e1000_poll_rx(&s_net_dev));
    inject(60, DESC_DD | RX_EOP, 0); assert(!e1000_poll_rx(&s_net_dev));
    inject(60, DESC_DD | RX_EOP, 0);
    pbuf_t *p = e1000_poll_rx(&s_net_dev); assert(p);
    e1000_recycle_rx(&s_net_dev, p);
    puts("[PASS] RX detach/replacement, exhaustion, recycle identity, wraparound, bad lengths/errors and fragments");
    unsigned char frame[60]; memset(frame, 0x7c, sizeof(frame));
    assert(e1000_send_raw(&s_net_dev, frame, 13) == -1);
    assert(e1000_send_raw(&s_net_dev, frame, 1515) == -1);
    for (unsigned i = 0; i < 65; ++i) {
        completed_slot = s_tx_next;
        pthread_t adapter;
        assert(!pthread_create(&adapter, NULL, complete_tx, NULL));
        assert(!e1000_send_raw(&s_net_dev, frame, sizeof(frame)));
        assert(!pthread_join(adapter, NULL));
        assert(!s_tx_busy[completed_slot]);
    }
    s_tx_busy[s_tx_next] = true;
    assert(e1000_send_raw(&s_net_dev, frame, sizeof(frame)) == -1);
    s_tx_busy[s_tx_next] = false;
    s_tx_pending = NET_RING_COUNT - 1;
    assert(e1000_send_raw(&s_net_dev, frame, sizeof(frame)) == -1);
    s_tx_pending = 0;
    registers[E1000_REG_STATUS / 4] = 0;
    assert(!e1000_network_online(&s_net_dev));
    assert(e1000_send_raw(&s_net_dev, frame, sizeof(frame)) == -1);
    registers[E1000_REG_STATUS / 4] = E1000_STATUS_LU;
    assert(e1000_network_online(&s_net_dev) && !e1000_network_online(NULL));
    puts("[PASS] TX driver-owned copy, command bits, completion reservation, wraparound, bounds/busy/link checks (mock completion)");
    assert(e1000_send_raw(&s_net_dev, frame, sizeof(frame)) == -1); /* no completion */
    assert(g_net_fatal && !e1000_get_net_device() && allocations == 146);
    assert(!e1000_network_online(&s_net_dev));
    assert(!(command_value & PCI_COMMAND_BUS_MASTER));
    assert(!e1000_quiesce()); /* mock keeps CTRL.RST set: bounded timeout */
    assert(allocations == 146);
    puts("[PASS] TX completion timeout and reset timeout: fatal latched, mastering off, all DMA retained");
    puts("NET rings host ASan/UBSan PASS (mock hardware; actual driver paths)");
    return 0;
}
