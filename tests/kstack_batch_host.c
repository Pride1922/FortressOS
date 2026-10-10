/* Actual kstack functions extracted from thread.c; mocked PMM/VMM, no SMP claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "thread.h"
#include "vmm.h"
#include "pmm.h"

static spinlock_t g_kstack_lock;
static uint64_t g_stack_slots_bitmap;
static bool held, allocated[8], mapped, reject_map;
static int budget = -1;
static uintptr_t mapped_frames[4], mapped_base;
static uint64_t dummy_root[512];
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(lock == &g_kstack_lock && !held); held = true; return 0x202;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    assert(lock == &g_kstack_lock && held && flags == 0x202); held = false;
}
uintptr_t pmm_alloc_page(void) {
    assert(!held);
    if (budget == 0) return 0;
    if (budget > 0) budget--;
    for (size_t i = 1; i < 8; i++) if (!allocated[i]) {
        allocated[i] = true; return i * 4096;
    }
    return 0;
}
void pmm_free_page(uintptr_t frame) {
    assert(!held && frame / 4096 < 8 && allocated[frame / 4096]);
    allocated[frame / 4096] = false;
}
uint64_t *vmm_get_kernel_pml4_virt(void) { return dummy_root; }
int vmm_map_pages(uint64_t *root, uintptr_t va, size_t count,
                  const uintptr_t *frames, uint64_t flags) {
    assert(!held && root == dummy_root && count == 4 && !mapped);
    assert((va - KERNEL_STACKS_BASE) % STACK_SLOT_SIZE == STACK_GUARD_SIZE);
    assert(flags == (PTE_PRESENT | PTE_WRITABLE | PTE_NX));
    if (reject_map) return VMM_ERR_NOMEM;
    mapped = true; mapped_base = va;
    memcpy(mapped_frames, frames, sizeof(mapped_frames)); return VMM_OK;
}
int vmm_map_pages_tracked(uint64_t *root, uintptr_t va, size_t count,
                          const uintptr_t *frames, uint64_t flags, vmm_op_metrics_t *metrics) {
    (void)metrics;
    return vmm_map_pages(root, va, count, frames, flags);
}
int vmm_unmap_pages(uint64_t *root, uintptr_t va, size_t count, uintptr_t *frames) {
    assert(!held && root == dummy_root && mapped && va == mapped_base && count == 4);
    memcpy(frames, mapped_frames, sizeof(mapped_frames)); mapped = false; return VMM_OK;
}
int vmm_unmap_pages_tracked(uint64_t *root, uintptr_t va, size_t count, uintptr_t *frames, vmm_op_metrics_t *metrics) {
    (void)metrics;
    return vmm_unmap_pages(root, va, count, frames);
}
void serial_puts(const char *s) { (void)s; }
void serial_raw_puts(const char *s) { (void)s; }
static spawn_fault_type_t g_spawn_fault_type = SPAWN_FAULT_NONE;
static size_t g_spawn_fault_trigger = 0;
void spawn_record_fault_hit(void) {}
#include "kstack_impl.h"

int main(void) {
    for (int test = 0; test < 6; test++) {
        budget = test < 4 ? test : -1; reject_map = test == 4;
        uintptr_t guard = 123, base = 456; size_t size = 789;
        int slot = kstack_alloc(&guard, &base, &size);
        if (test < 5) {
            assert(slot == -1 && !g_stack_slots_bitmap && !mapped);
            assert(guard == 123 && base == 456 && size == 789);
        } else {
            assert(slot == 0 && mapped && g_stack_slots_bitmap == 1);
            assert(guard == KERNEL_STACKS_BASE && base == guard + STACK_GUARD_SIZE);
            assert(size == STACK_USABLE_SIZE);
            kstack_free(slot, base);
        }
        for (size_t i = 0; i < 8; i++) assert(!allocated[i]);
        assert(!held && !mapped && !g_stack_slots_bitmap);
    }
    g_stack_slots_bitmap = UINT64_MAX;
    uintptr_t guard = 0, base = 0; size_t size = 0;
    assert(kstack_alloc(&guard, &base, &size) == -1);
    assert(g_stack_slots_bitmap == UINT64_MAX && !mapped);
    g_stack_slots_bitmap &= ~(1ULL << 63);
    reject_map = false; budget = -1;
    assert(kstack_alloc(&guard, &base, &size) == 63);
    assert(guard == KERNEL_STACKS_BASE + 63 * STACK_SLOT_SIZE);
    assert(base == guard + STACK_GUARD_SIZE);
    kstack_free(63, base);
    assert(g_stack_slots_bitmap == (UINT64_MAX & ~(1ULL << 63)));
    for (size_t i = 0; i < 8; i++) assert(!allocated[i]);
    puts("kstack actual functions: all data-frame OOM cuts, mapper rejection, slot/guard outputs, success/free and exhaustion PASS");
}
