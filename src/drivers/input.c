#include "input.h"
#include "input_buffer.h"
#include "keyboard.h"
#include "serial.h"
#include "ioapic.h"
#include "apic.h"
#include "thread.h"
#include "percpu.h"
#include "terminal.h"
#include "syscall_abi.h"
#include "process_table.h"
#include "vfs.h"
#include "spinlock.h"

#define KBD_VECTOR 0x31
#define UART_VECTOR 0x34
#define KBC_DATA 0x60
#define KBC_STATUS 0x64
#define KBC_LIMIT 100000
#define TERMINAL_EVENTS 16
typedef struct { process_group_ref_t target; unsigned signal; } terminal_event_t;
static struct terminal {
    uint64_t controlling_sid, fg_pgid;
    process_group_ref_t foreground;
    terminal_attrs_t attrs;
    input_buffer_t input;
    terminal_event_t events[TERMINAL_EVENTS];
    unsigned event_head, event_count;
    uint64_t reported_drops;
} g_terminal = {.attrs={.version=1, .input_flags=TERM_ISIG, .vintr=3, .vsusp=26}};
#define g_input (g_terminal.input)
static spinlock_t g_terminal_lock = SPINLOCK_RANKED(1, "terminal");
/* Read-only debugger metadata: queue moved inside the shared terminal. */
const void *const input_wait_channel_debug=&g_input;
#ifndef INPUT_HOST_TEST
static keyboard_decoder_t g_keyboard;
#endif
static bool g_input_ready;
static bool serial_last_cr;
static bool pending_wake;
static uint64_t input_ticks, observed_drops;
static unsigned timed_readers;
static uint32_t input_hz = 100;
static bool worker_started;

static bool on_bsp(void) {
#ifdef INPUT_HOST_TEST
    extern unsigned input_test_cpu;
    return input_test_cpu == 0;
#else
    return cpu_current()->id == 0;
#endif
}
/* Terminal and input buffer are protected by g_terminal_lock (Rank 1).
 * IRQ handlers and Ring 3 callers from all CPUs can access safely. */
static void ingress(const char *bytes, size_t count) {
    if (!count) return;
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    pending_wake = true;
    unsigned sig = 0;
    if (count == 1 && (g_terminal.attrs.input_flags & TERM_ISIG)) {
        if ((uint8_t)*bytes == g_terminal.attrs.vintr) sig = SIGINT;
        else if ((uint8_t)*bytes == g_terminal.attrs.vsusp) sig = SIGTSTP;
    }
    if (!sig) {
        (void)input_buffer_sequence(&g_input, bytes, count);
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return;
    }
    g_input.head = g_input.count = 0; /* flush ordinary input, including on overflow */
    observed_drops = g_input.dropped; /* deliberate flush acknowledges prior loss */
    if (!g_terminal.foreground.generation) {
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return; /* no owner yet */
    }
    for (unsigned i = 0; i < g_terminal.event_count; ++i) {
        terminal_event_t *e = &g_terminal.events[(g_terminal.event_head + i) % TERMINAL_EVENTS];
        if (e->signal == sig && e->target.slot == g_terminal.foreground.slot &&
            e->target.generation == g_terminal.foreground.generation) {
            ++g_terminal.attrs.signal_coalesced;
            spin_unlock_irqrestore(&g_terminal_lock, flags);
            return;
        }
    }
    if (g_terminal.event_count == TERMINAL_EVENTS) {
        ++g_terminal.attrs.signal_dropped;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return;
    }
    terminal_event_t *e = &g_terminal.events[(g_terminal.event_head + g_terminal.event_count) % TERMINAL_EVENTS];
    if (!process_group_try_retain(&g_terminal.foreground, &e->target)) {
        ++g_terminal.attrs.signal_dropped;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return;
    }
    e->signal = sig;
    ++g_terminal.event_count;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
}

static void serial_ingress(char c) {
    if (c == '\n' && serial_last_cr) { serial_last_cr = false; return; }
    serial_last_cr = (c == '\r');
    if (c == '\r') c = '\n';
    if (c == 127) c = '\b';
    ingress(&c, 1);
}

static bool events_ready(void *arg) {
    (void)arg;
    return g_terminal.event_count || g_terminal.attrs.signal_dropped != g_terminal.reported_drops;
}

/* Owns the popped reference until publication finishes. No scheduler or console
 * operation under the process lock. Also used by the host adapter. */
static bool drain_event(void) {
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    if (!g_terminal.event_count) {
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return false;
    }
    terminal_event_t e = g_terminal.events[g_terminal.event_head];
    g_terminal.events[g_terminal.event_head] = (terminal_event_t){0};
    g_terminal.event_head = (g_terminal.event_head + 1) % TERMINAL_EVENTS;
    --g_terminal.event_count;
    spin_unlock_irqrestore(&g_terminal_lock, flags);

    (void)process_group_signal(&e.target, e.signal);
    (void)process_group_release_ref(&e.target);
    return true;
}

static void signal_worker(void *arg) {
    (void)arg;
    for (;;) {
        sched_wait_until(&g_terminal, events_ready, NULL);
        for (unsigned i = 0; i < TERMINAL_EVENTS && drain_event(); ++i) {}
        uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
        bool report = (g_terminal.reported_drops != g_terminal.attrs.signal_dropped);
        g_terminal.reported_drops = g_terminal.attrs.signal_dropped;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        if (report) serial_puts("[INPUT] terminal signal queue overflow; inspect TERMATTR counters\n");
        thread_yield();
    }
}

int input_terminal_bootstrap(uint64_t pid) {
    uint64_t sid = process_record_session(pid);
    int64_t pgid = process_record_group(pid);
    process_group_ref_t ref = {0};
    int result = SYSCALL_EPERM;
    if (sid == pid && pgid == (int64_t)pid) {
        result = process_group_acquire(sid, pid, &ref);
    }
    if (!result) {
        uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
        process_group_ref_t old = g_terminal.foreground;
        g_terminal.foreground = ref;
        g_terminal.controlling_sid = sid;
        g_terminal.fg_pgid = pid;
        g_terminal.attrs.input_flags = TERM_ISIG;
        g_terminal.attrs.vintr = 3;
        g_terminal.attrs.vsusp = 26;
        g_input.head = g_input.count = 0;
        serial_last_cr = false;
        observed_drops = g_input.dropped;
        pending_wake = true;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        if (old.generation) (void)process_group_release_ref(&old);
    }
    return result;
}

static int terminal_caller(uint64_t fd) {
    tcb_t *t = thread_current();
    if (!t || fd >= MAX_PROCESS_FDS) return SYSCALL_EBADF;
    file_t *f = fd_get(t, (int)fd);
    if (!f) return SYSCALL_EBADF;
    if (f->node != vfs_get_terminal_node()) return SYSCALL_ENOTTY;
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    uint64_t sid = g_terminal.controlling_sid;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    if (!t->is_user || t->sid != sid) return SYSCALL_ENOTTY;
    return 0;
}

static int access_check(bool mutation) {
    tcb_t *t = thread_current();
    if (!t || !t->is_user) return SYSCALL_ENOTTY;
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    uint64_t csid = g_terminal.controlling_sid;
    uint64_t fg = g_terminal.fg_pgid;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    if (t->sid != csid) return SYSCALL_ENOTTY;
    while (t->pgid != fg) {
        if (t->sid != csid) return SYSCALL_ENOTTY;
        unsigned sig = mutation ? SIGTTOU : SIGTTIN;
        uint64_t unavailable = __atomic_load_n(&t->signals.blocked_mask, __ATOMIC_ACQUIRE) |
                               __atomic_load_n(&t->signals.ignored_mask, __ATOMIC_ACQUIRE);
        if (unavailable & SIGNAL_BIT(sig)) return mutation ? 0 : SYSCALL_EIO;
        int64_t result = process_signal_send_kernel(t->tid, 0, sig);
        if (result) return (int)result;
        if (process_signal_interrupt()) return SYSCALL_EINTR;
        /* Default stop resumes here. Recheck ownership and disposition */
        flags = spin_lock_irqsave(&g_terminal_lock);
        csid = g_terminal.controlling_sid;
        fg = g_terminal.fg_pgid;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
    }
    return 0;
}

int input_control_check(void) {
    return access_check(true);
}

int64_t input_tcgetpgrp(uint64_t fd) {
    int error = terminal_caller(fd);
    if (error) return error;
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    int64_t result = (int64_t)g_terminal.fg_pgid;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    return result;
}

int64_t input_tcsetpgrp(uint64_t fd, uint64_t pgid) {
    int error = terminal_caller(fd);
    if (!error && (!pgid || pgid > INT64_MAX)) error = SYSCALL_EINVAL;
    if (!error) error = access_check(true);
    if (error) return error;

    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    uint64_t csid = g_terminal.controlling_sid;
    spin_unlock_irqrestore(&g_terminal_lock, flags);

    process_group_ref_t ref = {0};
    error = process_group_acquire(csid, pgid, &ref);
    if (!error) {
        flags = spin_lock_irqsave(&g_terminal_lock);
        process_group_ref_t old = g_terminal.foreground;
        g_terminal.foreground = ref;
        g_terminal.fg_pgid = pgid;
        pending_wake = true;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        if (old.generation) (void)process_group_release_ref(&old);
        sched_wake_all(&g_input);
    }
    return error;
}

int input_termattr(uint64_t fd, uint64_t op, terminal_attrs_t *attrs) {
    int error = terminal_caller(fd);
    if (!error && op > TERM_SET) error = SYSCALL_EINVAL;
    if (!error && op == TERM_SET) {
        if (attrs->version != 1 || (attrs->input_flags & ~TERM_ISIG) ||
            !attrs->vintr || attrs->vintr > 127 || !attrs->vsusp || attrs->vsusp > 127 ||
            attrs->vintr == attrs->vsusp) error = SYSCALL_EINVAL;
        for (unsigned i = 0; i < sizeof(attrs->reserved); ++i)
            if (attrs->reserved[i]) error = SYSCALL_EINVAL;
        if (!error) error = access_check(true);
        if (!error) {
            uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
            g_terminal.attrs.input_flags = attrs->input_flags;
            g_terminal.attrs.vintr = attrs->vintr;
            g_terminal.attrs.vsusp = attrs->vsusp;
            spin_unlock_irqrestore(&g_terminal_lock, flags);
        }
    }
    if (!error && op == TERM_GET) {
        uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
        *attrs = g_terminal.attrs;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
    }
    return error;
}

void input_timer_tick(uint32_t hz) {
    if (!on_bsp()) return;
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    input_ticks++;
    if (hz) input_hz = hz;
    bool wake_term = (worker_started && events_ready(NULL));
    bool wake_input = (pending_wake || timed_readers);
    if (wake_input) pending_wake = false;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    if (wake_term) sched_wake_all(&g_terminal);
    if (wake_input) sched_wake_all(&g_input);
}

/* Metadata may be queried from another CPU; queue ownership remains BSP-only. */
uint64_t input_dropped(void) { return __atomic_load_n(&g_input.dropped, __ATOMIC_RELAXED); }
typedef struct { uint64_t deadline; bool timed; } input_wait_t;
static bool available(void *arg) {
    input_wait_t *w = arg;
    tcb_t *t = thread_current();
    return !t || t->sid != g_terminal.controlling_sid || t->pgid != g_terminal.fg_pgid ||
           g_input.count != 0 || g_input.dropped != observed_drops ||
           (w->timed && input_ticks >= w->deadline);
}
int64_t input_read(void *buffer, size_t count) {
    return input_read_timeout(buffer, count, -1);
}
int64_t input_read_timeout(void *buffer, size_t count, int64_t timeout_ms) {
    if (!count) return 0;
    if (timeout_ms < -1 || timeout_ms > 1000) return SYSCALL_EINVAL;
    if (!g_input_ready) return -3; /* EBADF: stdin has no input source yet. */
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    input_wait_t w = {input_ticks + ((uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * input_hz + 999) / 1000, timeout_ms >= 0};
    if (w.timed) timed_readers++;
    spin_unlock_irqrestore(&g_terminal_lock, flags);

    do {
        int error = access_check(false);
        if (error) {
            if (w.timed) {
                flags = spin_lock_irqsave(&g_terminal_lock);
                timed_readers--;
                spin_unlock_irqrestore(&g_terminal_lock, flags);
            }
            return error;
        }
        sched_wait_until(&g_input, available, &w);
        if (process_signal_interrupt()) {
            if (w.timed) {
                flags = spin_lock_irqsave(&g_terminal_lock);
                timed_readers--;
                spin_unlock_irqrestore(&g_terminal_lock, flags);
            }
            return SYSCALL_EINTR;
        }
        error = access_check(false);
        if (error) {
            if (w.timed) {
                flags = spin_lock_irqsave(&g_terminal_lock);
                timed_readers--;
                spin_unlock_irqrestore(&g_terminal_lock, flags);
            }
            return error;
        }
    } while (!available(&w));

    flags = spin_lock_irqsave(&g_terminal_lock);
    if (w.timed) timed_readers--;
    if (g_input.dropped != observed_drops) {
        observed_drops = g_input.dropped;
        g_input.head = g_input.count = 0;
        spin_unlock_irqrestore(&g_terminal_lock, flags);
        return INPUT_LOST;
    }
    size_t n = input_buffer_read(&g_input, buffer, count);
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    return (int64_t)n;
}

#ifndef INPUT_HOST_TEST
static bool kbc_write(uint16_t port, uint8_t value) {
    for (unsigned i = 0; i < KBC_LIMIT; i++) {
        uint8_t status = inb(KBC_STATUS);
        if (status == 0xff) return false;
        if (!(status & 2)) { outb(port, value); return true; }
        __asm__ volatile("pause");
    }
    return false;
}
static bool kbc_read(uint8_t *value) {
    for (unsigned i = 0; i < KBC_LIMIT; i++) {
        uint8_t status = inb(KBC_STATUS);
        if (status == 0xff) return false;
        if (status & 1) {
            uint8_t byte = inb(KBC_DATA);
            if (status & 0xe0) continue; /* AUX, timeout or parity error. */
            *value = byte;
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}
static bool keyboard_command(uint8_t byte) {
    for (unsigned retry = 0; retry < 3; retry++) {
        uint8_t reply;
        if (!kbc_write(KBC_DATA, byte) || !kbc_read(&reply)) return false;
        if (reply == 0xfa) return true;
        if (reply != 0xfe) return false;
    }
    return false;
}
static void keyboard_irq(interrupt_frame_t *frame) {
    (void)frame;
    for (unsigned n = 0; n < 32; n++) {
        uint8_t status = inb(KBC_STATUS);
        if (status == 0xff || !(status & 1)) break;
        uint8_t byte = inb(KBC_DATA);
        if (!(status & 0xe0)) {
            char bytes[4];
            size_t count = keyboard_decode_bytes(&g_keyboard, byte, bytes);
            ingress(bytes,count);
        } else if (status & 0xc0) {
            __atomic_fetch_add(&g_input.dropped, 1, __ATOMIC_RELAXED); pending_wake = true;
        }
    }
    /* Dispatcher owns EOI. No allocations, logging or switching in this ISR. */
}
static bool keyboard_init(const acpi_madt_info_t *madt, uint8_t apic_id) {
    uint8_t config, set;
    if (!kbc_write(KBC_STATUS, 0xad) || !kbc_write(KBC_STATUS, 0xa7)) return false;
    for (unsigned i = 0; i < 32 && (inb(KBC_STATUS) & 1); i++) (void)inb(KBC_DATA);
    if (!kbc_write(KBC_STATUS, 0x20) || !kbc_read(&config)) return false;
    config = (config & ~0x53u) | 0x20; /* No translation/IRQs, first port enabled. */
    if (!kbc_write(KBC_STATUS, 0x60) || !kbc_write(KBC_DATA, config) ||
        !kbc_write(KBC_STATUS, 0xae)) return false;
    /* Explicitly select and query set 2, then ask 8042 to translate to set 1.
     * Avoid controller reset: laptop EC may manage more than the keyboard. */
    if (!keyboard_command(0xf5) || !keyboard_command(0xf0) || !keyboard_command(2) ||
        !keyboard_command(0xf0) || !keyboard_command(0) || !kbc_read(&set) || set != 2)
        return false;
    idt_register_hardware_handler(KBD_VECTOR, keyboard_irq);
    if (!ioapic_route_isa(madt, 1, KBD_VECTOR, apic_id) || !keyboard_command(0xf4)) return false;
    return kbc_write(KBC_STATUS, 0x60) && kbc_write(KBC_DATA, config | 0x41);
}

static void serial_irq(interrupt_frame_t *frame) {
    (void)frame;
    for (unsigned n = 0; n < 64; n++) {
        uint8_t status = inb(COM1_PORT + 5);
        if (status == 0xff || !(status & 1)) break;
        char c = (char)inb(COM1_PORT);
        if (status & 0x1e) { /* A damaged stream must never become a command. */
            __atomic_fetch_add(&g_input.dropped, 1, __ATOMIC_RELAXED); pending_wake = true; continue;
        }
        serial_ingress(c);
    }
}
bool input_init(const acpi_madt_info_t *madt) {
    if (!worker_started) {
        if (!thread_create_on_cpu(0,"terminal-signals",signal_worker,NULL)) {
            return false;
        }
        worker_started=true;
    }
    uint64_t flags = spin_lock_irqsave(&g_terminal_lock);
    uint8_t apic_id = lapic_read(APIC_REG_ID) >> 24;
    bool keyboard = keyboard_init(madt, apic_id);
    if (!keyboard) (void)kbc_write(KBC_STATUS, 0xad);
    bool uart = false;
    if (serial_is_available()) {
        idt_register_hardware_handler(UART_VECTOR, serial_irq);
        uart = ioapic_route_isa(madt, 4, UART_VECTOR, apic_id);
        if (uart) {
            outb(COM1_PORT + 2, 0x07); /* FIFO enabled, one-byte RX threshold. */
            outb(COM1_PORT + 1, 0x01); /* RX interrupt only. */
        }
    }
    g_input_ready = keyboard || uart;
    spin_unlock_irqrestore(&g_terminal_lock, flags);
    serial_puts(keyboard ? "[INPUT] PS/2 set 2 -> set 1, IRQ1 ready (US layout)\n" :
                          "[WARN] PS/2 keyboard unavailable\n");
    serial_puts(uart ? "[INPUT] COM1 RX IRQ4 ready\n" : "[INPUT] COM1 RX unavailable\n");
    return g_input_ready;
}
#endif
