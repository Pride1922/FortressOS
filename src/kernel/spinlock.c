#include "spinlock.h"
#include "serial.h"

/* IRQs are disabled before every access. NMI handlers must never take locks.
 * Replace this bootstrap-CPU storage with per-CPU storage BEFORE enabling APs. */
static spinlock_t *held[16];
static unsigned depth;

static bool can_acquire(spinlock_t *lock) {
    if (!lock || !lock->rank || depth == 16) return false;
    for (unsigned i = 0; i < depth; i++) {
        if (held[i] == lock || held[i]->rank >= lock->rank) return false;
    }
    return true;
}

static void fail(const char *reason, spinlock_t *lock) {
    serial_raw_puts("[FATAL] Lock discipline: ");
    serial_raw_puts(reason);
    if (lock && lock->name) serial_raw_puts(lock->name);
    serial_raw_puts("\n");
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

void spin_debug_acquire(spinlock_t *lock) {
    if (!can_acquire(lock)) fail("recursive/inverted acquisition: ", lock);
    held[depth++] = lock;
}

void spin_debug_release(spinlock_t *lock) {
    if (!depth || held[depth - 1] != lock) fail("out-of-order release: ", lock);
    held[--depth] = NULL;
}

void spin_debug_assert_unheld(void) {
    if (depth) fail("lock held across context switch", held[depth - 1]);
}

bool spin_debug_selftest(void) {
    spinlock_t a = SPINLOCK_RANKED(1, "test-a");
    spinlock_t b = SPINLOCK_RANKED(2, "test-b");
    uint64_t before, after;
    __asm__ volatile("pushfq; pop %0" : "=r"(before) :: "memory");
    uint64_t fa = spin_lock_irqsave(&a);
    bool ok = !can_acquire(&a) && can_acquire(&b);
    uint64_t fb = spin_lock_irqsave(&b);
    ok = ok && !can_acquire(&a) && !can_acquire(&b) && !(fb & 0x200);
    spin_unlock_irqrestore(&b, fb);
    __asm__ volatile("pushfq; pop %0" : "=r"(after) :: "memory");
    ok = ok && !(after & 0x200);
    spin_unlock_irqrestore(&a, fa);
    __asm__ volatile("pushfq; pop %0" : "=r"(after) :: "memory");
    return ok && ((before ^ after) & 0x200) == 0 && depth == 0;
}
