#include "serial.h"

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

int serial_init(void) {
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
    outb(COM1_MCR, 0x1E);       /* Enable loopback mode */
    outb(COM1_DATA, 0xAE);      /* Send test byte */

    if (inb(COM1_DATA) != 0xAE) {
        return -1; /* Faulty serial port */
    }

    /* Set normal operation mode (disable loopback, enable RTS/DSR and OUT2) */
    outb(COM1_MCR, 0x0F);
    return 0;
}

static int serial_is_transmit_empty(void) {
    return (inb(COM1_LSR) & LSR_THRE);
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }

    /* Wait until the transmit buffer is empty */
    while (!serial_is_transmit_empty()) {
        __asm__ volatile("pause");
    }

    outb(COM1_DATA, (uint8_t)c);
}

void serial_puts(const char *str) {
    if (!str) return;
    while (*str) {
        serial_putc(*str++);
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
