// SPDX-License-Identifier: MIT
/*
 * SMP Piece 6D - Step 1: Address Space Lifecycle & Registry Host Test
 * Verifies vmm_space_t data structures, registry lookup, kernel root immutability,
 * idempotent retirement, and clean rollback on allocation failures.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "vmm.h"

/* --- Host Shims & Mocking --- */
uintptr_t g_host_mock_cr3 = 0x1000;

void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t v) { (void)v; }
void serial_print_dec(uint64_t v) { (void)v; }
size_t smp_get_cpu_count(void) { return 1; }
void smp_tlb_shootdown(uintptr_t va, uintptr_t cr3) { (void)va; (void)cr3; }

#include "percpu.h"
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];

/* Mock Linker and GDT symbols referenced by vmm_init */
uint8_t __text_start[1];
uint8_t __text_end[1];
uint8_t __rodata_start[1];
uint8_t __rodata_end[1];
uint8_t __kernel_end[1];
uint8_t kernel_stack_guard[4096];
uintptr_t gdt_get_ist1_guard(void) { return 0x1000; }
uintptr_t gdt_get_ist1_stack_top(void) { return 0x6000; }
uintptr_t gdt_get_ist2_guard(void) { return 0x7000; }
uintptr_t gdt_get_ist2_stack_top(void) { return 0xC000; }

/* Mock Physical Memory Manager (PMM) with leak detection */
#define MOCK_PAGE_SIZE 4096
#define MAX_MOCK_PAGES 256
static uint8_t g_mock_ram[MAX_MOCK_PAGES * MOCK_PAGE_SIZE] __attribute__((aligned(4096)));
static bool g_mock_page_allocated[MAX_MOCK_PAGES];
static size_t g_mock_allocated_count = 0;
static bool g_fail_pmm_alloc = false;

uintptr_t pmm_alloc_page(void) {
    if (g_fail_pmm_alloc) return 0;
    for (size_t i = 1; i < MAX_MOCK_PAGES; i++) {
        if (!g_mock_page_allocated[i]) {
            g_mock_page_allocated[i] = true;
            g_mock_allocated_count++;
            return i * MOCK_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uintptr_t phys) {
    size_t idx = phys / MOCK_PAGE_SIZE;
    assert(idx < MAX_MOCK_PAGES);
    assert(g_mock_page_allocated[idx]);
    g_mock_page_allocated[idx] = false;
    g_mock_allocated_count--;
}

bool pmm_unlock_high_memory(void) { return true; }

/* Mock Heap Allocator with fault injection and leak tracking */
static size_t g_mock_heap_alloc_count = 0;
static bool g_fail_kmalloc = false;

void *kmalloc(size_t size) {
    if (g_fail_kmalloc) return NULL;
    void *ptr = malloc(size);
    if (ptr) g_mock_heap_alloc_count++;
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;
    assert(g_mock_heap_alloc_count > 0);
    g_mock_heap_alloc_count--;
    free(ptr);
}

/* External hook from vmm.c for host test initialization */
void vmm_test_init_kernel_space(uintptr_t k_cr3, uint64_t *k_virt, uint64_t hhdm);

/* --- Test Cases --- */

static void test_kernel_space_immutability(void) {
    printf("[TEST] Testing kernel address space permanent immutability...\n");

    uintptr_t k_phys = 0x1000;
    g_mock_page_allocated[1] = true; /* Page 1 is permanently occupied by master kernel PML4 */
    g_mock_allocated_count++;
    uint64_t *k_virt = (uint64_t *)(g_mock_ram + k_phys);
    memset(k_virt, 0, MOCK_PAGE_SIZE);

    vmm_test_init_kernel_space(k_phys, k_virt, (uint64_t)(uintptr_t)g_mock_ram);
    g_host_mock_cr3 = 0x2000; /* Current CPU is not on kernel CR3 for this check */

    vmm_space_t *k_space = vmm_space_get_kernel();
    assert(k_space != NULL);
    assert(k_space->is_kernel == true);
    assert(k_space->state == VMM_SPACE_LIVE);
    assert(k_space->cr3 == k_phys);
    assert(k_space->pml4_virt == k_virt);

    /* Lookup by exact address or normalized address */
    assert(vmm_space_lookup(k_phys) == k_space);
    assert(vmm_space_lookup(k_phys | 0x18) == k_space);

    /* Retiring kernel space must be rejected */
    assert(vmm_space_retire(k_phys) == VMM_ERR_INVALID_ADDR);
    assert(k_space->state == VMM_SPACE_LIVE);

    /* Destroying kernel space must be rejected */
    assert(vmm_destroy_pml4(k_phys, false) == VMM_ERR_INVALID_ADDR);
    assert(vmm_space_lookup(k_phys) == k_space);

    printf("       [PASS] Kernel space is permanent and rejects retirement/destruction\n");
}

static void test_user_space_creation_and_lifecycle(void) {
    printf("[TEST] Testing user space creation, lookup, retirement, and destruction...\n");

    size_t initial_heap_allocs = g_mock_heap_alloc_count;
    size_t initial_pmm_allocs = g_mock_allocated_count;
    size_t initial_table_frames = vmm_get_allocated_table_frames();

    /* 1. Create User Address Space */
    uintptr_t u_pml4 = vmm_create_user_pml4();
    assert(u_pml4 != 0);
    assert(g_mock_heap_alloc_count == initial_heap_allocs + 1);
    assert(g_mock_allocated_count == initial_pmm_allocs + 1);
    assert(vmm_get_allocated_table_frames() == initial_table_frames + 1);

    /* 2. Lookup & Verify Fields */
    vmm_space_t *u_space = vmm_space_lookup(u_pml4);
    assert(u_space != NULL);
    assert(u_space->cr3 == (u_pml4 & PTE_ADDR_MASK));
    assert(u_space->state == VMM_SPACE_LIVE);
    assert(u_space->is_kernel == false);
    assert(u_space->owner_refs == 1);
    assert(u_space->sched_refs == 0);
    assert(u_space->op_refs == 0);
    assert(u_space->active_cpus_mask == 0);

    /* 3. Retirement */
    assert(vmm_space_retire(u_pml4) == VMM_OK);
    assert(u_space->state == VMM_SPACE_DYING);

    /* Verify Self-Destruction Guard: cannot destroy active CR3 */
    g_host_mock_cr3 = u_pml4;
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_INVALID_ADDR);

    /* Switch away to kernel CR3 */
    g_host_mock_cr3 = 0x1000;

    /* 4. Destruction & Unlink */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_OK);

    /* Must be unlinked from registry */
    assert(vmm_space_lookup(u_pml4) == NULL);

    /* Metadata and physical frame must be fully reaped */
    assert(g_mock_heap_alloc_count == initial_heap_allocs);
    assert(g_mock_allocated_count == initial_pmm_allocs);
    assert(vmm_get_allocated_table_frames() == initial_table_frames);

    printf("       [PASS] User space creation, retirement, unlinking, and cleanup verified\n");
}

static void test_kmalloc_failure_rollback(void) {
    printf("[TEST] Testing kmalloc failure rollback during vmm_create_user_pml4...\n");

    size_t initial_heap_allocs = g_mock_heap_alloc_count;
    size_t initial_pmm_allocs = g_mock_allocated_count;
    size_t initial_table_frames = vmm_get_allocated_table_frames();

    /* Inject kmalloc failure */
    g_fail_kmalloc = true;
    uintptr_t u_pml4 = vmm_create_user_pml4();
    g_fail_kmalloc = false;

    assert(u_pml4 == 0);
    /* Verify zero memory leak */
    assert(g_mock_heap_alloc_count == initial_heap_allocs);
    assert(g_mock_allocated_count == initial_pmm_allocs);
    assert(vmm_get_allocated_table_frames() == initial_table_frames);

    printf("       [PASS] kmalloc failure cleanly returned 0 with zero resource leaks\n");
}

static void test_pmm_failure_rollback(void) {
    printf("[TEST] Testing PMM allocation failure rollback during vmm_create_user_pml4...\n");

    size_t initial_heap_allocs = g_mock_heap_alloc_count;
    size_t initial_pmm_allocs = g_mock_allocated_count;
    size_t initial_table_frames = vmm_get_allocated_table_frames();

    /* Inject PMM allocation failure */
    g_fail_pmm_alloc = true;
    uintptr_t u_pml4 = vmm_create_user_pml4();
    g_fail_pmm_alloc = false;

    assert(u_pml4 == 0);
    /* Verify allocated vmm_space_t metadata was freed */
    assert(g_mock_heap_alloc_count == initial_heap_allocs);
    assert(g_mock_allocated_count == initial_pmm_allocs);
    assert(vmm_get_allocated_table_frames() == initial_table_frames);

    printf("       [PASS] PMM failure cleanly rolled back metadata with zero resource leaks\n");
}

static void test_op_refs_and_busy_destruction(void) {
    printf("[TEST] Testing op_refs acquisition and BUSY destruction guard...\n");

    uintptr_t u_pml4 = vmm_create_user_pml4();
    assert(u_pml4 != 0);
    vmm_space_t *u_space = vmm_space_lookup(u_pml4);
    assert(u_space != NULL);
    assert(u_space->op_refs == 0);

    /* Acquire operation reference */
    assert(vmm_space_get_op(u_space->pml4_virt) == VMM_OK);
    assert(u_space->op_refs == 1);

    /* Attempting destruction while op_refs > 0 must return VMM_ERR_BUSY */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);
    assert(vmm_space_lookup(u_pml4) == u_space); /* Must still be linked */

    /* Release operation reference */
    vmm_space_put_op(u_space->pml4_virt);
    assert(u_space->op_refs == 0);

    /* Now destruction must succeed */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_OK);
    assert(vmm_space_lookup(u_pml4) == NULL);

    printf("       [PASS] op_refs correctly causes vmm_destroy_pml4 to return VMM_ERR_BUSY\n");
}

static void test_dying_space_rejects_new_operations(void) {
    printf("[TEST] Testing that DYING space rejects new operation references...\n");

    uintptr_t u_pml4 = vmm_create_user_pml4();
    assert(u_pml4 != 0);
    vmm_space_t *u_space = vmm_space_lookup(u_pml4);
    assert(u_space != NULL);

    /* Retire space to DYING */
    assert(vmm_space_retire(u_pml4) == VMM_OK);
    assert(u_space->state == VMM_SPACE_DYING);

    /* Direct op_ref acquisition must fail */
    assert(vmm_space_get_op(u_space->pml4_virt) == VMM_ERR_INVALID_ADDR);
    assert(u_space->op_refs == 0);

    /* All page-table walkers must reject operations on DYING space */
    assert(vmm_map_page(u_space->pml4_virt, 0x400000, 0x10000, PTE_PRESENT | PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_is_mapped(u_space->pml4_virt, 0x400000) == false);
    assert(vmm_get_physical_address(u_space->pml4_virt, 0x400000) == 0);
    assert(vmm_validate_user_range(u_space->pml4_virt, 0x400000, 4096, false) == false);
    assert(vmm_unmap_page(u_space->pml4_virt, 0x400000) == VMM_ERR_INVALID_ADDR);

    /* Clean destruction succeeds */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_OK);

    printf("       [PASS] All operations on DYING space cleanly rejected without corruption\n");
}

static void test_sched_refs_and_active_mask_busy(void) {
    printf("[TEST] Testing that sched_refs and active_cpus_mask trigger VMM_ERR_BUSY...\n");

    uintptr_t u_pml4 = vmm_create_user_pml4();
    assert(u_pml4 != 0);
    vmm_space_t *u_space = vmm_space_lookup(u_pml4);
    assert(u_space != NULL);

    /* 1. Simulate thread holding sched_refs */
    u_space->sched_refs = 1;
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);
    u_space->sched_refs = 0;

    /* 2. Simulate CPU active mask bit set */
    u_space->active_cpus_mask = (1ULL << 0);
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);
    u_space->active_cpus_mask = 0;

    /* 3. Both 0 -> destruction succeeds */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_OK);

    printf("       [PASS] sched_refs and active_cpus_mask enforce VMM_ERR_BUSY deferral\n");
}

static void test_scheduler_context_switch_lifecycle(void) {
    printf("[TEST] Testing scheduler references and active_cpus_mask context switch lifecycle...\n");

    uintptr_t u_pml4 = vmm_create_user_pml4();
    assert(u_pml4 != 0);
    vmm_space_t *u_space = vmm_space_lookup(u_pml4);
    assert(u_space != NULL);
    assert(u_space->sched_refs == 0);
    assert(u_space->active_cpus_mask == 0);

    /* 1. Add scheduler reference at thread creation / process spawn */
    assert(vmm_space_add_sched_ref(u_pml4) == VMM_OK);
    assert(u_space->sched_refs == 1);
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);

    /* 2. Context switch on CPU 0 enters the address space */
    cpu_locals[0].id = 0;
    assert(vmm_space_enter(u_pml4) == VMM_OK);
    assert(u_space->active_cpus_mask == (1ULL << 0));
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);

    /* 3. Context switch on CPU 2 enters the same address space (SMP concurrency) */
    cpu_locals[0].id = 2;
    assert(vmm_space_enter(u_pml4) == VMM_OK);
    assert(u_space->active_cpus_mask == ((1ULL << 0) | (1ULL << 2)));

    /* 4. CPU 0 yields (leaves space, cr3_changed = true, thread_terminated = false) */
    cpu_locals[0].id = 0;
    vmm_space_leave(u_pml4, false, true);
    assert(u_space->active_cpus_mask == (1ULL << 2));
    assert(u_space->sched_refs == 1);

    /* 5. CPU 2 switches between threads in the same address space (cr3_changed = false) */
    cpu_locals[0].id = 2;
    vmm_space_leave(u_pml4, false, false);
    assert(u_space->active_cpus_mask == (1ULL << 2)); /* Bit NOT cleared because CPU did not leave space */
    assert(u_space->sched_refs == 1);

    /* 6. Space retires to DYING while threads are still scheduled/active */
    assert(vmm_space_retire(u_pml4) == VMM_OK);
    assert(u_space->state == VMM_SPACE_DYING);

    /* 7. New scheduler reference in DYING space is rejected */
    assert(vmm_space_add_sched_ref(u_pml4) == VMM_ERR_INVALID_ADDR);
    assert(u_space->sched_refs == 1);

    /* 8. Already-scheduled thread entering DYING space succeeds */
    cpu_locals[0].id = 0;
    assert(vmm_space_enter(u_pml4) == VMM_OK);
    assert(u_space->active_cpus_mask == ((1ULL << 0) | (1ULL << 2)));

    /* 9. CPU 0 terminates thread (thread_terminated = true, cr3_changed = true) */
    vmm_space_leave(u_pml4, true, true);
    assert(u_space->active_cpus_mask == (1ULL << 2));
    assert(u_space->sched_refs == 0);

    /* 10. CPU 2 is still running in this space, destruction must return BUSY */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_ERR_BUSY);

    /* 11. CPU 2 switches away (cr3_changed = true, thread_terminated = false) */
    cpu_locals[0].id = 2;
    vmm_space_leave(u_pml4, false, true);
    assert(u_space->active_cpus_mask == 0);

    /* 12. All references and active masks drained: destruction succeeds */
    assert(vmm_destroy_pml4(u_pml4, false) == VMM_OK);
    assert(vmm_space_lookup(u_pml4) == NULL);

    /* Reset host CPU ID */
    cpu_locals[0].id = 0;
    printf("       [PASS] Sched refs and active CPU masks accurately track context switches\n");
}

static void test_kernel_space_context_switch_noops(void) {
    printf("[TEST] Testing that kernel CR3 operations are lock-free immediate no-ops...\n");

    uintptr_t k_phys = 0x1000;

    assert(vmm_space_enter(k_phys) == VMM_OK);
    vmm_space_leave(k_phys, true, true);
    assert(vmm_space_add_sched_ref(k_phys) == VMM_OK);
    vmm_space_sub_sched_ref(k_phys);

    assert(vmm_space_enter(0) == VMM_OK);
    vmm_space_leave(0, true, true);
    assert(vmm_space_add_sched_ref(0) == VMM_OK);
    vmm_space_sub_sched_ref(0);

    printf("       [PASS] Kernel space and 0 CR3 are zero-overhead no-ops\n");
}

static void test_deferred_destruction_queue(void) {
    printf("[TEST] Testing deferred destruction queue and asynchronous drainage...\n");

    size_t initial_heap_allocs = g_mock_heap_alloc_count;
    size_t initial_pmm_allocs = g_mock_allocated_count;
    assert(vmm_get_deferred_count() == 0);

    /* 1. Create two user address spaces */
    uintptr_t u1 = vmm_create_user_pml4();
    uintptr_t u2 = vmm_create_user_pml4();
    assert(u1 != 0 && u2 != 0);
    vmm_space_t *s1 = vmm_space_lookup(u1);
    vmm_space_t *s2 = vmm_space_lookup(u2);
    assert(s1 != NULL && s2 != NULL);

    /* 2. Bind references so immediate destruction is blocked */
    assert(vmm_space_add_sched_ref(u1) == VMM_OK);
    assert(vmm_space_add_sched_ref(u2) == VMM_OK);

    /* 3. Call vmm_destroy_pml4 on both */
    assert(vmm_destroy_pml4(u1, false) == VMM_ERR_BUSY);
    assert(s1->state == VMM_SPACE_DYING);
    assert(s1->deferred_queued == true);
    assert(vmm_get_deferred_count() == 1);

    assert(vmm_destroy_pml4(u2, false) == VMM_ERR_BUSY);
    assert(s2->state == VMM_SPACE_DYING);
    assert(s2->deferred_queued == true);
    assert(vmm_get_deferred_count() == 2);

    /* 4. Drain while still busy -> 0 spaces drained */
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_deferred_count() == 2);

    /* 5. Drain u1 only: simulate thread exit and context switch away */
    cpu_locals[0].id = 0;
    assert(vmm_space_enter(u1) == VMM_OK);
    vmm_space_leave(u1, true, true); /* thread_terminated = true */
    assert(s1->sched_refs == 0 && s1->active_cpus_mask == 0);

    /* Drain now: u1 must be drained and destroyed, u2 remains queued */
    assert(vmm_drain_deferred_destructions() == 1);
    assert(vmm_get_deferred_count() == 1);
    assert(vmm_space_lookup(u1) == NULL);
    assert(vmm_space_lookup(u2) == s2);

    /* 6. Drain u2: simulate thread exit and context switch away */
    cpu_locals[0].id = 1;
    assert(vmm_space_enter(u2) == VMM_OK);
    vmm_space_leave(u2, true, true); /* thread_terminated = true */
    assert(s2->sched_refs == 0 && s2->active_cpus_mask == 0);

    /* Drain now: u2 must be drained and destroyed, queue empty */
    assert(vmm_drain_deferred_destructions() == 1);
    assert(vmm_get_deferred_count() == 0);
    assert(vmm_space_lookup(u2) == NULL);

    /* Leak check */
    assert(g_mock_heap_alloc_count == initial_heap_allocs);
    assert(g_mock_allocated_count == initial_pmm_allocs);

    cpu_locals[0].id = 0;
    printf("       [PASS] Deferred destruction queue safely defers and drains with 0 leaks\n");
}

int main(void) {
    printf("========================================================\n");
    printf("SMP Piece 6D Step 4: Complete Address Space Lifetime\n");
    printf("========================================================\n");

    test_kernel_space_immutability();
    test_user_space_creation_and_lifecycle();
    test_kmalloc_failure_rollback();
    test_pmm_failure_rollback();
    test_op_refs_and_busy_destruction();
    test_dying_space_rejects_new_operations();
    test_sched_refs_and_active_mask_busy();
    test_scheduler_context_switch_lifecycle();
    test_kernel_space_context_switch_noops();
    test_deferred_destruction_queue();

    printf("\n[ OK ] All SMP Piece 6D Step 4 host tests passed successfully!\n");
    return 0;
}


