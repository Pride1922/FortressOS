#include "smp.h"
#include "limine.h"
#include "serial.h"

/* SMP_DESIGN.md SM2 note: the original draft assumed a hand-rolled
 * INIT-SIPI-SIPI trampoline in identity-mapped sub-1MiB memory. Limine
 * (already load-bearing for this kernel's boot) performs that exact
 * sequence itself as part of its documented SMP boot protocol before
 * this kernel's entry point is ever reached, and parks every AP waiting
 * on its own `goto_address` field. Reimplementing INIT-SIPI-SIPI by hand
 * here would duplicate a sequence Limine already owns and has to get
 * right for its own boot to work, for no benefit. This request is what
 * that reuse looks like in practice.
 */
__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = NULL
};

/* No wall-clock source is safely readable this early (the APIC timer is
 * configured but not yet started -- see kmain). Bounded instruction-count
 * spin instead; calibrated generously and expected to be revisited once
 * real hardware/QEMU timing evidence exists (see the Piece 1 test doc). */
#define SMP_AP_REPORT_TIMEOUT_SPINS 200000000ULL

static volatile size_t g_online_ap_count = 0;
static size_t          g_total_cpu_count = 1;
static uint32_t        g_bsp_lapic_id = 0;

/* Runs on the AP, on the stack Limine allocated for it, in long mode,
 * with interrupts already disabled per the Limine SMP protocol. Piece 1
 * scope ends here: report in, then halt. No IDT, no per-CPU GDT/TSS, no
 * scheduler -- none of those exist per-CPU yet. */
static void smp_ap_entry(struct limine_smp_info *info) {
    (void)info;
    __atomic_fetch_add(&g_online_ap_count, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

size_t smp_get_cpu_count(void) { return g_total_cpu_count; }
uint32_t smp_get_bsp_lapic_id(void) { return g_bsp_lapic_id; }

size_t smp_init(const acpi_madt_info_t *madt_info) {
    if (!smp_request.response || smp_request.response->cpu_count == 0) {
        serial_puts("[ OK ] No Limine SMP response; staying single-CPU\n");
        return 0;
    }

    struct limine_smp_response *resp = smp_request.response;
    g_bsp_lapic_id    = resp->bsp_lapic_id;
    g_total_cpu_count = resp->cpu_count;

    /* SM1: ACPI MADT is the sole source of truth for who is expected to
     * exist. Cross-check every Limine-reported LAPIC ID against MADT's
     * enabled set; refuse to start anything MADT did not enumerate as
     * enabled, rather than trusting Limine's count on its own. */
    size_t matched = 0;
    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        for (size_t j = 0; j < madt_info->enabled_cpu_count; j++) {
            if (madt_info->enabled_cpu_apic_ids[j] == (uint8_t)cpu->lapic_id) {
                matched++;
                break;
            }
        }
    }
    if (matched != resp->cpu_count) {
        serial_puts("[FAIL] Limine SMP response disagrees with ACPI MADT enabled CPU set (");
        serial_print_dec(matched);
        serial_puts(" of ");
        serial_print_dec(resp->cpu_count);
        serial_puts(" matched); staying single-CPU\n");
        g_total_cpu_count = 1;
        return 0;
    }
    serial_puts("[ OK ] Limine SMP response cross-checked against ACPI MADT: ");
    serial_print_dec(resp->cpu_count);
    serial_puts(" CPU(s) agree (BSP LAPIC ID ");
    serial_print_dec(g_bsp_lapic_id);
    serial_puts(")\n");

    /* SM2: hand off every non-BSP CPU to smp_ap_entry via Limine's
     * documented goto_address protocol; the BSP is already running this
     * code and is excluded. */
    g_online_ap_count = 0;
    size_t ap_count = 0;
    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == g_bsp_lapic_id) continue;
        ap_count++;
        __atomic_store_n(&cpu->goto_address, smp_ap_entry, __ATOMIC_SEQ_CST);
    }

    if (ap_count == 0) {
        serial_puts("[ OK ] Single enabled CPU per MADT/Limine; no APs to start\n");
        return 0;
    }

    serial_puts("[....] Waiting for ");
    serial_print_dec(ap_count);
    serial_puts(" application processor(s) to report in...\n");

    for (uint64_t spins = 0; spins < SMP_AP_REPORT_TIMEOUT_SPINS; spins++) {
        if (__atomic_load_n(&g_online_ap_count, __ATOMIC_SEQ_CST) == ap_count) break;
        __asm__ volatile("pause");
    }

    size_t online = __atomic_load_n(&g_online_ap_count, __ATOMIC_SEQ_CST);
    if (online != ap_count) {
        serial_puts("[FAIL] Only ");
        serial_print_dec(online);
        serial_puts(" of ");
        serial_print_dec(ap_count);
        serial_puts(" AP(s) reported in before the spin timeout\n");
        g_total_cpu_count = 1 + online;
        return online;
    }

    serial_puts("[ OK ] All ");
    serial_print_dec(ap_count);
    serial_puts(" application processor(s) online (parked, interrupts disabled)\n");
    return online;
}
