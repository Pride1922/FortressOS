#ifndef FORTRESS_PIC_H
#define FORTRESS_PIC_H

#include "types.h"

/* 8259 PIC Ports */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/* Mask all 16 legacy IRQs on master and slave PICs */
void pic_disable(void);

/* Read current mask registers from master and slave PICs */
uint8_t pic1_get_mask(void);
uint8_t pic2_get_mask(void);

#endif /* FORTRESS_PIC_H */
