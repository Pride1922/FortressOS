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
#include "thread.h"
#include "smp.h"
#include <pthread.h>
_Thread_local tcb_t *g_vmm_host_current;
static pthread_mutex_t host_vmm_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t host_pmm_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool host_vmm_held;
static _Thread_local unsigned host_vmm_acquires;
uint64_t vmm_host_lock(spinlock_t *lock) {
    assert(lock->rank == 3 && !host_vmm_held);
    assert(pthread_mutex_lock(&host_vmm_mutex) == 0);
    host_vmm_held = true;
    host_vmm_acquires++;
    return 0;
}
void vmm_host_unlock(spinlock_t *lock) {
    assert(lock->rank == 3 && host_vmm_held);
    host_vmm_held = false;
    assert(pthread_mutex_unlock(&host_vmm_mutex) == 0);
}

/* --- Host Shims & Mocking --- */
uintptr_t g_host_mock_cr3 = 0x1000;

void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t v) { (void)v; }
void serial_print_dec(uint64_t v) { (void)v; }
size_t smp_get_cpu_count(void) { return 1; }
static unsigned host_shootdowns;
static bool expect_batch_pin;
void smp_tlb_shootdown(uintptr_t va, uintptr_t cr3) {
    __atomic_add_fetch(&host_shootdowns, 1, __ATOMIC_RELAXED);
    (void)va;
    assert(!host_vmm_held);
    if (expect_batch_pin) {
        assert(va == 0x1ff000 && cr3 != 0);
        vmm_space_t *s = vmm_space_lookup(cr3);
        assert(s && s->op_refs > 0);
    }
    if (va && cr3) {
        vmm_space_t *space = vmm_space_lookup(cr3);
        assert(space && space->op_refs > 0);
    }
}

#include "percpu.h"
void smp_tlb_shootdown_pages(uintptr_t va, uintptr_t cr3, size_t count) {
    assert(count > 0 && count <= 16);
    if (expect_batch_pin) assert(count == 4);
    smp_tlb_shootdown(va, cr3);
}
void smp_tlb_shootdown_pages_tracked(uintptr_t va, uintptr_t cr3, size_t count, smp_tlb_trace_metrics_t *metrics) {
    (void)metrics;
    smp_tlb_shootdown_pages(va, cr3, count);
}
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
static int g_pmm_alloc_budget = -1;
static void (*g_before_pmm_alloc)(void);

uintptr_t pmm_alloc_page(void) {
    assert(!host_vmm_held); /* Allocation must precede installation lock. */
    if (g_before_pmm_alloc) {
        void (*hook)(void) = g_before_pmm_alloc;
        g_before_pmm_alloc = NULL;
        hook();
    }
    if (g_fail_pmm_alloc || g_pmm_alloc_budget == 0) return 0;
    if (g_pmm_alloc_budget > 0) g_pmm_alloc_budget--;
    assert(pthread_mutex_lock(&host_pmm_mutex) == 0);
    for (size_t i = 1; i < MAX_MOCK_PAGES; i++) {
        if (!g_mock_page_allocated[i]) {
            g_mock_page_allocated[i] = true;
            g_mock_allocated_count++;
            pthread_mutex_unlock(&host_pmm_mutex);
            return i * MOCK_PAGE_SIZE;
        }
    }
    pthread_mutex_unlock(&host_pmm_mutex);
    return 0;
}

void pmm_free_page(uintptr_t phys) {
    assert(!host_vmm_held);
    pthread_mutex_lock(&host_pmm_mutex);
    size_t idx = phys / MOCK_PAGE_SIZE;
    assert(idx < MAX_MOCK_PAGES);
    assert(g_mock_page_allocated[idx]);
    g_mock_page_allocated[idx] = false;
    g_mock_allocated_count--;
    pthread_mutex_unlock(&host_pmm_mutex);
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

static void test_atomic_walk_boundaries(void) {
    size_t baseline = g_mock_allocated_count;
    uintptr_t root = vmm_create_user_pml4();
    vmm_space_t *space = vmm_space_lookup(root);
    uint64_t *table = space->pml4_virt;
    uintptr_t frame = pmm_alloc_page();
    for (int budget = 0; budget < 3; budget++) {
        size_t before_pages = g_mock_allocated_count;
        g_pmm_alloc_budget = budget;
        assert(vmm_map_page(table, 0x400000, frame, PTE_USER | PTE_WRITABLE) == VMM_ERR_NOMEM);
        g_pmm_alloc_budget = -1;
        assert(g_mock_allocated_count == before_pages);
        assert(table[0] == 0);
    }
    assert(table[0] == 0); /* OOM publishes no partial hierarchy. */
    assert(vmm_map_page(table, 0x400000, frame, PTE_USER | PTE_WRITABLE) == VMM_OK);
    assert(vmm_map_page(table, 0x401000, frame, PTE_USER) == VMM_OK);
    assert(vmm_get_physical_address(table, 0x400123) == frame + 0x123);
    assert(vmm_validate_user_range(table, 0x400FFF, 2, false));
    assert(!vmm_validate_user_range(table, 0x400FFF, 2, true));
    assert(!vmm_validate_user_range(table, 0x402000, 1, false));
    assert(!vmm_validate_user_range(table, UINTPTR_MAX - 1, 4, false));
    assert(!vmm_validate_user_range(table, 0x800000000000ULL, 1, false));
    assert(vmm_map_page(table, 0x400000, frame, PTE_USER) == VMM_ERR_ALREADY_MAPPED);
    assert(vmm_space_add_sched_ref(root) == VMM_OK);
    tcb_t task = {.cr3 = root, .vmm_space = space};
    g_vmm_host_current = &task;
    unsigned before = host_vmm_acquires;
    for (unsigned i = 0; i < 1000; i++)
        assert(vmm_validate_user_range(table, 0x400000, 4096, true));
    assert(host_vmm_acquires == before); /* Own-task validation acquires no lock. */
    assert(vmm_unmap_page(table, 0x400000) == VMM_OK);
    assert(!vmm_validate_user_range(table, 0x400000, 1, false));
    assert(vmm_space_retire(root) == VMM_OK);
    before = host_vmm_acquires;
    assert(!vmm_validate_user_range(table, 0x401000, 1, false));
    assert(host_vmm_acquires == before);
    g_vmm_host_current = NULL;
    vmm_space_sub_sched_ref(root);
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    pmm_free_page(frame);
    assert(g_mock_allocated_count == baseline);
    puts("[PASS] Atomic walk boundaries, zero-lock own-task validation, OOM and retirement");
}

static uint64_t *race_root;
static uintptr_t race_frame;
static void competing_mapper(void) {
    assert(vmm_map_page(race_root, 0x401000, race_frame, PTE_USER) == VMM_OK);
}
static void test_spare_table_race(void) {
    bool baseline[MAX_MOCK_PAGES];
    memcpy(baseline, g_mock_page_allocated, sizeof(baseline));
    uintptr_t root = vmm_create_user_pml4();
    race_root = vmm_space_lookup(root)->pml4_virt;
    race_frame = pmm_alloc_page();
    g_before_pmm_alloc = competing_mapper;
    assert(vmm_map_page(race_root, 0x400000, race_frame, PTE_USER) == VMM_OK);
    assert(vmm_is_mapped(race_root, 0x400000));
    assert(vmm_is_mapped(race_root, 0x401000));
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    pmm_free_page(race_frame);
    assert(memcmp(baseline, g_mock_page_allocated, sizeof(baseline)) == 0);
    puts("[PASS] Competing mapper supplies hierarchy during allocation; unused spares reclaimed");
}

typedef struct { vmm_space_t *space; unsigned id; uintptr_t frame; } walk_worker_t;
static void *walk_worker(void *arg) {
    walk_worker_t *w = arg;
    tcb_t task = {.cr3 = w->space->cr3, .vmm_space = w->space};
    g_vmm_host_current = &task;
    uintptr_t va = 0x400000 + w->id * 4096;
    for (unsigned i = 0; i < 1000; i++) {
        assert(vmm_map_page(w->space->pml4_virt, va, w->frame, PTE_USER | PTE_WRITABLE) == VMM_OK);
        assert(vmm_validate_user_range(w->space->pml4_virt, va, 4096, true));
        /* Other threads may change adjacent leaves while this snapshot walks. */
        (void)vmm_validate_user_range(w->space->pml4_virt, 0x400000, 4 * 4096, true);
        assert(vmm_get_physical_address(w->space->pml4_virt, va + 7) == w->frame + 7);
        assert(vmm_unmap_page(w->space->pml4_virt, va) == VMM_OK);
    }
    g_vmm_host_current = NULL;
    return NULL;
}
static void test_concurrent_walks(void) {
    size_t baseline = g_mock_allocated_count;
    uintptr_t root = vmm_create_user_pml4();
    vmm_space_t *space = vmm_space_lookup(root);
    pthread_t workers[4];
    walk_worker_t args[4];
    for (unsigned i = 0; i < 4; i++) {
        assert(vmm_space_add_sched_ref(root) == VMM_OK);
        args[i] = (walk_worker_t){space, i, pmm_alloc_page()};
        assert(args[i].frame);
    }
    for (unsigned i = 0; i < 4; i++)
        assert(pthread_create(&workers[i], NULL, walk_worker, &args[i]) == 0);
    for (unsigned i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
        vmm_space_sub_sched_ref(root);
        pmm_free_page(args[i].frame);
    }
    assert(space->op_refs == 0);
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    assert(g_mock_allocated_count == baseline);
    puts("[PASS] Four concurrent map/unmap/walk workers, exact frame reclamation");
}

static void test_invalidation_and_rejected_teardown(void) {
    bool baseline[MAX_MOCK_PAGES];
    memcpy(baseline, g_mock_page_allocated, sizeof(baseline));
    uintptr_t root = vmm_create_user_pml4(), frame = pmm_alloc_page();
    vmm_space_t *s = vmm_space_lookup(root);
    unsigned start = host_shootdowns;
    assert(vmm_map_page(s->pml4_virt, 0x400000, frame, PTE_USER) == VMM_OK);
    assert(vmm_unmap_page(s->pml4_virt, 0x400000) == VMM_OK);
    assert(host_shootdowns == start);
    /* Raw CR3 switches must also make the sticky residency mark. */
    vmm_switch_pml4(root);
    assert(s->ever_active);
    vmm_switch_pml4(vmm_get_kernel_pml4());
    assert(vmm_map_page(s->pml4_virt, 0x400000, frame, PTE_USER) == VMM_OK);
    assert(host_shootdowns == start + 1);
    assert(vmm_unmap_page(s->pml4_virt, 0x400000) == VMM_OK);
    assert(host_shootdowns == start + 2);
    uint64_t saved = s->pml4_virt[0];
    s->pml4_virt[0] |= PTE_HUGE;
    size_t count = g_mock_allocated_count;
    assert(vmm_destroy_pml4(root, false) == VMM_ERR_INVALID_ADDR);
    assert(vmm_space_lookup(root) == s && g_mock_allocated_count == count);
    assert(vmm_space_get_op(s->pml4_virt) == VMM_OK);
    assert(vmm_destroy_pml4(root, false) == VMM_ERR_BUSY);
    vmm_space_put_op(s->pml4_virt);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_space_lookup(root) == s && vmm_get_deferred_count() == 1);
    s->pml4_virt[0] = saved;
    assert(vmm_drain_deferred_destructions() == 1);
    pmm_free_page(frame);
    assert(memcmp(baseline, g_mock_page_allocated, sizeof(baseline)) == 0);
    puts("[PASS] Private construction skips IPIs; used roots invalidate; invalid teardown retains ownership");
}

static void test_batch_unmap(void) {
    bool baseline[MAX_MOCK_PAGES];
    memcpy(baseline, g_mock_page_allocated, sizeof(baseline));
    uintptr_t root = vmm_create_user_pml4();
    vmm_space_t *s = vmm_space_lookup(root);
    uintptr_t frames[4], removed[4] = {123,123,123,123};
    const uintptr_t va = 0x1ff000; /* Cross a PT boundary. */
    assert(vmm_space_enter(root) == VMM_OK);
    vmm_space_leave(root, false, true);
    for (size_t i = 0; i < 4; i++) {
        frames[i] = pmm_alloc_page();
        assert(vmm_map_page(s->pml4_virt, va + i * 4096, frames[i],
                            PTE_USER | (i == 2 ? PTE_GLOBAL : 0)) == VMM_OK);
    }
    assert(vmm_unmap_pages(s->pml4_virt, va, 4, removed) == VMM_ERR_INVALID_ADDR);
    assert(removed[0] == 123 && s->op_refs == 0);
    for (size_t i = 0; i < 4; i++) assert(vmm_is_mapped(s->pml4_virt, va + i * 4096));
    assert(vmm_unmap_page(s->pml4_virt, va + 2 * 4096) == VMM_OK);
    assert(vmm_unmap_pages(s->pml4_virt, va, 4, removed) == VMM_ERR_NOT_MAPPED);
    assert(vmm_is_mapped(s->pml4_virt, va) && removed[0] == 123 && s->op_refs == 0);
    assert(vmm_map_page(s->pml4_virt, va + 2 * 4096, frames[2], PTE_USER) == VMM_OK);
    assert(vmm_unmap_pages(s->pml4_virt, va, 0, removed) == VMM_ERR_INVALID_ADDR);
    assert(vmm_unmap_pages(s->pml4_virt, va, 17, removed) == VMM_ERR_INVALID_ADDR);
    assert(vmm_unmap_pages(s->pml4_virt, UINTPTR_MAX - 4095, 4, removed) == VMM_ERR_INVALID_ADDR);
    unsigned before = host_shootdowns;
    expect_batch_pin = true;
    assert(vmm_unmap_pages(s->pml4_virt, va, 4, removed) == VMM_OK);
    expect_batch_pin = false;
    assert(host_shootdowns == before + 1 && s->op_refs == 0);
    for (size_t i = 0; i < 4; i++) {
        assert(removed[i] == frames[i] && !vmm_is_mapped(s->pml4_virt, va + i * 4096));
        pmm_free_page(frames[i]);
    }
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    assert(memcmp(baseline, g_mock_page_allocated, sizeof(baseline)) == 0);
    puts("[PASS] Batch unmap crosses PT boundary, validates before mutation and keeps pin through one flush");
}

static void test_batch_map(void) {
    bool baseline[MAX_MOCK_PAGES];
    memcpy(baseline, g_mock_page_allocated, sizeof(baseline));
    uintptr_t root = vmm_create_user_pml4();
    vmm_space_t *space = vmm_space_lookup(root);
    uint64_t *table = space->pml4_virt;
    uintptr_t frames[4];
    for (size_t i = 0; i < 4; i++) frames[i] = pmm_alloc_page();
    unsigned shoots = host_shootdowns;
    for (int budget = 0; budget < 3; budget++) {
        bool before[MAX_MOCK_PAGES];
        memcpy(before, g_mock_page_allocated, sizeof(before));
        size_t tables = vmm_get_allocated_table_frames();
        g_pmm_alloc_budget = budget;
        assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER | PTE_WRITABLE | PTE_NX) == VMM_ERR_NOMEM);
        g_pmm_alloc_budget = -1;
        assert(!table[0] && !space->op_refs && host_shootdowns == shoots);
        assert(vmm_get_allocated_table_frames() == tables);
        assert(!memcmp(before, g_mock_page_allocated, sizeof(before)));
    }
    assert(vmm_map_pages(table, 0x401000, 0, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x401000, 17, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x1ff000, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, UINTPTR_MAX - 4095, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x401001, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x800000000000ULL, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0xffffffffa0001000ULL, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER | PTE_HUGE) == VMM_ERR_INVALID_ADDR);
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER | PTE_GLOBAL) == VMM_ERR_INVALID_ADDR);
    uintptr_t bad[4]; memcpy(bad, frames, sizeof(bad)); bad[3] = frames[0];
    assert(vmm_map_pages(table, 0x401000, 4, bad, PTE_USER) == VMM_ERR_INVALID_ADDR);
    bad[3] = frames[3] + 1;
    assert(vmm_map_pages(table, 0x401000, 4, bad, PTE_USER) == VMM_ERR_INVALID_ADDR);
    bad[3] = 1ULL << 60;
    assert(vmm_map_pages(table, 0x401000, 4, bad, PTE_USER) == VMM_ERR_INVALID_ADDR);
    table[0] = PTE_PRESENT | PTE_HUGE;
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    assert(table[0] == (PTE_PRESENT | PTE_HUGE) && !space->op_refs);
    table[0] = 0;
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER | PTE_WRITABLE | PTE_NX) == VMM_OK);
    assert(host_shootdowns == shoots && !space->op_refs); /* never-loaded root */
    assert(!vmm_is_mapped(table, 0x400000)); /* guard stays absent */
    for (size_t i = 0; i < 4; i++)
        assert(vmm_get_physical_address(table, 0x401000 + i * 4096) == frames[i]);
    assert(vmm_validate_user_range(table, 0x401000, 4 * 4096, true));
    uint64_t *pdpt = vmm_phys_to_virt(table[0] & PTE_ADDR_MASK);
    uint64_t *pd = vmm_phys_to_virt(pdpt[0] & PTE_ADDR_MASK);
    uint64_t *pt = vmm_phys_to_virt(pd[2] & PTE_ADDR_MASK);
    for (size_t i = 0; i < 4; i++)
        assert((pt[1 + i] & ~PTE_ADDR_MASK) == (PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX));
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER) == VMM_ERR_ALREADY_MAPPED);
    uintptr_t removed[4];
    assert(vmm_unmap_pages(table, 0x401000, 4, removed) == VMM_OK);
    assert(vmm_space_enter(root) == VMM_OK);
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER) == VMM_OK);
    assert(host_shootdowns == shoots + 1 && !space->op_refs);
    /* A mapped middle leaf rejects the entire batch without installing holes. */
    assert(vmm_unmap_page(table, 0x401000) == VMM_OK);
    shoots = host_shootdowns;
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER) == VMM_ERR_ALREADY_MAPPED);
    assert(!vmm_is_mapped(table, 0x401000) && host_shootdowns == shoots);
    assert(vmm_space_retire(root) == VMM_OK);
    assert(vmm_map_pages(table, 0x401000, 4, frames, PTE_USER) == VMM_ERR_INVALID_ADDR);
    vmm_space_leave(root, false, true);
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    for (size_t i = 0; i < 4; i++) pmm_free_page(frames[i]);
    assert(!memcmp(baseline, g_mock_page_allocated, sizeof(baseline)));
    puts("[PASS] Batch map exact OOM rollback, boundaries, guard, collision, inactive/active flush and lifecycle pin");
}

static void batch_competing_mapper(void) {
    assert(vmm_map_page(race_root, 0x403000, race_frame, PTE_USER) == VMM_OK);
}
static void test_batch_map_race(void) {
    bool baseline[MAX_MOCK_PAGES];
    memcpy(baseline, g_mock_page_allocated, sizeof(baseline));
    uintptr_t root = vmm_create_user_pml4();
    race_root = vmm_space_lookup(root)->pml4_virt;
    uintptr_t frames[4];
    for (size_t i = 0; i < 4; i++) frames[i] = pmm_alloc_page();
    race_frame = frames[2];
    g_before_pmm_alloc = batch_competing_mapper;
    assert(vmm_map_pages(race_root, 0x401000, 4, frames, PTE_USER) == VMM_ERR_ALREADY_MAPPED);
    assert(!vmm_is_mapped(race_root, 0x401000) && !vmm_is_mapped(race_root, 0x402000));
    assert(vmm_get_physical_address(race_root, 0x403000) == frames[2]);
    assert(!vmm_is_mapped(race_root, 0x404000));
    assert(!vmm_space_lookup(root)->op_refs);
    assert(vmm_destroy_pml4(root, false) == VMM_OK);
    for (size_t i = 0; i < 4; i++) pmm_free_page(frames[i]);
    assert(!memcmp(baseline, g_mock_page_allocated, sizeof(baseline)));
    puts("[PASS] Batch map repeats full preflight after competing mapper, spares reclaimed");
}

static void test_burst_memory_accounting(void) {
    bool baseline_pages[MAX_MOCK_PAGES];
    memcpy(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages));
    size_t baseline_allocated = g_mock_allocated_count;
    size_t baseline_tables = vmm_get_allocated_table_frames();

    /* 1. Warmed baseline: Create, map and destroy a single space */
    uintptr_t warm_root = vmm_create_user_pml4();
    assert(warm_root != 0);
    uint64_t *warm_pml4 = vmm_space_lookup(warm_root)->pml4_virt;
    uintptr_t warm_frame = pmm_alloc_page();
    assert(warm_frame != 0);
    assert(vmm_map_page(warm_pml4, 0x400000, warm_frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK);
    assert(vmm_destroy_pml4(warm_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(vmm_get_allocated_table_frames() == baseline_tables);

    /* 2. Burst of user spaces with deferred destruction */
    #define BURST_COUNT 4
    uintptr_t roots[BURST_COUNT];
    uintptr_t data_frames[BURST_COUNT];
    for (int i = 0; i < BURST_COUNT; i++) {
        roots[i] = vmm_create_user_pml4();
        assert(roots[i] != 0);
        vmm_space_t *s = vmm_space_lookup(roots[i]);
        assert(s != NULL);
        data_frames[i] = pmm_alloc_page();
        assert(data_frames[i] != 0);
        assert(vmm_map_page(s->pml4_virt, 0x400000, data_frames[i], PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK);
        /* Acquire transient op_ref so destruction defers */
        assert(vmm_space_get_op(s->pml4_virt) == VMM_OK);
        assert(vmm_destroy_pml4(roots[i], true) == VMM_ERR_BUSY);
    }
    assert(vmm_get_deferred_count() == BURST_COUNT);
    assert(g_mock_allocated_count > baseline_allocated);

    /* Release transient op_refs and drain */
    for (int i = 0; i < BURST_COUNT; i++) {
        vmm_space_t *s = vmm_space_lookup(roots[i]);
        assert(s != NULL);
        vmm_space_put_op(s->pml4_virt);
    }
    size_t drained = vmm_drain_deferred_destructions();
    assert(drained == BURST_COUNT);
    assert(vmm_get_deferred_count() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)) == 0);

    /* 3. Bounded PMM pressure & clean OOM rollback */
    g_pmm_alloc_budget = 1;
    uintptr_t oom_root = vmm_create_user_pml4();
    if (oom_root != 0) {
        vmm_space_t *s = vmm_space_lookup(oom_root);
        assert(s != NULL);
        uintptr_t frame = pmm_alloc_page();
        assert(frame == 0);
        assert(vmm_destroy_pml4(oom_root, true) == VMM_OK);
    }
    g_pmm_alloc_budget = -1;
    assert(vmm_get_deferred_count() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)) == 0);

    puts("[PASS] Burst memory accounting and bounded PMM pressure recovery");
}

static void test_concurrent_cohort_and_fragmented_pmm(void) {
    bool baseline_pages[MAX_MOCK_PAGES];
    memcpy(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages));
    size_t baseline_allocated = g_mock_allocated_count;
    size_t baseline_tables = vmm_get_allocated_table_frames();

    /* 1. Concurrent cohorts of sizes 2 and 4 */
    const size_t test_cohorts[] = {2, 4};
    for (size_t c = 0; c < 2; c++) {
        size_t n = test_cohorts[c];
        size_t pass0_tables = 0, pass0_alloc = 0;
        for (int pass = 0; pass < 2; pass++) {
            uintptr_t roots[4];
            uintptr_t data[4];
            for (size_t i = 0; i < n; i++) {
                roots[i] = vmm_create_user_pml4();
                assert(roots[i] != 0);
                vmm_space_t *s = vmm_space_lookup(roots[i]);
                assert(s != NULL);
                data[i] = pmm_alloc_page();
                assert(data[i] != 0);
                assert(vmm_map_page(s->pml4_virt, 0x400000, data[i], PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK);
                assert(vmm_space_add_sched_ref(roots[i]) == VMM_OK);
            }
            if (pass == 0) {
                pass0_tables = vmm_get_allocated_table_frames();
                pass0_alloc = g_mock_allocated_count;
            } else {
                assert(vmm_get_allocated_table_frames() == pass0_tables);
                assert(g_mock_allocated_count == pass0_alloc);
            }

            /* Mixed-order release & destruction */
            size_t order[4];
            for (size_t i = 0; i < n; i++) order[i] = (n - 1 - i);
            for (size_t i = 0; i < n; i++) {
                size_t idx = order[i];
                vmm_space_sub_sched_ref(roots[idx]);
                assert(vmm_destroy_pml4(roots[idx], true) == VMM_OK);
            }
            assert(vmm_drain_deferred_destructions() == 0);
            assert(vmm_get_allocated_table_frames() == baseline_tables);
            assert(g_mock_allocated_count == baseline_allocated);
            assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));
        }
    }

    /* 2. Controlled scattered free frames & fragmented PMM */
    uintptr_t scatter[16];
    for (size_t i = 0; i < 16; i++) {
        scatter[i] = pmm_alloc_page();
        assert(scatter[i] != 0);
    }
    for (size_t i = 0; i < 16; i += 2) {
        pmm_free_page(scatter[i]);
    }
    uintptr_t single = pmm_alloc_page();
    assert(single != 0);
    pmm_free_page(single);

    uintptr_t sc_root = vmm_create_user_pml4();
    assert(sc_root != 0);
    vmm_space_t *sc_space = vmm_space_lookup(sc_root);
    assert(sc_space != NULL);
    uintptr_t sc_data = pmm_alloc_page();
    assert(sc_data != 0);
    assert(vmm_map_page(sc_space->pml4_virt, 0x400000, sc_data, PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK);
    assert(vmm_destroy_pml4(sc_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);

    for (size_t i = 1; i < 16; i += 2) {
        pmm_free_page(scatter[i]);
    }

    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    puts("[PASS] Concurrent cohorts and fragmented PMM host verification");
}

static void test_process_launch_rollback(void) {
    bool baseline_pages[MAX_MOCK_PAGES];
    memcpy(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages));
    size_t baseline_allocated = g_mock_allocated_count;
    size_t baseline_tables = vmm_get_allocated_table_frames();

    /* Cut 1: Failure during user PML4 allocation */
    g_fail_kmalloc = true;
    uintptr_t cut1_root = vmm_create_user_pml4();
    assert(cut1_root == 0);
    g_fail_kmalloc = false;
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 2: Failure during first segment page allocation */
    uintptr_t cut2_root = vmm_create_user_pml4();
    assert(cut2_root != 0);
    assert(vmm_destroy_pml4(cut2_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 3: Segment page 1 mapped, segment page 2 mapping failure */
    uintptr_t cut3_root = vmm_create_user_pml4();
    assert(cut3_root != 0);
    vmm_space_t *cut3_space = vmm_space_lookup(cut3_root);
    assert(cut3_space != NULL);
    uintptr_t seg1 = pmm_alloc_page();
    assert(seg1 != 0);
    assert(vmm_map_page(cut3_space->pml4_virt, 0x400000, seg1, PTE_PRESENT | PTE_USER) == VMM_OK);
    uintptr_t seg2 = pmm_alloc_page();
    assert(seg2 != 0);
    pmm_free_page(seg2);
    assert(vmm_destroy_pml4(cut3_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 4: Segment mapped, sigrestorer allocation failure */
    uintptr_t cut4_root = vmm_create_user_pml4();
    assert(cut4_root != 0);
    vmm_space_t *cut4_space = vmm_space_lookup(cut4_root);
    uintptr_t c4_seg = pmm_alloc_page();
    assert(c4_seg != 0);
    assert(vmm_map_page(cut4_space->pml4_virt, 0x400000, c4_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_destroy_pml4(cut4_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 5: Segment and restorer mapped, user stack allocation failure */
    uintptr_t cut5_root = vmm_create_user_pml4();
    assert(cut5_root != 0);
    vmm_space_t *cut5_space = vmm_space_lookup(cut5_root);
    uintptr_t c5_seg = pmm_alloc_page();
    uintptr_t c5_rest = pmm_alloc_page();
    assert(c5_seg != 0 && c5_rest != 0);
    assert(vmm_map_page(cut5_space->pml4_virt, 0x400000, c5_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_map_page(cut5_space->pml4_virt, 0x7fffffffe000, c5_rest, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_destroy_pml4(cut5_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 6: All user pages mapped, kernel stack allocation failure */
    uintptr_t cut6_root = vmm_create_user_pml4();
    assert(cut6_root != 0);
    vmm_space_t *cut6_space = vmm_space_lookup(cut6_root);
    uintptr_t c6_seg = pmm_alloc_page();
    uintptr_t c6_rest = pmm_alloc_page();
    uintptr_t c6_stack = pmm_alloc_page();
    assert(c6_seg != 0 && c6_rest != 0 && c6_stack != 0);
    assert(vmm_map_page(cut6_space->pml4_virt, 0x400000, c6_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_map_page(cut6_space->pml4_virt, 0x7fffffffe000, c6_rest, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_map_page(cut6_space->pml4_virt, 0x7ffffffff000, c6_stack, PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK);
    assert(vmm_destroy_pml4(cut6_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 7: All user pages mapped, kstack allocated (4 pages), TCB kmalloc failure */
    uintptr_t cut7_root = vmm_create_user_pml4();
    assert(cut7_root != 0);
    vmm_space_t *cut7_space = vmm_space_lookup(cut7_root);
    uintptr_t c7_seg = pmm_alloc_page();
    assert(c7_seg != 0);
    assert(vmm_map_page(cut7_space->pml4_virt, 0x400000, c7_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    uintptr_t kstack_frames[4];
    for (int k = 0; k < 4; k++) {
        kstack_frames[k] = pmm_alloc_page();
        assert(kstack_frames[k] != 0);
    }
    for (int k = 0; k < 4; k++) pmm_free_page(kstack_frames[k]);
    assert(vmm_destroy_pml4(cut7_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Cut 8: All user pages mapped, kstack allocated, sched_ref acquisition failure */
    uintptr_t cut8_root = vmm_create_user_pml4();
    assert(cut8_root != 0);
    vmm_space_t *cut8_space = vmm_space_lookup(cut8_root);
    uintptr_t c8_seg = pmm_alloc_page();
    assert(c8_seg != 0);
    assert(vmm_map_page(cut8_space->pml4_virt, 0x400000, c8_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    for (int k = 0; k < 4; k++) kstack_frames[k] = pmm_alloc_page();
    for (int k = 0; k < 4; k++) pmm_free_page(kstack_frames[k]);
    assert(vmm_destroy_pml4(cut8_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    /* Verify subsequent normal process creation succeeds */
    uintptr_t norm_root = vmm_create_user_pml4();
    assert(norm_root != 0);
    vmm_space_t *norm_space = vmm_space_lookup(norm_root);
    uintptr_t norm_seg = pmm_alloc_page();
    assert(norm_seg != 0);
    assert(vmm_map_page(norm_space->pml4_virt, 0x400000, norm_seg, PTE_PRESENT | PTE_USER) == VMM_OK);
    assert(vmm_space_add_sched_ref(norm_root) == VMM_OK);
    vmm_space_sub_sched_ref(norm_root);
    assert(vmm_destroy_pml4(norm_root, true) == VMM_OK);
    assert(vmm_drain_deferred_destructions() == 0);
    assert(vmm_get_allocated_table_frames() == baseline_tables);
    assert(g_mock_allocated_count == baseline_allocated);
    assert(!memcmp(baseline_pages, g_mock_page_allocated, sizeof(baseline_pages)));

    puts("[PASS] Process launch allocation-failure rollback host verification");
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
    test_atomic_walk_boundaries();
    test_concurrent_walks();
    test_spare_table_race();
    test_invalidation_and_rejected_teardown();
    test_batch_unmap();
    test_batch_map();
    test_batch_map_race();
    test_burst_memory_accounting();
    test_concurrent_cohort_and_fragmented_pmm();
    test_process_launch_rollback();

    printf("\n[ OK ] All SMP Piece 6D Step 4 host tests passed successfully!\n");
    return 0;
}
