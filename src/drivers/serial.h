#ifndef FORTRESS_SERIAL_H
#define FORTRESS_SERIAL_H

#include "types.h"

#define COM1_PORT 0x3F8

/* Port I/O Low-level Primitives */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* Serial Driver Public API */
int serial_init(void);
void serial_putc(char c);
void serial_puts(const char *str);
void serial_print_hex(uint64_t val);
void serial_print_dec(uint64_t val);

/* Raw Lockless UART API (for NMI, Double Fault, and emergency panics) */
void serial_raw_putc(char c);
void serial_raw_puts(const char *str);
void serial_raw_print_hex(uint64_t val);
void serial_raw_print_dec(uint64_t val);

#endif /* FORTRESS_SERIAL_H */
