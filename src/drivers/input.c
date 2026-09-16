#include "input.h"
#include "input_buffer.h"
#include "keyboard.h"
#include "serial.h"
#include "ioapic.h"
#include "apic.h"
#include "thread.h"

#define KBD_VECTOR 0x31
#define UART_VECTOR 0x34
#define KBC_DATA 0x60
#define KBC_STATUS 0x64
#define KBC_LIMIT 100000
static input_buffer_t g_input;
static keyboard_decoder_t g_keyboard;
static bool g_input_ready;
static bool serial_last_cr;

/* Bootstrap CPU only: IRQ exclusion protects the buffer, including the gap
 * between scheduler predicate and dequeue. No input lock spans a switch. */
static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}
static void publish(char c) {
    if (c && input_buffer_push(&g_input, c)) sched_wake_all(&g_input);
}
static bool available(void *arg) {
    (void)arg;
    return g_input.count != 0;
}
int64_t input_read(void *buffer, size_t count) {
    if (!count) return 0;
    if (!g_input_ready) return -3; /* EBADF: stdin has no input source yet. */
    uint64_t flags = irq_save();
    sched_wait_until(&g_input, available, NULL);
    size_t n = input_buffer_read(&g_input, buffer, count);
    irq_restore(flags);
    return (int64_t)n;
}

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
        if (!(status & 0xe0)) publish(keyboard_decode(&g_keyboard, byte));
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
        if (status & 0x1e) continue; /* Drop overrun/parity/framing/break bytes. */
        if (c == '\n' && serial_last_cr) { serial_last_cr = false; continue; }
        serial_last_cr = c == '\r';
        if (c == '\r') c = '\n';
        if (c == 127) c = '\b';
        publish(c);
    }
}
bool input_init(const acpi_madt_info_t *madt) {
    uint64_t flags = irq_save();
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
    irq_restore(flags);
    serial_puts(keyboard ? "[INPUT] PS/2 set 2 -> set 1, IRQ1 ready (US layout)\n" :
                          "[WARN] PS/2 keyboard unavailable\n");
    serial_puts(uart ? "[INPUT] COM1 RX IRQ4 ready\n" : "[INPUT] COM1 RX unavailable\n");
    return g_input_ready;
}
