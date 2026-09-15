#include "pic.h"
#include "serial.h"

void pic_disable(void) {
    /* Mask all interrupts on Master PIC (0x21) and Slave PIC (0xA1) */
    outb(PIC1_DATA, 0xFF);
    io_wait();
    outb(PIC2_DATA, 0xFF);
    io_wait();
    serial_puts("[ OK ] 8259 PIC disabled (all 16 IRQ lines masked)\n");
}

uint8_t pic1_get_mask(void) {
    return inb(PIC1_DATA);
}

uint8_t pic2_get_mask(void) {
    return inb(PIC2_DATA);
}
