#include "spinlock.h"
#include "percpu.h"
#include "serial.h"

/*
 * Lock Discipline & Invariant Enforcement
 *
 * All tracking state lives inside cpu_local_t (per-CPU). IRQs are disabled before
 * every lock acquisition (L1-L4).
 */

static void fail(const char *reason, spinlock_t *lock);

static bool can_acquire(cpu_local_t *cpu, spinlock_t *lock) {
    if (!lock || !lock->name || !lock->rank) {
        fail("uninitialized spinlock (missing name or rank): ", lock);
        return false;
    }
    if (cpu->lock_depth >= MAX_HELD_LOCKS) return false;
    for (uint32_t i = 0; i < cpu->lock_depth; i++) {
        spinlock_t *h = cpu->held_locks[i];
        if (h == lock) return false; /* Strictly non-recursive */
        if (h->rank > lock->rank) return false; /* Rank inversion */
        if (h->rank == lock->rank) {
            /* SM11a: Two scheduler locks allowed if strictly ordered by ascending address */
            if (h->kind == LOCK_KIND_SCHED && lock->kind == LOCK_KIND_SCHED &&
                (uintptr_t)h < (uintptr_t)lock) {
                continue;
            }
            return false;
        }
    }
    return true;
}

static void print_held_chain(cpu_local_t *cpu) {
    serial_raw_puts("        held chain (depth ");
    serial_raw_print_dec(cpu->lock_depth);
    serial_raw_puts("):\n");
    for (uint32_t i = 0; i < cpu->lock_depth; i++) {
        spinlock_t *h = cpu->held_locks[i];
        serial_raw_puts("          [");
        serial_raw_print_dec(i);
        serial_raw_puts("] ");
        if (h && h->name) {
            serial_raw_puts(h->name);
        } else {
            serial_raw_puts("<unnamed>");
        }
        serial_raw_puts(" (rank ");
        if (h) {
            serial_raw_print_dec(h->rank);
            serial_raw_puts(", kind ");
            serial_raw_print_dec(h->kind);
        } else {
            serial_raw_puts("?");
        }
        serial_raw_puts(")\n");
    }
}

static void spin_fatal(const char *reason, spinlock_t *attempted) {
    cpu_local_t *cpu = cpu_current();
    serial_raw_puts("[FATAL] Lock discipline on ");
    if (cpu->id != 0) {
        serial_raw_puts("AP ");
        serial_raw_print_dec(cpu->id);
    } else {
        serial_raw_puts("BSP");
    }
    serial_raw_puts(": ");
    serial_raw_puts(reason);
    if (attempted && attempted->name) {
        serial_raw_puts(attempted->name);
        serial_raw_puts(" (rank ");
        serial_raw_print_dec(attempted->rank);
        serial_raw_puts(", kind ");
        serial_raw_print_dec(attempted->kind);
        serial_raw_puts(")");
    }
    serial_raw_puts("\n");
    print_held_chain(cpu);
    __atomic_store_n(&cpu->lock_panic, 1, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

static void fail(const char *reason, spinlock_t *lock) {
    spin_fatal(reason, lock);
}

void spin_debug_acquire(spinlock_t *lock) {
    cpu_local_t *cpu = cpu_current();
    if (!can_acquire(cpu, lock)) fail("recursive/inverted acquisition: ", lock);
    if (cpu->lock_depth >= MAX_HELD_LOCKS) fail("lock tracker capacity exceeded: ", lock);
    cpu->held_locks[cpu->lock_depth++] = lock;
}

void spin_debug_release(spinlock_t *lock) {
    cpu_local_t *cpu = cpu_current();
    if (!cpu->lock_depth || cpu->held_locks[cpu->lock_depth - 1] != lock) {
        fail("out-of-order release: ", lock);
    }
    cpu->held_locks[--cpu->lock_depth] = NULL;
}

void spin_debug_assert_unheld(void) {
    cpu_local_t *cpu = cpu_current();
    if (cpu->lock_depth) {
        fail("lock held across context switch: ", cpu->held_locks[cpu->lock_depth - 1]);
    }
}

void spin_debug_assert_held(spinlock_t *lock) {
    if (!lock) fail("assertion failed: null lock", NULL);
    cpu_local_t *cpu = cpu_current();
    for (uint32_t i = 0; i < cpu->lock_depth; i++) {
        if (cpu->held_locks[i] == lock) return;
    }
    fail("assertion failed: lock not held by caller: ", lock);
}

void spin_debug_warn_high_contention(spinlock_t *lock, uint64_t iters) {
    serial_raw_puts("[WARN] Lock contention high on CPU ");
    serial_raw_print_dec(cpu_current()->id);
    serial_raw_puts(": lock ");
    if (lock && lock->name) serial_raw_puts(lock->name);
    else serial_raw_puts("<unnamed>");
    serial_raw_puts(" spun ");
    serial_raw_print_dec(iters);
    serial_raw_puts(" iters\n");
}

bool spin_debug_selftest(void) {
    spinlock_t a = SPINLOCK_RANKED(1, "test-a");
    spinlock_t b = SPINLOCK_RANKED(2, "test-b");
    cpu_local_t *cpu = cpu_current();
    uint64_t before, after;
    __asm__ volatile("pushfq; pop %0" : "=r"(before) :: "memory");

    uint64_t fa = spin_lock_irqsave(&a);
    bool ok = !can_acquire(cpu, &a) && can_acquire(cpu, &b);
    spin_debug_assert_held(&a);

    uint64_t fb = spin_lock_irqsave(&b);
    ok = ok && !can_acquire(cpu, &a) && !can_acquire(cpu, &b) && !(fb & 0x200);
    spin_debug_assert_held(&a);
    spin_debug_assert_held(&b);

    spin_unlock_irqrestore(&b, fb);
    __asm__ volatile("pushfq; pop %0" : "=r"(after) :: "memory");
    ok = ok && !(after & 0x200);
    spin_debug_assert_held(&a);

    spin_unlock_irqrestore(&a, fa);
    __asm__ volatile("pushfq; pop %0" : "=r"(after) :: "memory");
    ok = ok && ((before ^ after) & 0x200) == 0 && cpu->lock_depth == 0;

    /* Multi-scheduler lock ascending ordering test (SM11a) */
    spinlock_t s_arr[2];
    s_arr[0] = (spinlock_t)SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, "sched-0");
    s_arr[1] = (spinlock_t)SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, "sched-1");
    spinlock_t *s_low = &s_arr[0] < &s_arr[1] ? &s_arr[0] : &s_arr[1];
    spinlock_t *s_high = &s_arr[0] < &s_arr[1] ? &s_arr[1] : &s_arr[0];

    uint64_t fs_low = spin_lock_irqsave(s_low);
    ok = ok && can_acquire(cpu, s_high); /* Ascending order is permitted */
    uint64_t fs_high = spin_lock_irqsave(s_high);
    ok = ok && !can_acquire(cpu, s_low); /* Descending order or reentrancy rejected */
    spin_unlock_irqrestore(s_high, fs_high);
    spin_unlock_irqrestore(s_low, fs_low);

    ok = ok && cpu->lock_depth == 0;
    return ok;
}
