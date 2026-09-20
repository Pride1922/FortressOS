#include "serial.h"
#include "console.h"
#include "dmesg.h"
#include "string.h"

#define COM1_DATA          (COM1_PORT + 0)
#define COM1_IER           (COM1_PORT + 1) /* Interrupt Enable Register */
#define COM1_BAUD_LO       (COM1_PORT + 0) /* With DLAB = 1 */
#define COM1_BAUD_HI       (COM1_PORT + 1) /* With DLAB = 1 */
#define COM1_FCR           (COM1_PORT + 2) /* FIFO Control Register */
#define COM1_LCR           (COM1_PORT + 3) /* Line Control Register */
#define COM1_MCR           (COM1_PORT + 4) /* Modem Control Register */
#define COM1_LSR           (COM1_PORT + 5) /* Line Status Register */

#define LSR_THRE           0x20            /* Transmitter Holding Register Empty */
#define LSR_DATA_READY     0x01            /* Data Ready */
static bool serial_available;

int serial_init(void) {
    serial_available = false;
    /* Disable all UART interrupts */
    outb(COM1_IER, 0x00);

    /* Enable DLAB (Divisor Latch Access Bit) to set baud rate */
    outb(COM1_LCR, 0x80);

    /* Set divisor to 1 (115200 baud) */
    outb(COM1_BAUD_LO, 0x01);
    outb(COM1_BAUD_HI, 0x00);

    /* 8 bits, no parity, 1 stop bit (8N1) & clear DLAB */
    outb(COM1_LCR, 0x03);

    /* Enable FIFO, clear transmit/receive queues, 14-byte threshold */
    outb(COM1_FCR, 0xC7);

    /* Set RTS/DSR set, auxiliary output 2 (enables IRQs in hardware if configured) */
    outb(COM1_MCR, 0x0B);

    /* Perform loopback self-test */
    outb(COM1_MCR, 0x1E);       /* Enable loopback mode (disconnects external RX) */

    /* Drain any leftover bytes now that external RX is disconnected */
    for (int i = 0; i < 256 && (inb(COM1_LSR) & LSR_DATA_READY); i++) {
        (void)inb(COM1_DATA);
    }

    outb(COM1_DATA, 0xAE);      /* Send test byte */

    /* Bounded wait for loopback byte to arrive in receiver */
    for (int i = 0; i < 10000 && !(inb(COM1_LSR) & LSR_DATA_READY); i++) {
        __asm__ volatile("pause");
    }

    if (inb(COM1_DATA) != 0xAE) {
        outb(COM1_MCR, 0x0F);
        return -1; /* Faulty serial port */
    }

    /* Set normal operation mode (disable loopback, enable RTS/DSR and OUT2) */
    outb(COM1_MCR, 0x0F);
    serial_available = true;
    return 0;
}

bool serial_is_available(void) { return serial_available; }

static bool serial_wait_transmit(void) {
    if (!serial_available) return false;
    for (unsigned i = 0; i < 100000; i++) {
        if (inb(COM1_LSR) & LSR_THRE) return true;
        __asm__ volatile("pause");
    }
    serial_available = false;
    return false;
}

void serial_raw_putc(char c) {
    if (c == '\n') {
        if (!serial_wait_transmit()) return;
        outb(COM1_DATA, (uint8_t)'\r');
    }

    if (!serial_wait_transmit()) return;

    outb(COM1_DATA, (uint8_t)c);
}

void serial_raw_puts(const char *str) {
    if (!str) return;
    while (*str) {
        serial_raw_putc(*str++);
    }
}

void serial_raw_print_hex(uint64_t val) {
    serial_raw_puts("0x");
    const char hex_digits[] = "0123456789ABCDEF";
    bool leading_zero = true;

    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        if (nibble != 0 || i == 0) {
            leading_zero = false;
        }
        if (!leading_zero) {
            serial_raw_putc(hex_digits[nibble]);
        }
    }
}

void serial_raw_print_dec(uint64_t val) {
    if (val == 0) {
        serial_raw_putc('0');
        return;
    }

    char buf[32];
    int idx = 0;

    while (val > 0) {
        buf[idx++] = '0' + (val % 10);
        val /= 10;
    }

    for (int i = idx - 1; i >= 0; i--) {
        serial_raw_putc(buf[i]);
    }
}

void serial_putc(char c) {
    dmesg_append(c);
    if (console_is_initialized()) {
        console_putc(c);
    }
    serial_raw_putc(c);
}

void serial_puts(const char *str) {
    if (!str) return;

    dmesg_append_str(str, strlen(str));

    if (console_is_initialized()) {
        console_puts(str);
    }

    /* Emit to COM1 */
    while (*str) {
        serial_raw_putc(*str++);
    }
}

void serial_print_hex(uint64_t val) {
    serial_puts("0x");
    const char hex_digits[] = "0123456789ABCDEF";
    bool leading_zero = true;

    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        if (nibble != 0 || i == 0) {
            leading_zero = false;
        }
        if (!leading_zero) {
            serial_putc(hex_digits[nibble]);
        }
    }
}

void serial_print_dec(uint64_t val) {
    if (val == 0) {
        serial_putc('0');
        return;
    }

    char buf[32];
    int idx = 0;

    while (val > 0) {
        buf[idx++] = '0' + (val % 10);
        val /= 10;
    }

    for (int i = idx - 1; i >= 0; i--) {
        serial_putc(buf[i]);
    }
}
